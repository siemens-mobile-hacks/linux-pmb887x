// SPDX-License-Identifier: GPL-2.0-only
/*
 * LG KE970 (Shine) LCD backlight.
 */
#define pr_fmt(fmt) "ke970-bl: " fmt

#include <linux/backlight.h>
#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/irqflags.h>
#include <linux/mfd/pmb887x-capcom.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/property.h>

/* Dimming steps the charge pump cycles through, 1/32 of the set current each. */
#define KE970_BL_STEPS 32

#define KE970_BL_LO_US 50 /* t_LO: 500 ns .. 250 us */
#define KE970_BL_HI_US 50 /* t_HI: > 500 ns */
#define KE970_BL_INIT_HI_US 250 /* initial t_HI: > 200 us */
#define KE970_BL_SHDN_US 5000 /* t_SHDN: 2 ms typical */

struct ke970_bl {
	struct pmb887x_capcom *cap;
	unsigned int ch;
	/* Pulses applied since the LEDs were last enabled, 0 being full. */
	unsigned int step;
	bool enabled;
};

static void ke970_bl_set_line(struct ke970_bl *bl, bool level)
{
	pmb887x_capcom_writel(bl->cap,
			      level ? PMB887X_CC_WHBSOUT : PMB887X_CC_WHBCOUT,
			      BIT(bl->ch));
}

static void ke970_bl_pulse(struct ke970_bl *bl)
{
	unsigned long flags;

	local_irq_save(flags);
	ke970_bl_set_line(bl, false);
	udelay(KE970_BL_LO_US);
	ke970_bl_set_line(bl, true);
	local_irq_restore(flags);

	udelay(KE970_BL_HI_US);
}

/*
 * Dimming is relative and wraps at 32, so any step is reachable from any other
 * without taking the LEDs through a shutdown. That costs us a software copy of
 * the charge pump's state, which is the price of not flashing the display dark
 * on every brightness change.
 */
static void ke970_bl_set_step(struct ke970_bl *bl, unsigned int step)
{
	unsigned int pulses;

	pulses = (step + KE970_BL_STEPS - bl->step) % KE970_BL_STEPS;

	while (pulses--)
		ke970_bl_pulse(bl);

	bl->step = step;
}

static void ke970_bl_enable(struct ke970_bl *bl)
{
	ke970_bl_set_line(bl, true);

	/*
	 * Soft-start outlasts the initial t_HI, so the dimming pulses have to
	 * follow it promptly or the LEDs visibly ramp through full brightness
	 * on the way to the requested step.
	 */
	udelay(KE970_BL_INIT_HI_US);

	/* Raising the line selects full current. */
	bl->step = 0;
	bl->enabled = true;
}

static void ke970_bl_disable(struct ke970_bl *bl)
{
	ke970_bl_set_line(bl, false);
	bl->enabled = false;

	/* Hold the line down long enough to be read as a shutdown. */
	usleep_range(KE970_BL_SHDN_US, 2 * KE970_BL_SHDN_US);
}

static int ke970_bl_update_status(struct backlight_device *bd)
{
	struct ke970_bl *bl = bl_get_data(bd);
	int brightness = backlight_get_brightness(bd);

	if (!brightness) {
		if (bl->enabled)
			ke970_bl_disable(bl);
		return 0;
	}

	if (!bl->enabled)
		ke970_bl_enable(bl);

	ke970_bl_set_step(bl, KE970_BL_STEPS - brightness);

	return 0;
}

static const struct backlight_ops ke970_bl_ops = {
	.options = BL_CORE_SUSPENDRESUME,
	.update_status = ke970_bl_update_status,
};

static void ke970_bl_release(void *data)
{
	struct ke970_bl *bl = data;

	ke970_bl_set_line(bl, false);
	pmb887x_capcom_release_channel(bl->cap, bl->ch);
}

static int ke970_bl_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct backlight_properties props = {};
	struct backlight_device *bd;
	struct ke970_bl *bl;
	u32 ch, level;
	int ret;

	bl = devm_kzalloc(dev, sizeof(*bl), GFP_KERNEL);
	if (!bl)
		return -ENOMEM;

	bl->cap = dev_get_drvdata(dev->parent);
	if (!bl->cap)
		return -ENODEV;

	ret = device_property_read_u32(dev, "lg,capcom-channel", &ch);
	if (ret)
		return dev_err_probe(dev, ret,
				     "missing lg,capcom-channel property\n");
	bl->ch = ch;

	ret = pmb887x_capcom_request_channel(bl->cap, ch, PMB887X_CC_TIMER_NONE,
					     PMB887X_CC_MOD_DISABLE, dev);
	if (ret)
		return dev_err_probe(dev, ret, "failed to claim CC%u\n", ch);

	ret = devm_add_action_or_reset(dev, ke970_bl_release, bl);
	if (ret)
		return ret;

	/* Reset the backlight, because we don't know what state it was left in. */
	ke970_bl_disable(bl);
	bl->step = 0;

	props.type = BACKLIGHT_RAW;
	props.max_brightness = KE970_BL_STEPS;
	props.brightness = KE970_BL_STEPS;
	props.power = BACKLIGHT_POWER_ON;

	if (!device_property_read_u32(dev, "default-brightness-level", &level))
		props.brightness = min_t(u32, level, KE970_BL_STEPS);

	bd = devm_backlight_device_register(dev, dev_name(dev), dev, bl,
					    &ke970_bl_ops, &props);
	if (IS_ERR(bd))
		return dev_err_probe(dev, PTR_ERR(bd),
				     "failed to register backlight\n");

	backlight_update_status(bd);

	return 0;
}

static const struct of_device_id ke970_bl_of_match[] = {
	{ .compatible = "lg,ke970-backlight" },
	{}
};
MODULE_DEVICE_TABLE(of, ke970_bl_of_match);

static struct platform_driver ke970_bl_driver = {
	.probe = ke970_bl_probe,
	.driver = {
		.name = "ke970-backlight",
		.of_match_table = ke970_bl_of_match,
	},
};
module_platform_driver(ke970_bl_driver);

MODULE_DESCRIPTION("LG KE970 LCD backlight driver");
MODULE_LICENSE("GPL");
