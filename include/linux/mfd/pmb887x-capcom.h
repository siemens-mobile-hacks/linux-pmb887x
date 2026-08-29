/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Infineon PMB887x CAPture/COMpare unit.
 *
 * The block owns two 31-bit timebases (T0/T1) and eight capture/compare
 * channels (CC0..CC7). Each channel is bound to one of the two timebases,
 * so channels are not independent: claiming a channel also claims its
 * timebase. Sub-functions (PWM, counter, irqchip) live in child nodes and
 * reach the block through this structure; only the parent maps the MMIO.
 */
#ifndef _LINUX_MFD_PMB887X_CAPCOM_H
#define _LINUX_MFD_PMB887X_CAPCOM_H

#include <linux/bits.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/spinlock.h>
#include <linux/types.h>

#define PMB887X_CC_NR_CHANNELS		8
#define PMB887X_CC_NR_TIMERS		2

/*
 * Timebase for a channel that only drives its output latch through OUT and
 * never compares or captures, so it has no business tying up T0 or T1.
 */
#define PMB887X_CC_TIMER_NONE		0xff

#define PMB887X_CC_CLC			0x00

#define PMB887X_CC_PISEL		0x04
#define PMB887X_CC_PISEL_C1C0IS		BIT(0)
#define PMB887X_CC_PISEL_C3C2IS		BIT(1)
#define PMB887X_CC_PISEL_C5C4IS		BIT(2)
#define PMB887X_CC_PISEL_C7C6IS		BIT(3)
#define PMB887X_CC_PISEL_T0INIS		BIT(4)
#define PMB887X_CC_PISEL_T1INIS		BIT(5)

#define PMB887X_CC_ID			0x08

/* Both timebases share T01CON, T1's fields sitting 8 bits above T0's. */
#define PMB887X_CC_T01CON		0x10
#define PMB887X_CC_T01CON_TI(n)		(GENMASK(2, 0) << ((n) * 8))
#define PMB887X_CC_T01CON_TI_SHIFT(n)	((n) * 8)
#define PMB887X_CC_T01CON_TM(n)		BIT(3 + (n) * 8)
#define PMB887X_CC_T01CON_TR(n)		BIT(6 + (n) * 8)

/* TI: what makes the timebase tick. */
#define PMB887X_CC_TI_OVERFLOW		0x0
#define PMB887X_CC_TI_RISING_EDGE	0x1
#define PMB887X_CC_TI_FALLING_EDGE	0x2
#define PMB887X_CC_TI_BOTH_EDGES	0x3

/* TM: prescaled system clock vs. external count input. */
#define PMB887X_CC_TM_TIMER		0
#define PMB887X_CC_TM_COUNTER		1

/* CCM0 covers CC0..CC3, CCM1 covers CC4..CC7, four bits per channel. */
#define PMB887X_CC_CCM(ch)		(0x14 + ((ch) / 4) * 4)
#define PMB887X_CC_CCM_SHIFT(ch)	(((ch) % 4) * 4)
#define PMB887X_CC_CCM_MOD(ch)		(GENMASK(2, 0) << PMB887X_CC_CCM_SHIFT(ch))
#define PMB887X_CC_CCM_ACC(ch)		BIT(PMB887X_CC_CCM_SHIFT(ch) + 3)

/* MOD: what the channel does. */
#define PMB887X_CC_MOD_DISABLE		0x0
#define PMB887X_CC_MOD_RISING_EDGE	0x1
#define PMB887X_CC_MOD_FALLING_EDGE	0x2
#define PMB887X_CC_MOD_BOTH_EDGES	0x3
#define PMB887X_CC_MOD_MODE0		0x4
#define PMB887X_CC_MOD_MODE1		0x5
#define PMB887X_CC_MOD_MODE2		0x6
#define PMB887X_CC_MOD_MODE3		0x7

#define PMB887X_CC_OUT			0x24
#define PMB887X_CC_IOC			0x28
#define PMB887X_CC_IOC_PL		BIT(1)
#define PMB887X_CC_IOC_STAG		BIT(2)
#define PMB887X_CC_IOC_PDS		BIT(3)

#define PMB887X_CC_SEM			0x2C
#define PMB887X_CC_SEE			0x30

#define PMB887X_CC_DRM			0x34
#define PMB887X_CC_DRM_DRM(n)		(GENMASK(1, 0) << ((n) * 2))

#define PMB887X_CC_WHBSSEE		0x38
#define PMB887X_CC_WHBCSEE		0x3C

/* T0/T0REL/T1/T1REL, interleaved. */
#define PMB887X_CC_T(n)			(0x40 + (n) * 8)
#define PMB887X_CC_TREL(n)		(0x44 + (n) * 8)
#define PMB887X_CC_T_OVF		BIT(31)

#define PMB887X_CC_CC(ch)		(0x50 + (ch) * 4)

/* Write-only: clears the matching overflow flag in T0/T1. */
#define PMB887X_CC_T01OCR		0x94
#define PMB887X_CC_T01OCR_CT(n)		BIT(n)

/* Write-only bit-set/bit-clear aliases of OUT. */
#define PMB887X_CC_WHBSOUT		0x98
#define PMB887X_CC_WHBCOUT		0x9C

/* Interrupt nodes, laid out backwards: CC7_SRC..CC0_SRC, then T1_SRC, T0_SRC. */
#define PMB887X_CC_CC_SRC(ch)		(0xF4 - (ch) * 4)
#define PMB887X_CC_T_SRC(n)		(0xFC - (n) * 4)
#define PMB887X_CC_SRC_SRPN		GENMASK(7, 0)
#define PMB887X_CC_SRC_TOS		GENMASK(11, 10)
#define PMB887X_CC_SRC_SRE		BIT(12)
#define PMB887X_CC_SRC_SRR		BIT(13)
#define PMB887X_CC_SRC_CLRR		BIT(14)
#define PMB887X_CC_SRC_SETR		BIT(15)

/**
 * struct pmb887x_capcom - CAPCOM block shared between the child drivers
 * @dev:	the MFD parent device
 * @base:	mapped registers, mapped once by the parent
 * @lock:	guards every read-modify-write on @base and the claim state
 * @t_irq:	VIC interrupts of T0/T1. T1's is owned by the parent's pulse
 *		engine, T0's is free for a child to request.
 * @cc_irq:	VIC interrupts of CC0..CC7, for the irqchip child to request
 * @counter_width: significant bits of T0/T1, the top bit being the overflow flag
 * @ch_claimed:	channels handed out by pmb887x_capcom_request_channel()
 * @ch_timer:	timebase each claimed channel is bound to
 * @tim_owner:	child device a timebase is bound to, NULL while unclaimed
 * @tim_users:	channels a timebase is currently bound to
 *
 * Everything below @counter_width is private to the parent.
 */
struct pmb887x_capcom {
	struct device *dev;
	void __iomem *base;
	raw_spinlock_t lock;
	int t_irq[PMB887X_CC_NR_TIMERS];
	int cc_irq[PMB887X_CC_NR_CHANNELS];
	u8 counter_width;

	unsigned long ch_claimed;
	u8 ch_timer[PMB887X_CC_NR_CHANNELS];
	struct device *tim_owner[PMB887X_CC_NR_TIMERS];
	unsigned int tim_users[PMB887X_CC_NR_TIMERS];
};

/**
 * struct pmb887x_capcom_pulse_cfg - finite pulse train on a channel output
 * @nphases:	number of output phases to emit, each ended by a T1 reload
 * @first:	length of the first phase in timer ticks
 * @next:	fetches the length of the phase following the one that just
 *		ended, in timer ticks. Called in hard IRQ context under
 *		&pmb887x_capcom.lock, so it must not sleep. Returning 0 ends
 *		the train early.
 * @ctx:	passed to @next
 * @start_high:	level driven during the first phase; the output is flipped at
 *		the end of every phase and is left wherever the last flip put it
 * @timeout_ms:	how long pmb887x_capcom_pulse() waits for the train to finish
 */
struct pmb887x_capcom_pulse_cfg {
	unsigned int nphases;
	u32 first;
	u32 (*next)(void *ctx);
	void *ctx;
	bool start_high;
	unsigned int timeout_ms;
};

static inline u32 pmb887x_capcom_readl(struct pmb887x_capcom *cap, u32 reg)
{
	return readl_relaxed(cap->base + reg);
}

static inline void pmb887x_capcom_writel(struct pmb887x_capcom *cap, u32 reg,
					 u32 val)
{
	writel_relaxed(val, cap->base + reg);
}

/* Callers must hold &pmb887x_capcom.lock. */
static inline void __pmb887x_capcom_update(struct pmb887x_capcom *cap, u32 reg,
					   u32 mask, u32 val)
{
	u32 tmp = readl_relaxed(cap->base + reg);

	writel_relaxed((tmp & ~mask) | (val & mask), cap->base + reg);
}

static inline void pmb887x_capcom_update(struct pmb887x_capcom *cap, u32 reg,
					 u32 mask, u32 val)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&cap->lock, flags);
	__pmb887x_capcom_update(cap, reg, mask, val);
	raw_spin_unlock_irqrestore(&cap->lock, flags);
}

/*
 * Reload value that makes an up-counting timebase overflow after @ticks.
 * The overflow flag occupies the bit above the counter, so it must not be
 * disturbed here.
 */
static inline u32 pmb887x_capcom_reload(const struct pmb887x_capcom *cap,
					u32 ticks)
{
	u32 span = 1U << cap->counter_width;

	return (span - ticks) & (span - 1);
}

static inline u32 pmb887x_capcom_max_ticks(const struct pmb887x_capcom *cap)
{
	return (1U << cap->counter_width) - 1;
}

#if IS_REACHABLE(CONFIG_MFD_PMB887X_CAPCOM)
int pmb887x_capcom_request_channel(struct pmb887x_capcom *cap, unsigned int ch,
				   unsigned int timer, u32 mode,
				   struct device *owner);
void pmb887x_capcom_release_channel(struct pmb887x_capcom *cap,
				    unsigned int ch);
int pmb887x_capcom_pulse(struct pmb887x_capcom *cap, unsigned int ch,
			 const struct pmb887x_capcom_pulse_cfg *cfg);
#else
static inline int pmb887x_capcom_request_channel(struct pmb887x_capcom *cap,
						 unsigned int ch,
						 unsigned int timer, u32 mode,
						 struct device *owner)
{
	return -ENODEV;
}

static inline void pmb887x_capcom_release_channel(struct pmb887x_capcom *cap,
						  unsigned int ch)
{
}

static inline int pmb887x_capcom_pulse(struct pmb887x_capcom *cap,
				       unsigned int ch,
				       const struct pmb887x_capcom_pulse_cfg *cfg)
{
	return -ENODEV;
}
#endif

#endif /* _LINUX_MFD_PMB887X_CAPCOM_H */
