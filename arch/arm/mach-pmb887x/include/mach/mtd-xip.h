/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * MTD primitives for XIP support. Architecture specific functions
 *
 * Do not include this file directly. It's included from linux/mtd/xip.h
 */

#ifndef __ARCH_PMB887X_MTD_XIP_H__
#define __ARCH_PMB887X_MTD_XIP_H__

#define xip_irqpending()	(0)
#define xip_currtime()		(0)
#define xip_elapsed_since(x)	(0)

#endif /* __ARCH_PMB887X_MTD_XIP_H__ */
