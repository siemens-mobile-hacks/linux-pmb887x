// SPDX-License-Identifier: GPL-2.0-only
/*
 * Infineon PMB887x CAPture/COMpare unit.
 *
 * Maps the block once and hands it to the PWM, counter and irqchip children
 * described below it in the device tree, along with a claim API that keeps
 * them from fighting over the eight channels and the two timebases.
 */
#define pr_fmt(fmt) "pmb887x-capcom: " fmt

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/mfd/pmb887x-capcom.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>

#define PMB887X_CC_PULSE_DEFAULT_TIMEOUT_MS 1000

struct pmb887x_capcom_pulse {
	const struct pmb887x_capcom_pulse_cfg *cfg;
	struct completion done;
	unsigned int ch;
	unsigned int remaining;
	bool level;
	bool active;
};

struct pmb887x_capcom_priv {
	struct pmb887x_capcom cap;
	/* Serializes pulse trains; only one may use T1 at a time. */
	struct mutex pulse_mutex;
	struct pmb887x_capcom_pulse pulse;
};

struct pmb887x_capcom_data {
	u8 counter_width;
};

static inline struct pmb887x_capcom_priv *to_priv(struct pmb887x_capcom *cap)
{
	return container_of(cap, struct pmb887x_capcom_priv, cap);
}

/* Callers must hold cap->lock. */
static void pmb887x_capcom_set_out(struct pmb887x_capcom *cap, unsigned int ch,
				   bool level)
{
	pmb887x_capcom_writel(
		cap, level ? PMB887X_CC_WHBSOUT : PMB887X_CC_WHBCOUT, BIT(ch));
}

/* Callers must hold cap->lock. */
static void pmb887x_capcom_timer_run(struct pmb887x_capcom *cap,
				     unsigned int timer, bool run)
{
	__pmb887x_capcom_update(cap, PMB887X_CC_T01CON,
				PMB887X_CC_T01CON_TR(timer),
				run ? PMB887X_CC_T01CON_TR(timer) : 0);
}

/**
 * pmb887x_capcom_request_channel - claim a channel and bind it to a timebase
 * @cap:	 the CAPCOM block
 * @ch:		channel index, 0..7
 * @timer:	timebase to drive the channel, 0 for T0, 1 for T1, or
 *		PMB887X_CC_TIMER_NONE for a channel that only drives its output
 * @mode:	PMB887X_CC_MOD_* value to program into the channel's MOD field
 * @owner:	child device taking the channel
 *
 * A timebase is shared by every channel of a single owner, so a second owner
 * asking for a channel on an already bound timebase is refused rather than
 * silently made to follow someone else's period.
 *
 * Return: 0 on success, -EBUSY if the channel or the timebase is taken.
 */
int pmb887x_capcom_request_channel(struct pmb887x_capcom *cap, unsigned int ch,
				   unsigned int timer, u32 mode,
				   struct device *owner)
{
	unsigned long flags;
	int ret = 0;

	if (ch >= PMB887X_CC_NR_CHANNELS || mode > PMB887X_CC_MOD_MODE3 ||
	    !owner)
		return -EINVAL;

	if (timer >= PMB887X_CC_NR_TIMERS && timer != PMB887X_CC_TIMER_NONE)
		return -EINVAL;

	raw_spin_lock_irqsave(&cap->lock, flags);

	if (cap->ch_claimed & BIT(ch)) {
		ret = -EBUSY;
		goto out;
	}

	if (timer != PMB887X_CC_TIMER_NONE && cap->tim_users[timer] &&
	    cap->tim_owner[timer] != owner) {
		ret = -EBUSY;
		goto out;
	}

	cap->ch_claimed |= BIT(ch);
	cap->ch_timer[ch] = timer;

	if (timer != PMB887X_CC_TIMER_NONE) {
		cap->tim_owner[timer] = owner;
		cap->tim_users[timer]++;
	}

	__pmb887x_capcom_update(cap, PMB887X_CC_CCM(ch),
				PMB887X_CC_CCM_MOD(ch) | PMB887X_CC_CCM_ACC(ch),
				(mode << PMB887X_CC_CCM_SHIFT(ch)) |
					(timer == 1 ? PMB887X_CC_CCM_ACC(ch) :
						      0));

out:
	raw_spin_unlock_irqrestore(&cap->lock, flags);

	if (ret)
		dev_dbg(cap->dev, "CC%u (T%u) busy for %s\n", ch, timer,
			dev_name(owner));

	return ret;
}
EXPORT_SYMBOL_GPL(pmb887x_capcom_request_channel);

/**
 * pmb887x_capcom_release_channel - give a channel and its timebase back
 * @cap:	the CAPCOM block
 * @ch:		channel index, 0..7
 *
 * Stops the timebase once its last channel is released.
 */
void pmb887x_capcom_release_channel(struct pmb887x_capcom *cap, unsigned int ch)
{
	unsigned long flags;
	unsigned int timer;

	if (ch >= PMB887X_CC_NR_CHANNELS)
		return;

	raw_spin_lock_irqsave(&cap->lock, flags);

	if (!(cap->ch_claimed & BIT(ch)))
		goto out;

	timer = cap->ch_timer[ch];
	cap->ch_claimed &= ~BIT(ch);

	if (timer != PMB887X_CC_TIMER_NONE && !--cap->tim_users[timer]) {
		cap->tim_owner[timer] = NULL;
		pmb887x_capcom_timer_run(cap, timer, false);
	}

	__pmb887x_capcom_update(cap, PMB887X_CC_CCM(ch), PMB887X_CC_CCM_MOD(ch),
				PMB887X_CC_MOD_DISABLE);
	pmb887x_capcom_set_out(cap, ch, false);

out:
	raw_spin_unlock_irqrestore(&cap->lock, flags);
}
EXPORT_SYMBOL_GPL(pmb887x_capcom_release_channel);

/*
 * T1 drives the pulse engine: each overflow ends one phase, flips the channel
 * output and reloads T1 with the length of the next one. Everything happens in
 * hard IRQ context so the phase boundaries stay where the caller asked for
 * them, which is the whole point of using the hardware for this.
 */
static irqreturn_t pmb887x_capcom_t1_isr(int irq, void *data)
{
	struct pmb887x_capcom_priv *priv = data;
	struct pmb887x_capcom *cap = &priv->cap;
	struct pmb887x_capcom_pulse *pulse = &priv->pulse;
	bool finished = false;
	u32 next = 0;

	raw_spin_lock(&cap->lock);

	pmb887x_capcom_writel(cap, PMB887X_CC_T01OCR, PMB887X_CC_T01OCR_CT(1));
	pmb887x_capcom_writel(cap, PMB887X_CC_T_SRC(1),
			      PMB887X_CC_SRC_SRE | PMB887X_CC_SRC_CLRR);

	if (!pulse->active)
		goto out;

	pulse->level = !pulse->level;
	pmb887x_capcom_set_out(cap, pulse->ch, pulse->level);

	if (--pulse->remaining && pulse->cfg->next)
		next = pulse->cfg->next(pulse->cfg->ctx);

	if (next && next <= pmb887x_capcom_max_ticks(cap)) {
		pmb887x_capcom_writel(cap, PMB887X_CC_TREL(1),
				      pmb887x_capcom_reload(cap, next));
	} else {
		pmb887x_capcom_timer_run(cap, 1, false);
		pmb887x_capcom_writel(cap, PMB887X_CC_T_SRC(1), 0);
		pulse->active = false;
		finished = true;
	}

out:
	raw_spin_unlock(&cap->lock);

	if (finished)
		complete(&pulse->done);

	return IRQ_HANDLED;
}

/**
 * pmb887x_capcom_pulse - emit a finite pulse train on a channel output
 * @cap:	the CAPCOM block
 * @ch:		channel index, previously claimed on timebase T1
 * @cfg:	shape of the train
 *
 * Blocks until the train has been emitted, so it must be called from process
 * context. The channel must already be claimed with timer = 1, since the
 * engine owns T1 for the duration.
 *
 * Return: 0 on success, -ETIMEDOUT if the train did not finish in time.
 */
int pmb887x_capcom_pulse(struct pmb887x_capcom *cap, unsigned int ch,
			 const struct pmb887x_capcom_pulse_cfg *cfg)
{
	struct pmb887x_capcom_priv *priv = to_priv(cap);
	struct pmb887x_capcom_pulse *pulse = &priv->pulse;
	unsigned int timeout_ms;
	unsigned long flags;
	int ret = 0;

	if (ch >= PMB887X_CC_NR_CHANNELS || !cfg || !cfg->nphases ||
	    !cfg->first)
		return -EINVAL;

	if (cfg->first > pmb887x_capcom_max_ticks(cap))
		return -ERANGE;

	timeout_ms = cfg->timeout_ms ?: PMB887X_CC_PULSE_DEFAULT_TIMEOUT_MS;

	guard(mutex)
		(&priv->pulse_mutex);

	raw_spin_lock_irqsave(&cap->lock, flags);

	if (!(cap->ch_claimed & BIT(ch)) || cap->ch_timer[ch] != 1) {
		raw_spin_unlock_irqrestore(&cap->lock, flags);
		return -EINVAL;
	}

	reinit_completion(&pulse->done);
	pulse->cfg = cfg;
	pulse->ch = ch;
	pulse->remaining = cfg->nphases;
	pulse->level = cfg->start_high;
	pulse->active = true;

	pmb887x_capcom_timer_run(cap, 1, false);
	pmb887x_capcom_set_out(cap, ch, pulse->level);

	pmb887x_capcom_writel(cap, PMB887X_CC_TREL(1),
			      pmb887x_capcom_reload(cap, cfg->first));
	pmb887x_capcom_writel(cap, PMB887X_CC_T(1),
			      pmb887x_capcom_reload(cap, cfg->first));

	pmb887x_capcom_writel(cap, PMB887X_CC_T01OCR, PMB887X_CC_T01OCR_CT(1));
	pmb887x_capcom_writel(cap, PMB887X_CC_T_SRC(1),
			      PMB887X_CC_SRC_SRE | PMB887X_CC_SRC_CLRR);

	pmb887x_capcom_timer_run(cap, 1, true);

	raw_spin_unlock_irqrestore(&cap->lock, flags);

	if (!wait_for_completion_timeout(&pulse->done,
					 msecs_to_jiffies(timeout_ms))) {
		raw_spin_lock_irqsave(&cap->lock, flags);
		pmb887x_capcom_timer_run(cap, 1, false);
		pmb887x_capcom_writel(cap, PMB887X_CC_T_SRC(1), 0);
		pulse->active = false;
		raw_spin_unlock_irqrestore(&cap->lock, flags);

		dev_err(cap->dev, "CC%u pulse train timed out\n", ch);
		ret = -ETIMEDOUT;
	}

	return ret;
}
EXPORT_SYMBOL_GPL(pmb887x_capcom_pulse);

static const char *const pmb887x_capcom_cc_irq_names[PMB887X_CC_NR_CHANNELS] = {
	"cc0", "cc1", "cc2", "cc3", "cc4", "cc5", "cc6", "cc7",
};

static const char *const pmb887x_capcom_t_irq_names[PMB887X_CC_NR_TIMERS] = {
	"t0",
	"t1",
};

static int pmb887x_capcom_probe(struct platform_device *pdev)
{
	const struct pmb887x_capcom_data *data;
	struct device *dev = &pdev->dev;
	struct pmb887x_capcom_priv *priv;
	struct pmb887x_capcom *cap;
	struct clk *clk;
	unsigned int i;
	int ret;

	data = of_device_get_match_data(dev);
	if (!data)
		return -ENODEV;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	cap = &priv->cap;
	cap->dev = dev;
	cap->counter_width = data->counter_width;
	raw_spin_lock_init(&cap->lock);
	init_completion(&priv->pulse.done);

	ret = devm_mutex_init(dev, &priv->pulse_mutex);
	if (ret)
		return ret;

	cap->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(cap->base))
		return PTR_ERR(cap->base);

	clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(clk))
		return dev_err_probe(dev, PTR_ERR(clk),
				     "failed to get clock\n");

	for (i = 0; i < PMB887X_CC_NR_TIMERS; i++) {
		ret = platform_get_irq_byname(pdev,
					      pmb887x_capcom_t_irq_names[i]);
		if (ret < 0)
			return ret;
		cap->t_irq[i] = ret;
	}

	for (i = 0; i < PMB887X_CC_NR_CHANNELS; i++) {
		ret = platform_get_irq_byname(pdev,
					      pmb887x_capcom_cc_irq_names[i]);
		if (ret < 0)
			return ret;
		cap->cc_irq[i] = ret;
	}

	/*
	 * Leave every interrupt node masked; the children arm the ones they
	 * use. T1's belongs to the pulse engine and is armed per train.
	 */
	for (i = 0; i < PMB887X_CC_NR_TIMERS; i++)
		pmb887x_capcom_writel(cap, PMB887X_CC_T_SRC(i), 0);
	for (i = 0; i < PMB887X_CC_NR_CHANNELS; i++)
		pmb887x_capcom_writel(cap, PMB887X_CC_CC_SRC(i), 0);

	pmb887x_capcom_writel(cap, PMB887X_CC_T01CON, 0);
	pmb887x_capcom_writel(cap, PMB887X_CC_CCM(0), 0);
	pmb887x_capcom_writel(cap, PMB887X_CC_CCM(4), 0);
	pmb887x_capcom_writel(cap, PMB887X_CC_WHBCOUT,
			      GENMASK(PMB887X_CC_NR_CHANNELS - 1, 0));

	ret = devm_request_irq(dev, cap->t_irq[1], pmb887x_capcom_t1_isr,
			       IRQF_NO_THREAD, dev_name(dev), priv);
	if (ret)
		return dev_err_probe(dev, ret, "failed to request T1 irq\n");

	platform_set_drvdata(pdev, cap);

	return devm_of_platform_populate(dev);
}

static const struct pmb887x_capcom_data pmb8876_capcom_data = {
	.counter_width = 31,
};

static const struct of_device_id pmb887x_capcom_of_match[] = {
	{ .compatible = "pmb887x,capcom", .data = &pmb8876_capcom_data },
	{}
};
MODULE_DEVICE_TABLE(of, pmb887x_capcom_of_match);

static struct platform_driver pmb887x_capcom_driver = {
	.probe = pmb887x_capcom_probe,
	.driver = {
		.name = "pmb887x-capcom",
		.of_match_table = pmb887x_capcom_of_match,
	},
};
module_platform_driver(pmb887x_capcom_driver);

MODULE_DESCRIPTION("Infineon PMB887x CAPCOM driver");
MODULE_LICENSE("GPL");
