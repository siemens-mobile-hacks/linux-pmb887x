// SPDX-License-Identifier: GPL-2.0
/*
 * ILI9320 family backend for the Infineon PMB8876 DIFv2 display interface.
 *
 * Covers the controllers that address their frame memory through a register
 * index and one 16-bit value per coordinate: a window built from four
 * separate registers plus a cursor that has to be pointed at the corner the
 * address counter starts from, and rotation expressed as the AM and ID bits
 * of an entry mode register. R61505U/ILI9320, R63400 and JBT6K71 all work
 * this way and differ only in which register numbers they use, so a new one
 * is a descriptor rather than code.
 *
 * Copyright (C) 2026 Alula
 */

#include <linux/bitfield.h>
#include <linux/of.h>

#include "pmb887x_dif.h"

#define ILI9320_ENTRY_MODE_AM		BIT(3)	/* address counter update axis */
#define ILI9320_ENTRY_MODE_ID		GENMASK(5, 4)
#define ILI9320_ENTRY_MODE_BGR		BIT(12)
#define ILI9320_ENTRY_MODE_DFM		BIT(14)
#define ILI9320_ENTRY_MODE_TRI		BIT(15)

/**
 * struct pmb887x_panel_ili9320_regs - register numbers of one controller
 * @entry_mode: address counter direction, AM and ID
 * @display_ctrl: display on/off, cleared to blank the panel
 * @gram_x: cursor column
 * @gram_y: cursor row
 * @win_x1: first column of the window
 * @win_x2: last column of the window
 * @win_y1: first row of the window
 * @win_y2: last row of the window
 */
struct pmb887x_panel_ili9320_regs {
	u16 entry_mode;
	u16 display_ctrl;
	u16 gram_x;
	u16 gram_y;
	u16 win_x1;
	u16 win_x2;
	u16 win_y1;
	u16 win_y2;
};

/*
 * AM picks the axis the address counter walks first and ID picks the
 * direction on each axis, which is what lets the framebuffer be streamed out
 * in its own row order at no CPU cost.
 */
static int pmb887x_panel_ili9320_set_rotation(struct pmb887x_dif_panel *panel)
{
	const struct pmb887x_panel_ili9320_regs *regs = panel->desc->data;
	u16 value = ILI9320_ENTRY_MODE_DFM;

	switch (panel->rotation) {
	case 90:
		value |= ILI9320_ENTRY_MODE_AM |
			 FIELD_PREP(ILI9320_ENTRY_MODE_ID, 1);
		break;
	case 180:
		value |= FIELD_PREP(ILI9320_ENTRY_MODE_ID, 0);
		break;
	case 270:
		value |= ILI9320_ENTRY_MODE_AM |
			 FIELD_PREP(ILI9320_ENTRY_MODE_ID, 2);
		break;
	default:
		value |= FIELD_PREP(ILI9320_ENTRY_MODE_ID, 3);
		break;
	}

	/*
	 * The IL220 module wires the panel's colour order inverted, so BGR=1
	 * is what produces normal RGB. The bare-metal r61505_entry_mode() in
	 * pmb887x-emu performs the same inversion.
	 */
	if (panel->desc->bgr_is_rgb)
		value |= ILI9320_ENTRY_MODE_BGR;

	return pmb887x_dif_panel_write_reg(panel, regs->entry_mode, value);
}

static int pmb887x_panel_ili9320_set_window(struct pmb887x_dif_panel *panel,
					    const struct pmb887x_dif_panel_area *area)
{
	const struct pmb887x_panel_ili9320_regs *regs = panel->desc->data;
	int ret;

	ret = pmb887x_dif_panel_write_reg(panel, regs->win_x1, area->x1);
	if (!ret)
		ret = pmb887x_dif_panel_write_reg(panel, regs->win_x2, area->x2);
	if (!ret)
		ret = pmb887x_dif_panel_write_reg(panel, regs->win_y1, area->y1);
	if (!ret)
		ret = pmb887x_dif_panel_write_reg(panel, regs->win_y2, area->y2);
	if (!ret)
		ret = pmb887x_dif_panel_write_reg(panel, regs->gram_x, area->cursor_x);
	if (!ret)
		ret = pmb887x_dif_panel_write_reg(panel, regs->gram_y, area->cursor_y);

	return ret;
}

static int pmb887x_panel_ili9320_power_off(struct pmb887x_dif_panel *panel)
{
	const struct pmb887x_panel_ili9320_regs *regs = panel->desc->data;

	return pmb887x_dif_panel_write_reg(panel, regs->display_ctrl, 0x0000);
}

static const struct pmb887x_dif_panel_ops pmb887x_panel_ili9320_ops = {
	.set_rotation	= pmb887x_panel_ili9320_set_rotation,
	.set_window	= pmb887x_panel_ili9320_set_window,
	.power_off	= pmb887x_panel_ili9320_power_off,
};

static const struct pmb887x_panel_ili9320_regs pmb887x_panel_r61505u_regs = {
	.entry_mode	= 0x03,
	.display_ctrl	= 0x07,
	.gram_x		= 0x20,
	.gram_y		= 0x21,
	.win_x1		= 0x50,
	.win_x2		= 0x51,
	.win_y1		= 0x52,
	.win_y2		= 0x53,
};

static const struct pmb887x_dif_panel_desc pmb887x_panel_r61505u = {
	.name		= "R61505U/ILI9320",
	.width		= 240,
	.height		= 320,
	.width_mm	= 34,
	.height_mm	= 45,
	.cmd_width	= 2,
	.param_width	= 2,
	.gram_cmd	= 0x22,
	.bgr_is_rgb	= true,
	.ops		= &pmb887x_panel_ili9320_ops,
	.data		= &pmb887x_panel_r61505u_regs,
};

const struct of_device_id pmb887x_panel_ili9320_of_match[] = {
	{ .compatible = "renesas,r61505u", .data = &pmb887x_panel_r61505u },
	{ }
};
