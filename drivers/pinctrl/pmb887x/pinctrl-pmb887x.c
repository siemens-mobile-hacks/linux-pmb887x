// SPDX-License-Identifier: GPL-2.0
#include <linux/clk.h>
#include <linux/gpio/driver.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <linux/seq_file.h>
#include <linux/slab.h>

#include <linux/pinctrl/consumer.h>
#include <linux/pinctrl/machine.h>
#include <linux/pinctrl/pinconf-generic.h>
#include <linux/pinctrl/pinconf.h>
#include <linux/pinctrl/pinctrl.h>
#include <linux/pinctrl/pinmux.h>

#include "../core.h"
#include "../pinconf.h"
#include "../pinmux.h"
#include "../pinctrl-utils.h"
#include "pinctrl-pmb887x.h"

#define	GPIO_PIN(n)			(0x20 + ((n) * 0x4))
#define	GPIO_IS				GENMASK(2, 0)
#define	GPIO_IS_SHIFT		0
#define	GPIO_OS				GENMASK(6, 4)
#define	GPIO_OS_SHIFT		4
#define	GPIO_PS				BIT(8)
#define	GPIO_PS_ALT			0x0
#define	GPIO_PS_MANUAL		0x100
#define	GPIO_DATA			BIT(9)
#define	GPIO_DATA_LOW		0x0
#define	GPIO_DATA_HIGH		0x200
#define	GPIO_DIR			BIT(10)
#define	GPIO_DIR_IN			0x0
#define	GPIO_DIR_OUT		0x400
#define	GPIO_PPEN			BIT(12)
#define	GPIO_PPEN_PUSHPULL	0x0
#define	GPIO_PPEN_OPENDRAIN	0x1000
#define	GPIO_PDPU			GENMASK(14, 13)
#define	GPIO_PDPU_SHIFT		13
#define	GPIO_PDPU_NONE		0x0
#define	GPIO_PDPU_PULLUP	0x2000
#define	GPIO_PDPU_PULLDOWN	0x4000
#define	GPIO_ENAQ			BIT(15)
#define	GPIO_ENAQ_OFF		0x0
#define	GPIO_ENAQ_ON		0x8000

struct pmb887x_pinctrl {
	struct device *dev;
	struct pinctrl_dev *pctl_dev;
	struct pinctrl_desc pctl_desc;
	unsigned ngroups;
	void __iomem *base;
	struct gpio_chip chip;
	struct pinctrl_gpio_range range;
	const struct pmb887x_desc_pin *pins;
	u8 *pins_alt_func;
	u32 npins;
	u32 nfuncs;
};

/* GPIO functions */
static void pmb887x_gpio_free(struct gpio_chip *chip, unsigned pin) {
	struct pmb887x_pinctrl *pctl = gpiochip_get_data(chip);
	u32 reg = readl_relaxed(pctl->base + GPIO_PIN(pin)) & ~GPIO_ENAQ;
	writel_relaxed(reg | GPIO_ENAQ_ON, pctl->base + GPIO_PIN(pin));
	pinctrl_gpio_free(chip, pin);
}

static int pmb887x_gpio_get(struct gpio_chip *chip, unsigned pin) {
	struct pmb887x_pinctrl *pctl = gpiochip_get_data(chip);
	u32 reg = readl_relaxed(pctl->base + GPIO_PIN(pin));
	return (reg & GPIO_DATA) == GPIO_DATA_HIGH;
}

static int pmb887x_gpio_set(struct gpio_chip *chip, unsigned pin, int value) {
	struct pmb887x_pinctrl *pctl = gpiochip_get_data(chip);
	
	u32 reg = readl_relaxed(pctl->base + GPIO_PIN(pin)) & ~GPIO_DATA;
	reg |= (value ? GPIO_DATA_HIGH : GPIO_DATA_LOW);
	writel_relaxed(reg, pctl->base + GPIO_PIN(pin));
	return 0;
}

static int pmb887x_gpio_direction_input(struct gpio_chip *chip, unsigned pin) {
	struct pmb887x_pinctrl *pctl = gpiochip_get_data(chip);
	u32 reg = GPIO_DIR_IN | GPIO_PS_MANUAL | GPIO_ENAQ_OFF;
	
	if (pctl->pins_alt_func[pin]) {
		dev_err(pctl->dev, "pin %d in the alt mode, can't change direction.\n", pin);
		return -EINVAL;
	}
	
	writel_relaxed(reg, pctl->base + GPIO_PIN(pin));
	return 0;
}

static int pmb887x_gpio_direction_output(struct gpio_chip *chip, unsigned pin, int value) {
	struct pmb887x_pinctrl *pctl = gpiochip_get_data(chip);
	u32 reg = GPIO_DIR_OUT | GPIO_PS_MANUAL | GPIO_ENAQ_OFF | (value ? GPIO_DATA_HIGH : GPIO_DATA_LOW);
	
	if (pctl->pins_alt_func[pin]) {
		dev_err(pctl->dev, "pin %d in the alt mode, can't change direction.\n", pin);
		return -EINVAL;
	}
	
	writel_relaxed(reg, pctl->base + GPIO_PIN(pin));
	return 0;
}

static int pmb887x_gpio_get_direction(struct gpio_chip *chip, unsigned int pin) {
	struct pmb887x_pinctrl *pctl = gpiochip_get_data(chip);
	u32 reg = readl_relaxed(pctl->base + GPIO_PIN(pin));
	return ((reg & GPIO_DIR) == GPIO_DIR_IN ? GPIO_LINE_DIRECTION_IN : GPIO_LINE_DIRECTION_OUT);
}

static const struct gpio_chip pmb887x_gpio_template = {
	.get				= pmb887x_gpio_get,
	.set				= pmb887x_gpio_set,
	.direction_input	= pmb887x_gpio_direction_input,
	.direction_output	= pmb887x_gpio_direction_output,
	.get_direction		= pmb887x_gpio_get_direction,
	.request			= gpiochip_generic_request,
	.free				= pmb887x_gpio_free,
};

static const struct pinctrl_ops pmb887x_pctl_ops = {
	.get_groups_count	= pinctrl_generic_get_group_count,
	.get_group_name		= pinctrl_generic_get_group_name,
	.get_group_pins		= pinctrl_generic_get_group_pins,
	.dt_node_to_map		= pinconf_generic_dt_node_to_map_all,
	.dt_free_map		= pinconf_generic_dt_free_map,
};

/* Pinmux functions */
static int pmb887x_pmx_request(struct pinctrl_dev *pctldev, unsigned int pin) {
	struct pmb887x_pinctrl *pctl = pinctrl_dev_get_drvdata(pctldev);
	struct pinctrl_gpio_range *range;
	
	range = pinctrl_find_gpio_range_from_pin_nolock(pctldev, pin);
	if (!range) {
		dev_err(pctl->dev, "No gpio range defined.\n");
		return -EINVAL;
	}
	
	if (!gpiochip_line_is_valid(range->gc, pin)) {
		dev_warn(pctl->dev, "Can't access gpio %d\n", pin);
		return -EACCES;
	}
	
	return 0;
}

static void pmb887x_pmx_free(struct pinctrl_dev *pctldev, struct pinctrl_gpio_range *range, unsigned int pin) {
	struct pmb887x_pinctrl *pctl = pinctrl_dev_get_drvdata(pctldev);
	u32 reg = readl_relaxed(pctl->base + GPIO_PIN(pin)) & ~GPIO_ENAQ;
	reg |= GPIO_PS_MANUAL | GPIO_DIR_IN;
	writel_relaxed(reg | GPIO_ENAQ_ON, pctl->base + GPIO_PIN(pin));
	
	pctl->pins_alt_func[pin] = 0;
}

static const struct pmb887x_desc_mux *pmb887x_pmx_find_mux(struct pmb887x_pinctrl *pctl, int pin, const char *name) {
	int i;
	for (i = 0; i < PMB887X_CONFIG_NUM; i++) {
		const struct pmb887x_desc_mux *mux = &pctl->pins[pin].functions[i];
		if (mux->name && strcmp(mux->name, name) == 0)
			return mux;
	}
	return NULL;
}

static int pmb887x_pmx_set_mux(struct pinctrl_dev *pctldev, unsigned int func_select, unsigned int group_select) {
	struct pmb887x_pinctrl *pctl = pinctrl_dev_get_drvdata(pctldev);
	const struct function_desc *function;
	struct group_desc *group;
	const char *group_name;
	const char *function_name;
	int i;
	
	group = pinctrl_generic_get_group(pctldev, group_select);
	if (!group || !group->grp.npins)
		return -EINVAL;
	
	function = pinmux_generic_get_function(pctldev, func_select);
	if (!function)
		return -EINVAL;
	
	for (i = 0; i < group->grp.npins; i++) {
		u32 reg;

		function_name = pinmux_generic_get_function_name(pctldev, func_select);

		const struct pmb887x_desc_mux *mux = pmb887x_pmx_find_mux(pctl, group->grp.pins[i], function_name);
		if (!mux) {
			group_name = pinctrl_generic_get_group_name(pctldev, group_select);
			dev_err(pctl->dev, "Function %s not found for pin %d! (%s)\n", function_name, group->grp.pins[i], group_name);
			return -ENOENT;
		}
		
		pctl->pins_alt_func[group->grp.pins[i]] = (mux->is << 8) | mux->os;
		
		if (mux->is || mux->os) {
			reg = (mux->is << GPIO_IS_SHIFT) | (mux->os << GPIO_OS_SHIFT) | GPIO_PS_ALT;
		} else {
			reg = GPIO_PS_MANUAL | GPIO_DIR_IN | GPIO_ENAQ_ON;
		}
		writel_relaxed(reg, pctl->base + GPIO_PIN(group->grp.pins[i]));
	}
	
	return 0;
}

static const struct pinmux_ops pmb887x_pmx_ops = {
	.get_functions_count	= pinmux_generic_get_function_count,
	.get_function_name		= pinmux_generic_get_function_name,
	.get_function_groups	= pinmux_generic_get_function_groups,
	.set_mux				= pmb887x_pmx_set_mux,
	.request				= pmb887x_pmx_request,
	.gpio_disable_free		= pmb887x_pmx_free,
};

/* Pinctrl conf */
static int pmb887x_pconf_get(struct pinctrl_dev *pctldev, unsigned int pin, unsigned long *config) {
	struct pmb887x_pinctrl *pctl = pinctrl_dev_get_drvdata(pctldev);
	enum pin_config_param param = pinconf_to_config_param(*config);
	u32 arg, reg;
	
	reg = readl_relaxed(pctl->base + GPIO_PIN(pin));
	
	switch (param) {
		case PIN_CONFIG_BIAS_HIGH_IMPEDANCE:
			arg = (reg & GPIO_ENAQ) == GPIO_ENAQ_ON;
		break;
		
		case PIN_CONFIG_BIAS_PULL_DOWN:
			arg = (reg & GPIO_PDPU) == GPIO_PDPU_PULLDOWN;
		break;
		
		case PIN_CONFIG_BIAS_PULL_UP:
			arg = (reg & GPIO_PDPU) == GPIO_PDPU_PULLUP;
		break;
		
		case PIN_CONFIG_DRIVE_OPEN_DRAIN:
			arg = (reg & GPIO_PPEN) == GPIO_PPEN_OPENDRAIN;
		break;
		
		case PIN_CONFIG_DRIVE_PUSH_PULL:
			arg = (reg & GPIO_PPEN) == GPIO_PPEN_PUSHPULL;
		break;
		
		default:
			return -ENOTSUPP;
	}
	
	*config = pinconf_to_config_packed(param, arg);
	
	return 0;
}

static int pmb887x_pconf_set(struct pinctrl_dev *pctldev, unsigned int pin, unsigned long *configs, unsigned int num_configs) {
	struct pmb887x_pinctrl *pctl = pinctrl_dev_get_drvdata(pctldev);
	enum pin_config_param param;
	unsigned int pinconf;
	u32 clear_mask = 0, set_mask = 0, reg, arg;
	
	for (pinconf = 0; pinconf < num_configs; pinconf++) {
		param = pinconf_to_config_param(configs[pinconf]);
		arg = pinconf_to_config_argument(configs[pinconf]);
		
		switch (param) {
			case PIN_CONFIG_BIAS_HIGH_IMPEDANCE:
				clear_mask |= GPIO_ENAQ;
				set_mask &= ~GPIO_ENAQ;
				set_mask |= GPIO_ENAQ_ON;
			break;
			
			case PIN_CONFIG_BIAS_PULL_DOWN:
				clear_mask |= GPIO_PDPU;
				set_mask &= ~GPIO_PDPU;
				set_mask |= GPIO_PDPU_PULLDOWN;
			break;
			
			case PIN_CONFIG_BIAS_PULL_UP:
				clear_mask |= GPIO_PDPU;
				set_mask &= ~GPIO_PDPU;
				set_mask |= GPIO_PDPU_PULLUP;
			break;
			
			case PIN_CONFIG_BIAS_DISABLE:
				clear_mask |= GPIO_PDPU;
				set_mask &= ~GPIO_PDPU;
				set_mask |= GPIO_PDPU_NONE;
			break;
			
			case PIN_CONFIG_DRIVE_OPEN_DRAIN:
				clear_mask |= GPIO_PPEN;
				set_mask &= ~GPIO_PPEN;
				set_mask |= GPIO_PPEN_OPENDRAIN;
			break;
			
			case PIN_CONFIG_DRIVE_PUSH_PULL:
				clear_mask |= GPIO_PPEN;
				set_mask &= ~GPIO_PPEN;
				set_mask |= GPIO_PPEN_PUSHPULL;
			break;
			
		case PIN_CONFIG_OUTPUT_ENABLE:
			clear_mask |= GPIO_DATA | GPIO_DIR;
				set_mask &= ~(GPIO_DATA | GPIO_DIR);
				set_mask |= GPIO_DIR_OUT | (arg ? GPIO_DATA_HIGH : GPIO_DATA_LOW);
			break;
			
			default:
				return -ENOTSUPP;
		}
	}
	
	reg = readl_relaxed(pctl->base + GPIO_PIN(pin)) & ~clear_mask;
	writel_relaxed(reg | set_mask, pctl->base + GPIO_PIN(pin));
	
	return 0;
}

static const struct pinconf_ops pmb887x_pconf_ops = {
	.is_generic				= true,
	.pin_config_set			= pmb887x_pconf_set,
	.pin_config_get			= pmb887x_pconf_get,
};

static int pmb887x_gpiolib_register(struct pmb887x_pinctrl *pctl) {
	int err;
	
	pctl->chip = pmb887x_gpio_template;
	pctl->chip.base = -1;
	pctl->chip.ngpio = pctl->npins;
	pctl->chip.parent = pctl->dev;
	pctl->chip.owner = THIS_MODULE;
	pctl->chip.label = dev_name(pctl->dev);
	
	pctl->range.name = dev_name(pctl->dev);
	pctl->range.id = 0;
	pctl->range.pin_base = 0;
	pctl->range.base = 0;
	pctl->range.npins = pctl->npins;
	pctl->range.gc = &pctl->chip;
	
	pinctrl_add_gpio_range(pctl->pctl_dev, &pctl->range);
	
	err = gpiochip_add_data(&pctl->chip, pctl);
	if (err) {
		dev_err(pctl->dev, "Failed to add gpiochip!\n");
		return err;
	}
	
	return 0;
}

static int pmb887x_build_groups(struct pmb887x_pinctrl *pctl) {
	struct group_desc *groups;
	int i;
	
	pctl->ngroups = pctl->npins;
	groups = devm_kcalloc(pctl->dev, pctl->ngroups, sizeof(*groups), GFP_KERNEL);
	if (!groups)
		return -ENOMEM;

	for (i = 0; i < pctl->ngroups; i++) {
		struct group_desc *group = &groups[i];
		const struct pinctrl_pin_desc *pin_info = &pctl->pins[i].pin;
		group->grp.name = pin_info->name;
		group->grp.pins = &pin_info->number;
		group->data = (void *) &pctl->pins[i].functions;
		pinctrl_generic_add_group(pctl->pctl_dev, group->grp.name,
					  group->grp.pins, 1, NULL);
	}
	
	return 0;
}

static int pmb887x_add_functions(struct pmb887x_pinctrl *pctl, struct pinfunction *funcs) {
	unsigned int i;
	
	/* Assign the groups for each function */
	for (i = 0; i < pctl->nfuncs; i++) {
		struct pinfunction *func = &funcs[i];
		const char **group_names;
		unsigned int grp_idx = 0;
		int j;
		
		group_names = devm_kcalloc(pctl->dev, func->ngroups, sizeof(*group_names), GFP_KERNEL);
		if (!group_names)
			return -ENOMEM;
		
		for (j = 0; j < pctl->npins; j++) {
			const struct pmb887x_desc_pin *pin_info = &pctl->pins[j];
			int k;
			
			for (k = 0; k < PMB887X_CONFIG_NUM; k++) {
				const struct pmb887x_desc_mux *pin_mux = &pin_info->functions[k];
				if (pin_mux->name && strcmp(pin_mux->name, func->name) == 0)
					group_names[grp_idx++] = pin_info->pin.name;
			}
		}
		
		func->groups = group_names;
	}
	
	/* Add all functions */
	for (i = 0; i < pctl->nfuncs; i++)
		pinmux_generic_add_pinfunction(pctl->pctl_dev, &funcs[i], NULL);
	
	return 0;
}

static int pmb887x_build_functions(struct pmb887x_pinctrl *pctl) {
	struct pinfunction *funcs, *new_funcs;
	int i;
	
	/*
	 * Allocate maximum possible number of functions. Assume every pin
	 * being part of 8 (hw maximum) globally unique muxes.
	 */
	pctl->nfuncs = 0;
	funcs = devm_kcalloc(pctl->dev, pctl->npins * PMB887X_CONFIG_NUM,
			     sizeof(*funcs), GFP_KERNEL);
	if (!funcs)
		return -ENOMEM;
	
	/* Setup 1 function for each unique mux */
	for (i = 0; i < pctl->npins; i++) {
		const struct pmb887x_desc_pin *pin_info = &pctl->pins[i];
		int k;
		
		for (k = 0; k < PMB887X_CONFIG_NUM; k++) {
			const struct pmb887x_desc_mux *pin_mux = &pin_info->functions[k];
			struct pinfunction *func;
			
			if (!pin_mux->name)
				continue;
			
			/* Check if we already have function for this mux */
			for (func = funcs; func->name; func++) {
				if (!strcmp(pin_mux->name, func->name)) {
					func->ngroups++;
					break;
				}
			}
			
			/* Setup new function for this mux we didn't see before */
			if (!func->name) {
				func->name = pin_mux->name;
				func->ngroups = 1;
				pctl->nfuncs++;
			}
		}
	}
	
	/* Reallocate memory based on actual number of functions */
	new_funcs = devm_krealloc_array(pctl->dev, funcs, pctl->nfuncs,
					sizeof(*new_funcs), GFP_KERNEL);
	if (!new_funcs)
		return -ENOMEM;
	
	return pmb887x_add_functions(pctl, new_funcs);
}

int pmb887x_pctl_probe(struct platform_device *pdev) {
	const struct pmb887x_pinctrl_match_data *match_data;
	struct device *dev = &pdev->dev;
	struct pmb887x_pinctrl *pctl;
	struct pinctrl_pin_desc *pins;
	int i, ret;
	
	match_data = device_get_match_data(dev);
	if (!match_data)
		return -EINVAL;
	
	pctl = devm_kzalloc(dev, sizeof(*pctl), GFP_KERNEL);
	if (!pctl)
		return -ENOMEM;
	
	platform_set_drvdata(pdev, pctl);
	
	pctl->base = of_iomap(dev->of_node, 0);
	if (!pctl->base) {
		dev_err(dev, "can't get iomap for gpio!\n");
		return -EINVAL;
	}
	
	pctl->dev = dev;
	pctl->pins = match_data->pins;
	pctl->npins = match_data->npins;
	
	/* Build pins list */
	pins = devm_kcalloc(&pdev->dev, pctl->npins, sizeof(*pins), GFP_KERNEL);
	if (!pins)
		return -ENOMEM;
	
	pctl->pins_alt_func = devm_kcalloc(&pdev->dev, pctl->npins, sizeof(*pctl->pins_alt_func), GFP_KERNEL);
	if (!pctl->pins_alt_func)
		return -ENOMEM;
	
	for (i = 0; i < pctl->npins; i++)
		pins[i] = pctl->pins[i].pin;
	
	/* Register pinctrl */
	pctl->pctl_desc.name = dev_name(&pdev->dev);
	pctl->pctl_desc.owner = THIS_MODULE;
	pctl->pctl_desc.pins = pins;
	pctl->pctl_desc.npins = pctl->npins;
	pctl->pctl_desc.link_consumers = true;
	pctl->pctl_desc.confops = &pmb887x_pconf_ops;
	pctl->pctl_desc.pctlops = &pmb887x_pctl_ops;
	pctl->pctl_desc.pmxops = &pmb887x_pmx_ops;
	pctl->dev = &pdev->dev;
	
	pctl->pctl_dev = devm_pinctrl_register(&pdev->dev, &pctl->pctl_desc, pctl);
	if (IS_ERR(pctl->pctl_dev)) {
		dev_err(&pdev->dev, "Failed pinctrl registration\n");
		return PTR_ERR(pctl->pctl_dev);
	}
	
	/* Setup pinmux groups */
	ret = pmb887x_build_groups(pctl);
	if (ret)
		return ret;
	
	/* Setup pinmux functions */
	ret = pmb887x_build_functions(pctl);
	if (ret)
		return ret;
	
	/* Setup GPIO */
	ret = pmb887x_gpiolib_register(pctl);
	if (ret)
		return ret;
	
	dev_info(dev, "Pinctrl PMB887X initialized\n");
	
	return 0;
}
