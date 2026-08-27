/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __PINCTRL_PMB887X_H
#define __PINCTRL_PMB887X_H

#include <linux/pinctrl/pinctrl.h>
#include <linux/pinctrl/pinconf-generic.h>

#define PMB887X_PIN_NO(x) ((x) << 8)
#define PMB887X_GET_PIN_NO(x) ((x) >> 8)
#define PMB887X_GET_PIN_FUNC(x) ((x) & 0xff)

#define PMB887X_CONFIG_NUM 8

#define PMB887X_ALT_MODE_IS 0
#define PMB887X_ALT_MODE_OS 1
#define PMB887X_ALT_MODE_IS_OS 2

struct pmb887x_desc_mux {
	const char *name;
	const u8 is;
	const u8 os;
};

struct pmb887x_desc_pin {
	struct pinctrl_pin_desc pin;
	const struct pmb887x_desc_mux functions[PMB887X_CONFIG_NUM];
};

#define PMB887X_PIN(_pin, ...)                \
	{                                     \
		.pin = _pin,                  \
		.functions = { __VA_ARGS__ }, \
	}

#define PMB887X_FUNCTION(_is, _os, _name) \
	{                                 \
		.name = _name,            \
		.is = _is,                \
		.os = _os,                \
	}

struct pmb887x_pinctrl_match_data {
	const struct pmb887x_desc_pin *pins;
	const unsigned int npins;
};

struct pmb887x_gpio_bank;

int pmb887x_pctl_probe(struct platform_device *pdev);
void pmb887x_pmx_get_mode(struct pmb887x_gpio_bank *bank, int pin, u32 *mode,
			  u32 *alt);
int pmb887x_pinctrl_suspend(struct device *dev);
int pmb887x_pinctrl_resume(struct device *dev);

#endif /* __PINCTRL_PMB887X_H */
