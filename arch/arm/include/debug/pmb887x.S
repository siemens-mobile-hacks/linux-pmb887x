/* SPDX-License-Identifier: GPL-2.0-only */
/* arch/arm/include/debug/pmb887x.S
 *
 *  Derived from pl01x.S
 *
 *  Copyright (C) 1994-1999 Russell King
 *  Moved from linux/arch/arm/kernel/debug.S by Ben Dooks
*/
#include <linux/amba/serial.h>

#define USART_TXB 0x20
#define USART_RIS 0x68
#define USART_RIS_TX 1
#define USART_RIS_RX 4
#define USART_ICR 0x70
#define USART_ICR_TX 1

#ifdef CONFIG_DEBUG_UART_PHYS
		.macro	addruart, rp, rv, tmp
		ldr	\rp, =CONFIG_DEBUG_UART_PHYS
		ldr	\rv, =CONFIG_DEBUG_UART_VIRT
		.endm
#endif

		.macro	senduart,rd,rx
		strb	\rd, [\rx, #USART_TXB]
		.endm

		.macro	waituartcts,rd,rx
		.endm

		.macro	waituarttxrdy,rd,rx
		.endm

		.macro	busyuart,rd,rx
1001:		ldr	\rd, [\rx, #USART_RIS]
 ARM_BE8(	rev	\rd, \rd )
		tst	\rd, #USART_RIS_TX
		beq	1001b
				
		mov	\rd, #USART_ICR_TX
		str	\rd, [\rx, #USART_ICR]
		.endm
