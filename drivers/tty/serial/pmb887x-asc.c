// SPDX-License-Identifier: GPL-2.0+
#include <linux/module.h>
#include <linux/serial.h>
#include <linux/console.h>
#include <linux/sysrq.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/delay.h>
#include <linux/spinlock.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/serial_core.h>
#include <linux/clk.h>
#include <linux/gpio/consumer.h>

#define DRIVER_NAME			"pmb887x-asc"
#define ASC_SERIAL_NAME		"ttyASC"

#define ASC_STATUS_PE		(1 << 0)
#define ASC_STATUS_FE		(1 << 1)
#define ASC_STATUS_OE		(1 << 2)
#define ASC_STATUS_BE		(1 << 3)
#define ASC_STATUS_RX		(1 << 4)

#define ASC_MAX_PORTS	2
#define ASC_FIFO_SIZE	8
#define ASC_CLOCK_DIV	16
#define ASC_MAX_FDV		511
#define ASC_MAX_BG		8191

#define	ASC_CON							(0x10)
#define	ASC_CON_M						GENMASK(2, 0)			 // ASC Mode Control.
#define	ASC_CON_M_SHIFT					0
#define	ASC_CON_M_SYNC_8BIT				0x0
#define	ASC_CON_M_ASYNC_8BIT			0x1
#define	ASC_CON_M_ASYNC_IRDA_8BIT		0x2
#define	ASC_CON_M_ASYNC_PARITY_7BIT		0x3
#define	ASC_CON_M_ASYNC_9BIT			0x4
#define	ASC_CON_M_ASYNC_WAKE_UP_8BIT	0x5
#define	ASC_CON_M_ASYNC_PARITY_8BIT		0x7
#define	ASC_CON_STP						BIT(3)					 // Number of stop bits (0: 1 stop bit; 1: two stop bits)
#define	ASC_CON_STP_ONE					0x0
#define	ASC_CON_STP_TWO					0x8
#define	ASC_CON_REN						BIT(4)					 // Receiver bit enable (0: disable; 1: enable)
#define	ASC_CON_PEN						BIT(5)					 // Parity check enable (0: ignore; 1: check)
#define	ASC_CON_FEN						BIT(6)					 // Framing error check (0: ignore; 1: check)
#define	ASC_CON_OEN						BIT(7)					 // Overrun check enable (0: ignore; 1: check)
#define	ASC_CON_PE						BIT(8)					 // Parity error flag
#define	ASC_CON_FE						BIT(9)					 // Framing error flag
#define	ASC_CON_OE						BIT(10)					 // Overrun error flag
#define	ASC_CON_FDE						BIT(11)					 // Fraction divider enable (0: disable; 1: enable)
#define	ASC_CON_ODD						BIT(12)					 // Parity selection (0: even; 1: odd)
#define	ASC_CON_BRS						BIT(13)					 // Baudrate selection (0: Pre-scaler /2; 1: Pre-scaler / 3)
#define	ASC_CON_LB						BIT(14)					 // Loopback mode (0: disable; 1: enable)
#define	ASC_CON_CON_R					BIT(15)					 // Baud rate generator run control (0: disable; 1: enable)

#define	ASC_BG							(0x14)

#define	ASC_FDV							(0x18)

#define	ASC_PMW							(0x1C)
#define	ASC_PMW_PW_VALUE				GENMASK(7, 0)			 // IrDA Pulse Width Value
#define	ASC_PMW_PW_VALUE_SHIFT			0
#define	ASC_PMW_IRPW					BIT(8)					 // IrDA Pulse Width Selection

#define	ASC_TXB							(0x20)

#define	ASC_RXB							(0x24)

#define	ASC_ABCON						(0x30)
#define	ASC_ABCON_ABEN					BIT(0)					 // Autobaud detection enable
#define	ASC_ABCON_AUREN					BIT(1)					 // Auto control of CON.REN (too complex for here)
#define	ASC_ABCON_ABSTEN				BIT(2)					 // Start of autobaud detect interrupt (0: dis; 1: en)
#define	ASC_ABCON_ABDETEN				BIT(3)					 // Autobaud detection interrupt enable (0: dis; 1: en)
#define	ASC_ABCON_FCDETEN				BIT(4)					 // Fir char of two byte frame detect
#define	ASC_ABCON_ABEM_ECHO_DET			BIT(8)					 // Autobaud echo mode enabled during detection
#define	ASC_ABCON_ABEM_ECHO_ALWAYS		BIT(9)					 // Autobaud echo mode always enabled
#define	ASC_ABCON_TXINV					BIT(10)					 // Transmit invert enable (0: disable; 1: enable)
#define	ASC_ABCON_RXINV					BIT(11)					 // Receive invert enable (0: disable; 1: enable)

#define	ASC_ABSTAT						(0x34)
#define	ASC_ABSTAT_FCSDET				BIT(0)					 // First character with small letter detected
#define	ASC_ABSTAT_FCCDET				BIT(1)					 // First character with capital letter detected
#define	ASC_ABSTAT_SCSDET				BIT(2)					 // Second character with small letter detected
#define	ASC_ABSTAT_SCCDET				BIT(3)					 // Second character with capital letter detected
#define	ASC_ABSTAT_DETWAIT				BIT(4)					 // Autobaud detect is waiting

#define	ASC_RXFCON						(0x40)
#define	ASC_RXFCON_RXFEN				BIT(0)					 // Receive FIFO enable
#define	ASC_RXFCON_RXFFLU				BIT(1)					 // Receive FIFO flush
#define	ASC_RXFCON_RXTMEN				BIT(2)					 // Receive FIFO transparent mode enable
#define	ASC_RXFCON_RXFITL				GENMASK(11, 8)			 // Receive FIFO interrupt trigger level
#define	ASC_RXFCON_RXFITL_SHIFT			8

#define	ASC_TXFCON						(0x44)
#define	ASC_TXFCON_TXFEN				BIT(0)					 // Transmit FIFO enable
#define	ASC_TXFCON_TXFFLU				BIT(1)					 // Transmit FIFO flush
#define	ASC_TXFCON_TXTMEN				BIT(2)					 // Transmit FIFO transparent mode enable
#define	ASC_TXFCON_TXFITL				GENMASK(11, 8)			 // Transmit FIFO interrupt trigger level
#define	ASC_TXFCON_TXFITL_SHIFT			8

#define	ASC_FSTAT						(0x48)
#define	ASC_FSTAT_RXFFL					GENMASK(3, 0)			 // Receive FIFO filling level mask
#define	ASC_FSTAT_RXFFL_SHIFT			0
#define	ASC_FSTAT_TXFFL					GENMASK(11, 8)			 // Transmit FIFO filling level mask
#define	ASC_FSTAT_TXFFL_SHIFT			8

#define	ASC_WHBCON						(0x50)
#define	ASC_WHBCON_CLRREN				BIT(4)					 // Clear receiver enable bit
#define	ASC_WHBCON_SETREN				BIT(5)					 // Set receiver enable bit
#define	ASC_WHBCON_CLRPE				BIT(8)					 // Clear parity error flag
#define	ASC_WHBCON_CLRFE				BIT(9)					 // Clear framing error flag
#define	ASC_WHBCON_CLROE				BIT(10)					 // Clear overrun error flag
#define	ASC_WHBCON_SETPE				BIT(11)					 // Set parity error flag
#define	ASC_WHBCON_SETFE				BIT(12)					 // Set framing error flag
#define	ASC_WHBCON_SETOE				BIT(13)					 // Set overrun error flag

#define	ASC_WHBABCON					(0x54)

#define	ASC_WHBABSTAT					(0x58)

#define	ASC_FCCON						(0x5C)
#define	ASC_FCCON_CTSEN					BIT(0)					 // RTS enbled (0: disable; 1: enable)
#define	ASC_FCCON_RTSEN					BIT(1)					 // CTS enable (0: disable; 1: enable)
#define	ASC_FCCON_RTS					BIT(4)					 // RTS control bit
#define	ASC_FCCON_RTS_TRIGGER			GENMASK(13, 8)			 // RTS receive FIFO trigger level
#define	ASC_FCCON_RTS_TRIGGER_SHIFT		8

#define	ASC_FCSTAT						(0x60)
#define	ASC_FCSTAT_CTS					BIT(0)					 // CTS Status (0: inactive; 1: active)
#define	ASC_FCSTAT_RTS					BIT(1)					 // RTS Status (0: inactive; 1: active)

#define	ASC_IMSC						(0x64)
#define	ASC_IMSC_TX						BIT(0)					 // Transmit interrupt mask
#define	ASC_IMSC_TB						BIT(1)					 // Transmit buffer interrupt mask
#define	ASC_IMSC_RX						BIT(2)					 // Receive interrupt mask
#define	ASC_IMSC_ERR					BIT(3)					 // Error interrupt mask
#define	ASC_IMSC_CTS					BIT(4)					 // CTS interrupt mask
#define	ASC_IMSC_ABDET					BIT(5)					 // Autobaud detected interrupt mask
#define	ASC_IMSC_ABSTART				BIT(6)					 // Autobaud start interrupt mask
#define	ASC_IMSC_TMO					BIT(7)					 // RX timeout interrupt mask

#define	ASC_RIS							(0x68)
#define	ASC_RIS_TX						BIT(0)					 // Transmit interrupt mask
#define	ASC_RIS_TB						BIT(1)					 // Transmit buffer interrupt mask
#define	ASC_RIS_RX						BIT(2)					 // Receive interrupt mask
#define	ASC_RIS_ERR						BIT(3)					 // Error interrupt mask
#define	ASC_RIS_CTS						BIT(4)					 // CTS interrupt mask
#define	ASC_RIS_ABDET					BIT(5)					 // Autobaud detected interrupt mask
#define	ASC_RIS_ABSTART					BIT(6)					 // Autobaud start interrupt mask
#define	ASC_RIS_TMO						BIT(7)					 // RX timeout interrupt mask

#define	ASC_MIS							(0x6C)
#define	ASC_MIS_TX						BIT(0)					 // Transmit interrupt mask
#define	ASC_MIS_TB						BIT(1)					 // Transmit buffer interrupt mask
#define	ASC_MIS_RX						BIT(2)					 // Receive interrupt mask
#define	ASC_MIS_ERR						BIT(3)					 // Error interrupt mask
#define	ASC_MIS_CTS						BIT(4)					 // CTS interrupt mask
#define	ASC_MIS_ABDET					BIT(5)					 // Autobaud detected interrupt mask
#define	ASC_MIS_ABSTART					BIT(6)					 // Autobaud start interrupt mask
#define	ASC_MIS_TMO						BIT(7)					 // RX timeout interrupt mask

#define	ASC_ICR							(0x70)
#define	ASC_ICR_TX						BIT(0)					 // Transmit interrupt mask
#define	ASC_ICR_TB						BIT(1)					 // Transmit buffer interrupt mask
#define	ASC_ICR_RX						BIT(2)					 // Receive interrupt mask
#define	ASC_ICR_ERR						BIT(3)					 // Error interrupt mask
#define	ASC_ICR_CTS						BIT(4)					 // CTS interrupt mask
#define	ASC_ICR_ABDET					BIT(5)					 // Autobaud detected interrupt mask
#define	ASC_ICR_ABSTART					BIT(6)					 // Autobaud start interrupt mask
#define	ASC_ICR_TMO						BIT(7)					 // RX timeout interrupt mask

#define	ASC_ISR							(0x74)
#define	ASC_ISR_TX						BIT(0)					 // Transmit interrupt mask
#define	ASC_ISR_TB						BIT(1)					 // Transmit buffer interrupt mask
#define	ASC_ISR_RX						BIT(2)					 // Receive interrupt mask
#define	ASC_ISR_ERR						BIT(3)					 // Error interrupt mask
#define	ASC_ISR_CTS						BIT(4)					 // CTS interrupt mask
#define	ASC_ISR_ABDET					BIT(5)					 // Autobaud detected interrupt mask
#define	ASC_ISR_ABSTART					BIT(6)					 // Autobaud start interrupt mask
#define	ASC_ISR_TMO						BIT(7)					 // RX timeout interrupt mask

#define	ASC_TMO							(0x7C)

struct asc_port {
	struct uart_port port;
	struct gpio_desc *rts;
	struct clk *clk;
	struct pinctrl *pinctrl;
	int irq_rx;
	int irq_tx;
	struct device_node *node;
};

static struct asc_port asc_ports[ASC_MAX_PORTS];
static struct uart_driver asc_uart_driver;
static atomic_t asc_uart_next_id = ATOMIC_INIT(0);

static inline struct asc_port *to_asc_port(struct uart_port *port) {
	return container_of(port, struct asc_port, port);
}

static inline u32 asc_in(struct uart_port *port, u32 offset) {
	return readl_relaxed(port->membase + offset);
}

static inline void asc_out(struct uart_port *port, u32 offset, u32 value) {
	writel_relaxed(value, port->membase + offset);
}

static inline void asc_disable_tx_interrupts(struct uart_port *port) {
	u32 reg = asc_in(port, ASC_IMSC) & ~ASC_IMSC_TX;
	asc_out(port, ASC_IMSC, reg);
}

static inline void asc_enable_tx_interrupts(struct uart_port *port) {
	u32 reg = asc_in(port, ASC_IMSC) | ASC_IMSC_TX;
	asc_out(port, ASC_IMSC, reg);
}

static inline void asc_disable_rx_interrupts(struct uart_port *port) {
	u32 reg = asc_in(port, ASC_IMSC) & ~ASC_IMSC_RX;
	asc_out(port, ASC_IMSC, reg);
}

static inline void asc_enable_rx_interrupts(struct uart_port *port) {
	u32 reg = asc_in(port, ASC_IMSC) | ASC_IMSC_RX;
	asc_out(port, ASC_IMSC, reg);
}

static inline void asc_tx_char(struct uart_port *port, u8 ch) {
	asc_out(port, ASC_TXB, ch);
}

static inline unsigned asc_tx_fifo_avail(struct uart_port *port) {
	u32 fifo_cnt = (asc_in(port, ASC_FSTAT) & ASC_FSTAT_TXFFL) >> ASC_FSTAT_TXFFL_SHIFT;
	return ASC_FIFO_SIZE - fifo_cnt;
}

static inline unsigned asc_rx_fifo_avail(struct uart_port *port) {
	u32 fifo_cnt = (asc_in(port, ASC_FSTAT) & ASC_FSTAT_RXFFL) >> ASC_FSTAT_RXFFL_SHIFT;
	return fifo_cnt;
}

static inline u32 asc_tx_fifo_is_empty(struct uart_port *port) {
	return !(asc_in(port, ASC_FSTAT) & ASC_FSTAT_TXFFL);
}

static inline u32 asc_tx_fifo_is_full(struct uart_port *port) {
	return !asc_tx_fifo_avail(port);
}

static inline u32 asc_rx_fifo_is_empty(struct uart_port *port) {
	return !(asc_in(port, ASC_FSTAT) & ASC_FSTAT_RXFFL);
}

static inline const char *asc_port_name(struct uart_port *port) {
	return to_platform_device(port->dev)->name;
}

static void asc_transmit_chars(struct uart_port *port) {
	u8 ch;
	uart_port_tx_limited(port, ch, asc_tx_fifo_avail(port), true, asc_tx_char(port, ch), ({}));
}

static bool asc_is_8bit_mode(struct uart_port *port) {
	u32 mode = asc_in(port, ASC_CON) & ASC_CON_M;
	return (mode & (ASC_CON_M_SYNC_8BIT | ASC_CON_M_ASYNC_8BIT | ASC_CON_M_ASYNC_IRDA_8BIT | ASC_CON_M_ASYNC_WAKE_UP_8BIT | ASC_CON_M_ASYNC_PARITY_8BIT)) != 0;
}

static u32 asc_get_status(struct uart_port *port) {
	u32 status = ASC_STATUS_RX;
	u32 lsr = asc_in(port, ASC_CON);
	
	if ((lsr & ASC_CON_PE))
		status |= ASC_STATUS_PE;
	
	if ((lsr & ASC_CON_FE))
		status |= ASC_STATUS_FE;
	
	if ((lsr & ASC_CON_OE)) 
		status |= ASC_STATUS_OE;
	
	return status;
}

static void asc_receive_chars(struct uart_port *port) {
	struct tty_port *tport = &port->state->port;
	u32 status, fifocnt;
	u8 flag, ch;
	bool ignore_pe = false;
	
	/*
	 * Datasheet states: If the MODE field selects an 8-bit frame then
	 * this [parity error] bit is undefined. Software should ignore this
	 * bit when reading 8-bit frames.
	 */
	if (asc_is_8bit_mode(port))
		ignore_pe = true;
	
	if (irqd_is_wakeup_set(irq_get_irq_data(port->irq)))
		pm_wakeup_event(tport->tty->dev, 0);
	
	fifocnt = asc_rx_fifo_avail(port);
	while (fifocnt--) {
		status = asc_get_status(port);
		ch = asc_in(port, ASC_RXB) & 0xFF;
		flag = TTY_NORMAL;
		port->icount.rx++;
		
		if ((status & ASC_STATUS_OE) || (status & ASC_STATUS_FE) || ((status & ASC_STATUS_PE) && !ignore_pe)) {
			if ((status & ASC_STATUS_FE)) {
				if (ch == 0x00) {
					port->icount.brk++;
					if (uart_handle_break(port))
						continue;
					status |= ASC_STATUS_BE;
				} else {
					port->icount.frame++;
				}
			} else if ((status & ASC_STATUS_PE)) {
				port->icount.parity++;
			}
			
			/*
			 * Reading any data from the RX FIFO clears the
			 * overflow error condition.
			 */
			if ((status & ASC_STATUS_OE))
				port->icount.overrun++;
			
			if ((status & ASC_STATUS_BE)) {
				flag = TTY_BREAK;
			} else if ((status & ASC_STATUS_PE)) {
				flag = TTY_PARITY;
			} else if ((status & ASC_STATUS_FE)) {
				flag = TTY_FRAME;
			}
		}
		
		status &= port->read_status_mask;
		
		if (uart_handle_sysrq_char(port, ch))
			continue;
		
		uart_insert_char(port, status, ASC_STATUS_OE, ch, flag);
	}
	
	/* Tell the rest of the system the news. New characters! */
	tty_flip_buffer_push(tport);
}

static irqreturn_t asc_interrupt_rx(int irq, void *ptr) {
	struct uart_port *port = ptr;
	
	spin_lock(&port->lock);
	writel_relaxed(ASC_ICR_RX, port->membase + ASC_ICR);
	asc_receive_chars(port);
	spin_unlock(&port->lock);
	
	return IRQ_HANDLED;
}

static irqreturn_t asc_interrupt_tx(int irq, void *ptr) {
	struct uart_port *port = ptr;
	
	spin_lock(&port->lock);
	writel_relaxed(ASC_ICR_TX, port->membase + ASC_ICR);
	asc_transmit_chars(port);
	spin_unlock(&port->lock);
	
	return IRQ_HANDLED;
}

static unsigned int asc_tx_empty(struct uart_port *port) {
	return asc_tx_fifo_avail(port) > 0 ? 0 : TIOCSER_TEMT;
}

static void asc_set_mctrl(struct uart_port *port, unsigned int mctrl) {
	// Not supported
}

static unsigned int asc_get_mctrl(struct uart_port *port) {
	// Not supported
	return TIOCM_CTS | TIOCM_CAR | TIOCM_DSR;
}

/* There are probably characters waiting to be transmitted. */
static void asc_start_tx(struct uart_port *port) {
	struct tty_port *tport = &port->state->port;
	if (!kfifo_is_empty(&tport->xmit_fifo))
		asc_enable_tx_interrupts(port);
}

/* Transmit stop */
static void asc_stop_tx(struct uart_port *port) {
	asc_disable_tx_interrupts(port);
}

/* Receive stop */
static void asc_stop_rx(struct uart_port *port) {
	asc_disable_rx_interrupts(port);
}

/* Handle breaks - ignored by us */
static void asc_break_ctl(struct uart_port *port, int break_state) {
	/* Nothing here yet .. */
}

/*
 * Enable port for reception.
 */
static int asc_startup(struct uart_port *port) {
	struct asc_port *ascport = to_asc_port(port);
	
	if (request_irq(ascport->irq_rx, asc_interrupt_rx, 0, asc_port_name(port), port)) {
		dev_err(port->dev, "cannot allocate RX irq.\n");
		return -ENODEV;
	}
	
	if (request_irq(ascport->irq_tx, asc_interrupt_tx, 0, asc_port_name(port), port)) {
		dev_err(port->dev, "cannot allocate TX irq.\n");
		free_irq(ascport->irq_rx, port);
		return -ENODEV;
	}
	
	asc_transmit_chars(port);
	asc_enable_rx_interrupts(port);
	
	return 0;
}

static void asc_shutdown(struct uart_port *port) {
	struct asc_port *ascport = to_asc_port(port);
	asc_disable_tx_interrupts(port);
	asc_disable_rx_interrupts(port);
	free_irq(ascport->irq_rx, port);
	free_irq(ascport->irq_tx, port);
}

static void asc_pm(struct uart_port *port, unsigned int state, unsigned int oldstate) {
	struct asc_port *ascport = to_asc_port(port);
	switch (state) {
		case UART_PM_STATE_ON:
			clk_prepare_enable(ascport->clk);
		break;
		case UART_PM_STATE_OFF:
			clk_disable_unprepare(ascport->clk);
		break;
	}
}

static u32 asc_calc_fdv_bg(struct uart_port *port, u32 baudrate, u32 *bg, u32 *fdv) {
	int div;
	u32 max_baudrate = port->uartclk / ASC_CLOCK_DIV;
	if (baudrate >= max_baudrate) { // Maximum baudrate
		*bg = 0;
		*fdv = 0;
		return max_baudrate;
	}
	
	if ((max_baudrate % baudrate) == 0) {
		// Exact baudrate
		*bg = (max_baudrate / baudrate) - 1;
		*fdv = 0;
		
		// Reducing BG by FDV if exceeded limit
		div = 256;
		while (*bg > ASC_MAX_BG) {
			*fdv = div;
			*bg >>= 1;
			div >>= 1;
		}
	} else {
		// Baudrate with approximation
		uint32_t good_baud = max_baudrate / (max_baudrate / baudrate);
		*bg = (max_baudrate / good_baud) - 1;
		*fdv = baudrate * 512 / good_baud;
		
		// Reducing BG by FDV if exceeded limit
		while (*bg > ASC_MAX_BG) {
			*bg >>= 1;
			*fdv >>= 1;
		}
	}
	
	// Real baudrate
	return *fdv ?
		(max_baudrate / (*bg + 1)) * *fdv / 512 :
		(max_baudrate / (*bg + 1));
}

static void asc_set_termios(struct uart_port *port, struct ktermios *termios, const struct ktermios *old) {
	struct asc_port *ascport = to_asc_port(port);
	u32 baud, fdv = 0, bg = 0, new_con = 0;
	unsigned long flags;
	
	// Get current usart clock
	port->uartclk = clk_get_rate(ascport->clk);
	
	// Recalc baudrate
	baud = uart_get_baud_rate(port, termios, old, 0, port->uartclk / ASC_CLOCK_DIV);
	baud = asc_calc_fdv_bg(port, baud, &fdv, &bg);
	
	spin_lock_irqsave(&port->lock, flags);
	
	// Update termios to reflect hardware capabilities
	termios->c_cflag &= ~(CMSPAR | CRTSCTS);
	
	// Char size (7, 8)
	if ((termios->c_cflag & CSIZE) == CS7) {
		new_con |= ASC_CON_M_ASYNC_PARITY_7BIT;
		termios->c_cflag |= PARENB; /* always parity */
	} else {
		new_con |= (termios->c_cflag & PARENB) ?  ASC_CON_M_ASYNC_PARITY_8BIT : ASC_CON_M_ASYNC_8BIT;
		termios->c_cflag &= ~CSIZE;
		termios->c_cflag |= CS8;
	}
	
	// Stop bits
	new_con |= (termios->c_cflag & CSTOPB) ? ASC_CON_STP_TWO : ASC_CON_STP_ONE;
	
	// Parity
	if ((termios->c_cflag & PARODD) && (termios->c_cflag & PARENB))
		new_con |= ASC_CON_ODD;
	
	// Enable receiver & enable FDV
	new_con |= ASC_CON_REN | ASC_CON_FDE;
	
	// Update configuration and run baudrate generator
	asc_out(port, ASC_CON, new_con);
	asc_out(port, ASC_FDV, fdv);
	asc_out(port, ASC_BG, bg);
	asc_out(port, ASC_CON, new_con | ASC_CON_CON_R);
	
	// Enable and flush FIFO
	asc_out(port, ASC_RXFCON, ASC_RXFCON_RXFEN | (1 << ASC_RXFCON_RXFITL_SHIFT) | ASC_RXFCON_RXFFLU);
	asc_out(port, ASC_TXFCON, ASC_TXFCON_TXFEN | (1 << ASC_TXFCON_TXFITL_SHIFT) | ASC_TXFCON_TXFFLU);
	
	port->read_status_mask = ASC_STATUS_OE;
	if ((termios->c_iflag & INPCK))
		port->read_status_mask |= ASC_STATUS_FE | ASC_STATUS_PE;
	if ((termios->c_iflag & (IGNBRK | BRKINT | PARMRK)))
		ascport->port.read_status_mask |= ASC_STATUS_BE;
	
	port->ignore_status_mask = 0;
	
	if ((termios->c_iflag & IGNPAR)) // Ignore parity errors
		port->ignore_status_mask |= ASC_STATUS_PE | ASC_STATUS_FE;
	
	if ((termios->c_iflag & IGNBRK)) {
		// If we're ignoring parity and break indicators, ignore overruns too (for real raw support).
		port->ignore_status_mask |= ASC_STATUS_BE;
		
		if ((termios->c_iflag & IGNPAR))
			port->ignore_status_mask |= ASC_STATUS_OE;
	}
	
	if (!(termios->c_cflag & CREAD)) // Ignore all data
		port->ignore_status_mask |= ASC_STATUS_RX;
	
	uart_update_timeout(port, termios->c_cflag, baud);
	
	spin_unlock_irqrestore(&port->lock, flags);
}

static const char *asc_type(struct uart_port *port) {
	return (port->type == PORT_ASC) ? DRIVER_NAME : NULL;
}

static void asc_release_port(struct uart_port *port) {
	
}

static int asc_request_port(struct uart_port *port) {
	return 0;
}

static void asc_config_port(struct uart_port *port, int flags) {
	if ((flags & UART_CONFIG_TYPE))
		port->type = PORT_ASC;
}

static int asc_verify_port(struct uart_port *port, struct serial_struct *ser) {
	return port->type != PORT_ASC ? -1 : 0;
}

#ifdef CONFIG_CONSOLE_POLL
static int asc_get_poll_char(struct uart_port *port) {
	if (!(asc_in(port, ASC_RIS) & ASC_RIS_RX))
		return NO_POLL_CHAR;
	asc_out(port, ASC_ICR, ASC_ICR_RX);
	return asc_in(port, ASC_RXB);
}

static void asc_put_poll_char(struct uart_port *port, unsigned char ch) {
	asc_out(port, ASC_TXB, ch);
	while (!(asc_in(port, ASC_RIS) & ASC_RIS_TX));
	asc_out(port, ASC_ICR, ASC_ICR_TX);
}
#endif /* CONFIG_CONSOLE_POLL */

static const struct uart_ops asc_uart_ops = {
	.tx_empty		= asc_tx_empty,
	.set_mctrl		= asc_set_mctrl,
	.get_mctrl		= asc_get_mctrl,
	.start_tx		= asc_start_tx,
	.stop_tx		= asc_stop_tx,
	.stop_rx		= asc_stop_rx,
	.break_ctl		= asc_break_ctl,
	.startup		= asc_startup,
	.shutdown		= asc_shutdown,
	.set_termios	= asc_set_termios,
	.type			= asc_type,
	.release_port	= asc_release_port,
	.request_port	= asc_request_port,
	.config_port	= asc_config_port,
	.verify_port	= asc_verify_port,
	.pm				= asc_pm,
#ifdef CONFIG_CONSOLE_POLL
	.poll_get_char	= asc_get_poll_char,
	.poll_put_char	= asc_put_poll_char,
#endif /* CONFIG_CONSOLE_POLL */
};

static int asc_init_port(struct asc_port *ascport, struct platform_device *pdev) {
	struct uart_port *port = &ascport->port;
	struct resource *res;
	int ret;
	
	ascport->irq_rx = platform_get_irq_byname(pdev, "rx");
	if (ascport->irq_rx < 0) {
		dev_err(&pdev->dev, "no RX irq");
		return -ENODEV;
	}
	
	ascport->irq_tx = platform_get_irq_byname(pdev, "tx");
	if (ascport->irq_tx < 0) {
		dev_err(&pdev->dev, "no TX irq");
		return -ENODEV;
	}
	
	port->iotype		= UPIO_MEM;
	port->flags			= UPF_BOOT_AUTOCONF;
	port->ops			= &asc_uart_ops;
	port->fifosize		= ASC_FIFO_SIZE;
	port->dev			= &pdev->dev;
	port->irq			= ascport->irq_rx;
	port->has_sysrq		= IS_ENABLED(CONFIG_SERIAL_PMB887X_CONSOLE);
	
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	port->membase = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(port->membase))
		return PTR_ERR(port->membase);
	port->mapbase = res->start;
	
	spin_lock_init(&port->lock);
	
	ascport->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(ascport->clk)) {
		dev_err(&pdev->dev, "no clk");
		return -EINVAL;
	}
	
	ascport->pinctrl = devm_pinctrl_get(&pdev->dev);
	if (IS_ERR(ascport->pinctrl)) {
		ret = PTR_ERR(ascport->pinctrl);
		dev_err(&pdev->dev, "Failed to get Pinctrl: %d\n", ret);
		return ret;
	}
	
	return 0;
}

static struct asc_port *asc_of_get_asc_port(struct platform_device *pdev) {
	struct device_node *np = pdev->dev.of_node;
	int id;
	
	if (!np)
		return NULL;
	
	id = of_alias_get_id(np, "serial");
	if (id < 0)
		id = pdev->id;
	if (id < 0)
		id = atomic_inc_return(&asc_uart_next_id) - 1;
	
	if (id >= ASC_MAX_PORTS) {
		dev_err(&pdev->dev, "Invalid port id: %d\n", id);
		return NULL;
	}
	
	dev_info(&pdev->dev, "detected port #%d\n", id);
	
	asc_ports[id].port.line = id;
	asc_ports[id].rts = NULL;
	
	return &asc_ports[id];
}

#ifdef CONFIG_OF
static const struct of_device_id asc_match[] = {
	{ .compatible = "pmb887x,asc", },
	{},
};

MODULE_DEVICE_TABLE(of, asc_match);
#endif

static int asc_serial_probe(struct platform_device *pdev) {
	int ret;
	struct asc_port *ascport;
	
	ascport = asc_of_get_asc_port(pdev);
	if (!ascport)
		return -ENODEV;
	
	ret = asc_init_port(ascport, pdev);
	if (ret)
		return ret;
	
	ret = uart_add_one_port(&asc_uart_driver, &ascport->port);
	if (ret)
		return ret;
	
	platform_set_drvdata(pdev, &ascport->port);
	
	return 0;
}

static void asc_serial_remove(struct platform_device *pdev) {
	struct uart_port *port = platform_get_drvdata(pdev);
	uart_remove_one_port(&asc_uart_driver, port);
}

#ifdef CONFIG_PM_SLEEP
static int asc_serial_suspend(struct device *dev) {
	struct uart_port *port = dev_get_drvdata(dev);
	return uart_suspend_port(&asc_uart_driver, port);
}

static int asc_serial_resume(struct device *dev) {
	struct uart_port *port = dev_get_drvdata(dev);
	return uart_resume_port(&asc_uart_driver, port);
}
#endif /* CONFIG_PM_SLEEP */

#ifdef CONFIG_SERIAL_PMB887X_CONSOLE
static void asc_console_putchar(struct uart_port *port, unsigned char ch) {
	u32 timeout = 1000000;
	while (timeout-- && asc_tx_fifo_is_full(port))
		udelay(1);
	asc_out(port, ASC_TXB, ch);
}

static void asc_console_write(struct console *co, const char *s, unsigned count) {
	struct uart_port *port = &asc_ports[co->index].port;
	unsigned long flags;
	int locked = 1;
	u32 intenable;
	
	if (port->sysrq) {
		locked = 0; /* asc_interrupt has already claimed the lock */
	} else if (oops_in_progress) {
		locked = spin_trylock_irqsave(&port->lock, flags);
	} else {
		spin_lock_irqsave(&port->lock, flags);
	}
	
	/*
	 * Disable interrupts so we don't get the IRQ line bouncing
	 * up and down while interrupts are disabled.
	 */
	intenable = asc_in(port, ASC_IMSC);
	asc_out(port, ASC_IMSC, 0);
	
	uart_console_write(port, s, count, asc_console_putchar);
	
	while (!asc_tx_fifo_is_empty(port))
		udelay(1);
	
	asc_out(port, ASC_IMSC, intenable);
	
	if (locked)
		spin_unlock_irqrestore(&port->lock, flags);
}

static int asc_console_setup(struct console *co, char *options) {
	struct asc_port *ascport;
	int baud = 115200;
	int bits = 8;
	int parity = 'n';
	int flow = 'n';
	
	if (co->index >= ASC_MAX_PORTS)
		return -ENODEV;
	
	ascport = &asc_ports[co->index];
	
	/*
	 * This driver does not support early console initialization
	 * (use ARM early printk support instead), so we only expect
	 * this to be called during the uart port registration when the
	 * driver gets probed and the port should be mapped at that point.
	 */
	if (ascport->port.mapbase == 0 || ascport->port.membase == NULL)
		return -ENXIO;
	
	if (options)
		uart_parse_options(options, &baud, &parity, &bits, &flow);
	
	return uart_set_options(&ascport->port, co, baud, parity, bits, flow);
}

static struct console asc_console = {
	.name		= ASC_SERIAL_NAME,
	.device		= uart_console_device,
	.write		= asc_console_write,
	.setup		= asc_console_setup,
	.flags		= CON_PRINTBUFFER,
	.index		= -1,
	.data		= &asc_uart_driver,
};
#define ASC_SERIAL_CONSOLE (&asc_console)

#else
#define ASC_SERIAL_CONSOLE NULL
#endif /* CONFIG_SERIAL_ST_ASC_CONSOLE */

#ifdef CONFIG_SERIAL_EARLYCON
static void early_asc_usart_console_putchar(struct uart_port *port, unsigned char ch) {
	asc_out(port, ASC_TXB, ch);
	while (!(asc_in(port, ASC_RIS) & ASC_RIS_TX));
	asc_out(port, ASC_ICR, ASC_ICR_TX);
}

static void early_asc_serial_write(struct console *console, const char *s, unsigned int count) {
	struct earlycon_device *device = console->data;
	struct uart_port *port = &device->port;
	uart_console_write(port, s, count, early_asc_usart_console_putchar);
}

static int __init early_asc_serial_setup(struct earlycon_device *device, const char *options) {
	if (!(device->port.membase || device->port.iobase))
		return -ENODEV;
	device->con->write = early_asc_serial_write;
	return 0;
}

OF_EARLYCON_DECLARE(asc, "pmb887x,asc", early_asc_serial_setup);
#endif /* CONFIG_SERIAL_EARLYCON */

static struct uart_driver asc_uart_driver = {
	.owner			= THIS_MODULE,
	.driver_name	= DRIVER_NAME,
	.dev_name		= ASC_SERIAL_NAME,
	.major			= TTY_MAJOR,
	.minor			= 64,
	.nr				= ASC_MAX_PORTS,
	.cons			= ASC_SERIAL_CONSOLE,
};

static struct platform_driver asc_serial_driver = {
	.probe		= asc_serial_probe,
	.remove		= asc_serial_remove,
	.driver	= {
		.name			= DRIVER_NAME,
		.of_match_table	= of_match_ptr(asc_match),
	},
};

static int __init asc_init(void) {
	int ret;
	
	ret = uart_register_driver(&asc_uart_driver);
	if (ret)
		return ret;
	
	ret = platform_driver_register(&asc_serial_driver);
	if (ret)
		uart_unregister_driver(&asc_uart_driver);
	
	return ret;
}

static void __exit asc_exit(void) {
	platform_driver_unregister(&asc_serial_driver);
	uart_unregister_driver(&asc_uart_driver);
}

module_init(asc_init);
module_exit(asc_exit);

MODULE_LICENSE("GPL");
