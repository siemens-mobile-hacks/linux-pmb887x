// SPDX-License-Identifier: GPL-2.0-only
/*
 * Infineon PMB887x keypad scanner.
 *
 * The block scans the matrix by itself and latches the result into the PORT
 * registers, so the driver never drives the column lines. It only has to read
 * the latched matrix back and report whatever changed.
 *
 * The service requests are not trustworthy enough to drive that on their own.
 * The press request keeps re-asserting while a key is held, and the release
 * request can arrive before PORT reflects the release, which leaves the key
 * stuck down for good. The vendor driver deals with this by masking the
 * requests as soon as one fires and polling the matrix on a timer until it
 * reads empty, and this driver does the same: interrupts only wake it out of
 * idle, the poll loop does the reporting.
 *
 * Based on st-keyscan.c, copyright (c) 2014 STMicroelectronics Ltd,
 * author Stuart Menefy <stuart.menefy@st.com>.
 */

#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/input.h>
#include <linux/input/matrix_keypad.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/log2.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/workqueue.h>

#define PMB887X_KEYPAD_CON			0x10
#define PMB887X_KEYPAD_CON_NEXT_REPEAT_DELAY	GENMASK(3, 0)
#define PMB887X_KEYPAD_CON_FIRST_REPEAT_DELAY	GENMASK(15, 8)

#define PMB887X_KEYPAD_PORT(n)			(0x18 + (n) * 0x4)

#define PMB887X_KEYPAD_SRC(n)			(0xF0 + (n) * 0x4)
#define PMB887X_KEYPAD_SRC_SRPN			GENMASK(7, 0)
#define PMB887X_KEYPAD_SRC_TOS			GENMASK(11, 10)
#define PMB887X_KEYPAD_SRC_SRE			BIT(12)
#define PMB887X_KEYPAD_SRC_SRR			BIT(13)
#define PMB887X_KEYPAD_SRC_CLRR			BIT(14)
#define PMB887X_KEYPAD_SRC_SETR			BIT(15)

/* Service request nodes, in the order the interrupts are listed in the DT. */
#define PMB887X_KEYPAD_SRC_PRESS		0
#define PMB887X_KEYPAD_SRC_REPEAT		1
#define PMB887X_KEYPAD_SRC_INT2			2
#define PMB887X_KEYPAD_SRC_RELEASE		3
#define PMB887X_KEYPAD_NUM_SRC			4

/* Each PORT register latches four columns of eight active low input rows. */
#define PMB887X_KEYPAD_MAX_PORTS		3
#define PMB887X_KEYPAD_COLS_PER_PORT		4
#define PMB887X_KEYPAD_ROWS_PER_COL		8
#define PMB887X_KEYPAD_MAX_ROWS			PMB887X_KEYPAD_ROWS_PER_COL
#define PMB887X_KEYPAD_MAX_COLS \
	(PMB887X_KEYPAD_MAX_PORTS * PMB887X_KEYPAD_COLS_PER_PORT)

#define PMB887X_KEYPAD_DEFAULT_POLL_MS		20

/* The requests this driver arms; the repeat and internal ones stay masked. */
static const unsigned int pmb887x_keypad_srcs[] = {
	PMB887X_KEYPAD_SRC_PRESS,
	PMB887X_KEYPAD_SRC_RELEASE,
};

struct pmb887x_keypad {
	void __iomem *base;
	struct input_dev *input;
	struct delayed_work poll_work;
	unsigned int poll_ms;
	unsigned int n_rows;
	unsigned int n_cols;
	unsigned int n_ports;
	unsigned int row_shift;
	/* Bits of each PORT register that carry a key of this board. */
	u32 mask[PMB887X_KEYPAD_MAX_PORTS];
	u32 last_state[PMB887X_KEYPAD_MAX_PORTS];
};

/*
 * Drop the enable bit first and clear the pending request second, the way the
 * vendor driver does it. The register is written as a whole, so the priority
 * and type of service fields have to be carried over from the read.
 */
static void pmb887x_keypad_mask(struct pmb887x_keypad *keypad, unsigned int src)
{
	void __iomem *reg = keypad->base + PMB887X_KEYPAD_SRC(src);
	u32 val = readl(reg);

	writel(val & ~PMB887X_KEYPAD_SRC_SRE, reg);
	writel((val & ~(PMB887X_KEYPAD_SRC_SRE | PMB887X_KEYPAD_SRC_CLRR)) |
	       PMB887X_KEYPAD_SRC_CLRR, reg);
}

/*
 * A request raised while masked stays latched in SRR and is deliberately kept,
 * so a key pressed between the last scan and this write still wakes us.
 */
static void pmb887x_keypad_unmask(struct pmb887x_keypad *keypad,
				  unsigned int src)
{
	void __iomem *reg = keypad->base + PMB887X_KEYPAD_SRC(src);

	writel(readl(reg) | PMB887X_KEYPAD_SRC_SRE, reg);
}

static bool pmb887x_keypad_scan(struct pmb887x_keypad *keypad)
{
	struct input_dev *input = keypad->input;
	unsigned short *keycodes = input->keycode;
	unsigned int i, bit;
	bool pressed = false;
	bool sync = false;

	for (i = 0; i < keypad->n_ports; i++) {
		unsigned long state, change;

		/* The matrix is latched active low. */
		state = ~readl(keypad->base + PMB887X_KEYPAD_PORT(i)) &
			keypad->mask[i];
		change = state ^ keypad->last_state[i];
		keypad->last_state[i] = state;
		pressed |= state != 0;

		for_each_set_bit(bit, &change, 32) {
			unsigned int col, row, code;

			col = i * PMB887X_KEYPAD_COLS_PER_PORT +
			      bit / PMB887X_KEYPAD_ROWS_PER_COL;
			row = bit % PMB887X_KEYPAD_ROWS_PER_COL;
			code = MATRIX_SCAN_CODE(row, col, keypad->row_shift);

			input_report_key(input, keycodes[code],
					 state & BIT(bit));
			sync = true;
		}
	}

	if (sync)
		input_sync(input);

	return pressed;
}

static void pmb887x_keypad_poll(struct work_struct *work)
{
	struct pmb887x_keypad *keypad = container_of(to_delayed_work(work),
						     struct pmb887x_keypad,
						     poll_work);
	unsigned int i;

	if (pmb887x_keypad_scan(keypad)) {
		schedule_delayed_work(&keypad->poll_work,
				      msecs_to_jiffies(keypad->poll_ms));
		return;
	}

	/* The matrix is empty, so hand the wake-up back to the hardware. */
	for (i = 0; i < ARRAY_SIZE(pmb887x_keypad_srcs); i++)
		pmb887x_keypad_unmask(keypad, pmb887x_keypad_srcs[i]);
}

static irqreturn_t pmb887x_keypad_isr(int irq, void *dev_id)
{
	struct pmb887x_keypad *keypad = dev_id;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(pmb887x_keypad_srcs); i++)
		pmb887x_keypad_mask(keypad, pmb887x_keypad_srcs[i]);

	mod_delayed_work(system_percpu_wq, &keypad->poll_work, 0);

	return IRQ_HANDLED;
}

static void pmb887x_keypad_stop(struct pmb887x_keypad *keypad)
{
	unsigned int i;

	for (i = 0; i < PMB887X_KEYPAD_NUM_SRC; i++)
		pmb887x_keypad_mask(keypad, i);

	cancel_delayed_work_sync(&keypad->poll_work);
}

static void pmb887x_keypad_start(struct pmb887x_keypad *keypad)
{
	unsigned int i;

	/*
	 * Push the hardware repeat as far out as it goes. The input core does
	 * autorepeat for us and the repeat request stays masked, but the block
	 * keeps cycling its state machine either way.
	 */
	writel(FIELD_PREP(PMB887X_KEYPAD_CON_NEXT_REPEAT_DELAY, 0xF) |
	       FIELD_PREP(PMB887X_KEYPAD_CON_FIRST_REPEAT_DELAY, 0xFF),
	       keypad->base + PMB887X_KEYPAD_CON);

	for (i = 0; i < keypad->n_ports; i++)
		keypad->last_state[i] = 0;

	/*
	 * Run the poll loop once. It reports anything already held down and
	 * arms the requests once the matrix reads empty.
	 */
	mod_delayed_work(system_percpu_wq, &keypad->poll_work, 0);
}

static int pmb887x_keypad_open(struct input_dev *dev)
{
	pmb887x_keypad_start(input_get_drvdata(dev));

	return 0;
}

static void pmb887x_keypad_close(struct input_dev *dev)
{
	pmb887x_keypad_stop(input_get_drvdata(dev));
}

static int pmb887x_keypad_probe(struct platform_device *pdev)
{
	static const char * const irq_names[] = { "press", "release" };
	struct device *dev = &pdev->dev;
	struct pmb887x_keypad *keypad;
	struct input_dev *input;
	unsigned int i;
	int irq, error;

	keypad = devm_kzalloc(dev, sizeof(*keypad), GFP_KERNEL);
	if (!keypad)
		return -ENOMEM;

	keypad->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(keypad->base))
		return PTR_ERR(keypad->base);

	error = matrix_keypad_parse_properties(dev, &keypad->n_rows,
					       &keypad->n_cols);
	if (error)
		return error;

	if (keypad->n_rows > PMB887X_KEYPAD_MAX_ROWS ||
	    keypad->n_cols > PMB887X_KEYPAD_MAX_COLS)
		return dev_err_probe(dev, -EINVAL,
				     "matrix is %ux%u, controller scans at most %ux%u\n",
				     keypad->n_rows, keypad->n_cols,
				     PMB887X_KEYPAD_MAX_ROWS,
				     PMB887X_KEYPAD_MAX_COLS);

	keypad->poll_ms = PMB887X_KEYPAD_DEFAULT_POLL_MS;
	device_property_read_u32(dev, "poll-interval", &keypad->poll_ms);
	if (!keypad->poll_ms)
		return dev_err_probe(dev, -EINVAL, "poll-interval is zero\n");

	keypad->row_shift = get_count_order(keypad->n_cols);
	keypad->n_ports = DIV_ROUND_UP(keypad->n_cols,
				       PMB887X_KEYPAD_COLS_PER_PORT);

	for (i = 0; i < keypad->n_cols; i++)
		keypad->mask[i / PMB887X_KEYPAD_COLS_PER_PORT] |=
			GENMASK(keypad->n_rows - 1, 0)
			<< (i % PMB887X_KEYPAD_COLS_PER_PORT *
			    PMB887X_KEYPAD_ROWS_PER_COL);

	INIT_DELAYED_WORK(&keypad->poll_work, pmb887x_keypad_poll);

	input = devm_input_allocate_device(dev);
	if (!input)
		return -ENOMEM;

	input->name = "pmb887x-keypad";
	input->phys = "pmb887x-keypad/input0";
	input->id.bustype = BUS_HOST;
	input->open = pmb887x_keypad_open;
	input->close = pmb887x_keypad_close;

	keypad->input = input;
	input_set_drvdata(input, keypad);

	error = matrix_keypad_build_keymap(NULL, NULL, keypad->n_rows,
					   keypad->n_cols, NULL, input);
	if (error)
		return dev_err_probe(dev, error, "failed to build keymap\n");

	if (!device_property_read_bool(dev, "linux,no-autorepeat"))
		__set_bit(EV_REP, input->evbit);

	/* Keep the block quiet until the input device is opened. */
	pmb887x_keypad_stop(keypad);

	for (i = 0; i < ARRAY_SIZE(irq_names); i++) {
		irq = platform_get_irq_byname(pdev, irq_names[i]);
		if (irq < 0)
			return irq;

		error = devm_request_irq(dev, irq, pmb887x_keypad_isr, 0,
					 dev_name(dev), keypad);
		if (error)
			return dev_err_probe(dev, error,
					     "failed to request %s interrupt\n",
					     irq_names[i]);
	}

	error = input_register_device(input);
	if (error)
		return dev_err_probe(dev, error,
				     "failed to register input device\n");

	return 0;
}

static const struct of_device_id pmb887x_keypad_of_match[] = {
	{ .compatible = "infineon,pmb8876-keypad" },
	{}
};
MODULE_DEVICE_TABLE(of, pmb887x_keypad_of_match);

static struct platform_driver pmb887x_keypad_driver = {
	.probe = pmb887x_keypad_probe,
	.driver = {
		.name = "pmb887x-keypad",
		.of_match_table = pmb887x_keypad_of_match,
	},
};
module_platform_driver(pmb887x_keypad_driver);

MODULE_DESCRIPTION("Infineon PMB887x keypad driver");
MODULE_LICENSE("GPL");
