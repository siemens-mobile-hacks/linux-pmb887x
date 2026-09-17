// SPDX-License-Identifier: GPL-2.0
/*
 * Panel layer for the Infineon PMB8876 DIFv2 display interface.
 *
 * The controller-independent half: what the device tree describes, how a
 * command is framed on the bus, and the order a flush happens in. What a
 * particular controller family does with a rotation or a GRAM window lives
 * behind struct pmb887x_dif_panel_ops.
 *
 * Copyright (C) 2026 Alula
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/of.h>

#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_print.h>
#include <drm/drm_rect.h>

#include "pmb887x_dif.h"
#include "pmb887x_dif_regs.h"

/* Device tree init-sequence opcodes */
#define PMB887X_DIF_INIT_CMD		0x01
#define PMB887X_DIF_INIT_DELAY		0x02

#define PMB887X_DIF_RESET_US		10000
#define PMB887X_DIF_TE_TIMEOUT_MS	50

/*
 * One match table per controller family. A panel in a family that is already
 * supported is a descriptor and a compatible, with no code behind it.
 */
static const struct of_device_id * const pmb887x_dif_panel_matches[] = {
#if IS_ENABLED(CONFIG_DRM_PMB887X_PANEL_ILI9320)
	pmb887x_panel_ili9320_of_match,
#endif
	NULL,
};

static const struct pmb887x_dif_panel_desc *
pmb887x_dif_panel_match(struct device_node *np)
{
	const struct of_device_id *match;
	unsigned int i;

	for (i = 0; pmb887x_dif_panel_matches[i]; i++) {
		match = of_match_node(pmb887x_dif_panel_matches[i], np);
		if (match)
			return match->data;
	}

	return NULL;
}

static unsigned int pmb887x_dif_panel_pack_cmd(struct pmb887x_dif_panel *panel,
					       u32 cmd, u8 *buf)
{
	unsigned int width = panel->desc->cmd_width;
	unsigned int i;

	for (i = 0; i < width; i++)
		buf[i] = cmd >> (8 * (width - 1 - i));

	return width;
}

/**
 * pmb887x_dif_panel_cmd - send a command and its parameters to the panel
 * @panel: the panel
 * @cmd: command index, sent big endian in desc->cmd_width bytes
 * @params: parameters, each sent big endian in desc->param_width bytes
 * @nparams: number of parameters
 *
 * The framing is the one piece of a controller's command set that is pure
 * data, so every backend shares this.
 *
 * Returns 0 on success or a negative errno.
 */
int pmb887x_dif_panel_cmd(struct pmb887x_dif_panel *panel, u32 cmd,
			  const u16 *params, unsigned int nparams)
{
	u8 par[PMB887X_DIF_PANEL_MAX_PARAMS * sizeof(u16)];
	u8 buf[PMB887X_DIF_PANEL_CMD_BYTES];
	unsigned int i, ncmd, npar = 0;

	if (WARN_ON(nparams > PMB887X_DIF_PANEL_MAX_PARAMS))
		return -EINVAL;

	ncmd = pmb887x_dif_panel_pack_cmd(panel, cmd, buf);

	for (i = 0; i < nparams; i++) {
		if (panel->desc->param_width == sizeof(u16))
			par[npar++] = params[i] >> 8;
		par[npar++] = params[i];
	}

	return pmb887x_dif_write(panel->dif, buf, ncmd, par, npar);
}

/*
 * Rotation is done by the panel: the backend picks the axis the address
 * counter walks first and the direction it walks on each, so the framebuffer
 * can be streamed out in its own row order at no CPU cost. The damage
 * rectangle is transformed into panel coordinates to match, and the cursor
 * goes to the window corner the scan direction starts at.
 */
static void pmb887x_dif_panel_map_area(struct pmb887x_dif_panel *panel,
				       const struct drm_framebuffer *fb,
				       const struct drm_rect *rect,
				       struct pmb887x_dif_panel_area *area)
{
	switch (panel->rotation) {
	case 90:
		area->x1 = rect->y1;
		area->x2 = rect->y2 - 1;
		area->y1 = fb->width - rect->x2;
		area->y2 = fb->width - rect->x1 - 1;
		area->cursor_x = area->x1;
		area->cursor_y = area->y2;
		break;
	case 180:
		area->x1 = fb->width - rect->x2;
		area->x2 = fb->width - rect->x1 - 1;
		area->y1 = fb->height - rect->y2;
		area->y2 = fb->height - rect->y1 - 1;
		area->cursor_x = area->x2;
		area->cursor_y = area->y2;
		break;
	case 270:
		area->x1 = fb->height - rect->y2;
		area->x2 = fb->height - rect->y1 - 1;
		area->y1 = rect->x1;
		area->y2 = rect->x2 - 1;
		area->cursor_x = area->x2;
		area->cursor_y = area->y1;
		break;
	default:
		area->x1 = rect->x1;
		area->x2 = rect->x2 - 1;
		area->y1 = rect->y1;
		area->y2 = rect->y2 - 1;
		area->cursor_x = area->x1;
		area->cursor_y = area->y1;
		break;
	}
}

static void pmb887x_dif_panel_reset(struct pmb887x_dif_panel *panel)
{
	if (!panel->reset)
		return;

	gpiod_set_value_cansleep(panel->reset, 1);
	fsleep(PMB887X_DIF_RESET_US);
	gpiod_set_value_cansleep(panel->reset, 0);
	fsleep(PMB887X_DIF_RESET_US);
}

static int pmb887x_dif_panel_run_init(struct pmb887x_dif_panel *panel)
{
	struct pmb887x_dif *dif = panel->dif;
	const u8 *seq = panel->init_seq;
	size_t pos = 0;
	int ret;

	/* The stream was fully validated at probe time. */
	while (pos < panel->init_seq_len) {
		if (seq[pos] == PMB887X_DIF_INIT_CMD) {
			u8 ncmd = seq[pos + 1];
			u8 npar = seq[pos + 2];

			ret = pmb887x_dif_write(dif, &seq[pos + 3], ncmd,
						&seq[pos + 3 + ncmd], npar);
			if (ret)
				return ret;

			pos += 3 + ncmd + npar;
		} else {
			ret = pmb887x_dif_sync(dif);
			if (ret)
				return ret;

			fsleep(seq[pos + 1] * USEC_PER_MSEC);
			pos += 2;
		}
	}

	return pmb887x_dif_sync(dif);
}

static void pmb887x_dif_panel_wait_te(struct pmb887x_dif_panel *panel)
{
	if (panel->te_irq <= 0)
		return;

	reinit_completion(&panel->te);
	enable_irq(panel->te_irq);
	if (!wait_for_completion_timeout(&panel->te,
					 msecs_to_jiffies(PMB887X_DIF_TE_TIMEOUT_MS)))
		drm_dbg_kms(&panel->dif->drm, "no tearing effect pulse, flushing anyway\n");
	disable_irq(panel->te_irq);
}

/**
 * pmb887x_dif_panel_flush - push a damage rectangle to the panel
 * @dif: display interface
 * @fb: framebuffer to read from
 * @rect: damage rectangle in framebuffer coordinates
 *
 * Pixels are streamed straight out of the framebuffer; the bit multiplexer
 * turns them into the panel's wire format on the way, so nothing is copied
 * or converted by the CPU.
 *
 * Returns 0 on success or a negative errno.
 */
int pmb887x_dif_panel_flush(struct pmb887x_dif *dif, struct drm_framebuffer *fb,
			    struct drm_rect *rect)
{
	struct pmb887x_dif_panel *panel = &dif->panel;
	u8 gram_cmd[PMB887X_DIF_PANEL_CMD_BYTES];
	const struct pmb887x_dif_format *fmt;
	struct pmb887x_dif_panel_area area;
	struct drm_gem_dma_object *obj;
	unsigned int cpp, pitch, nspans;
	struct drm_rect r = *rect;
	dma_addr_t dma;
	const void *vaddr;
	int y, width, ncmd, ret;

	fmt = pmb887x_dif_find_format(fb->format->format);
	if (WARN_ON(!fmt))
		return -EINVAL;

	/*
	 * One 32-bit FIFO stage carries a whole number of pixels, so widen
	 * the rectangle until both edges land on a stage boundary.
	 */
	r.x1 = ALIGN_DOWN(r.x1, fmt->pixels_per_word);
	r.x2 = ALIGN(r.x2, fmt->pixels_per_word);

	cpp = fb->format->cpp[0];
	pitch = fb->pitches[0];
	width = drm_rect_width(&r);

	obj = drm_fb_dma_get_gem_obj(fb, 0);
	dma = obj->dma_addr + fb->offsets[0];
	vaddr = obj->vaddr ? obj->vaddr + fb->offsets[0] : NULL;

	if (width == fb->width && pitch == fb->width * cpp) {
		/* The rows are back to back, so the rectangle is one transfer. */
		dif->spans[0].offset = r.y1 * pitch;
		dif->spans[0].len = drm_rect_height(&r) * pitch;
		nspans = 1;
	} else {
		nspans = drm_rect_height(&r);
		if (WARN_ON(nspans > dif->span_max))
			return -EINVAL;

		for (y = 0; y < nspans; y++) {
			dif->spans[y].offset = (r.y1 + y) * pitch + r.x1 * cpp;
			dif->spans[y].len = width * cpp;
		}
	}

	pmb887x_dif_panel_map_area(panel, fb, &r, &area);
	ncmd = pmb887x_dif_panel_pack_cmd(panel, panel->desc->gram_cmd, gram_cmd);

	mutex_lock(&dif->lock);

	/*
	 * Idling the crossbar takes a RUNCTRL cycle, which pmb887x_dif_write()
	 * would otherwise do from inside the tearing window.
	 */
	ret = pmb887x_dif_set_format(dif, NULL);
	if (ret)
		goto out;

	pmb887x_dif_panel_wait_te(panel);

	ret = panel->desc->ops->set_window(panel, &area);
	if (ret)
		goto out;

	ret = pmb887x_dif_blit(dif, gram_cmd, ncmd, fmt, dma, vaddr,
			       dif->spans, nspans);
out:
	mutex_unlock(&dif->lock);

	return ret;
}

/**
 * pmb887x_dif_panel_enable - power the panel up and show the first frame
 * @dif: display interface
 * @fb: framebuffer holding the first frame, may be NULL
 *
 * Nothing reaches the screen until the very end: the panel is reset and
 * initialised, the frame memory is overwritten with the (blank) framebuffer
 * and only then is the backlight switched on. With a boot loader splash to
 * preserve, the reset, the init sequence and the frame write are all skipped.
 *
 * Returns 0 on success or a negative errno.
 */
int pmb887x_dif_panel_enable(struct pmb887x_dif *dif, struct drm_framebuffer *fb)
{
	struct pmb887x_dif_panel *panel = &dif->panel;
	bool takeover = panel->continuous_splash;
	int ret;

	mutex_lock(&dif->lock);

	if (!takeover) {
		pmb887x_dif_panel_reset(panel);

		ret = pmb887x_dif_panel_run_init(panel);
		if (ret)
			goto out;
	}

	ret = panel->desc->ops->set_rotation(panel);
	if (!ret)
		ret = pmb887x_dif_sync(dif);
out:
	mutex_unlock(&dif->lock);
	if (ret)
		return ret;

	if (!takeover && fb) {
		struct drm_rect rect = {
			.x1 = 0,
			.y1 = 0,
			.x2 = fb->width,
			.y2 = fb->height,
		};

		ret = pmb887x_dif_panel_flush(dif, fb, &rect);
		if (ret)
			return ret;
	}

	/* The panel is only left untouched on the first enable. */
	panel->continuous_splash = false;

	return backlight_enable(panel->backlight);
}

void pmb887x_dif_panel_disable(struct pmb887x_dif *dif)
{
	struct pmb887x_dif_panel *panel = &dif->panel;

	backlight_disable(panel->backlight);

	mutex_lock(&dif->lock);
	panel->desc->ops->power_off(panel);
	pmb887x_dif_sync(dif);
	mutex_unlock(&dif->lock);
}

static irqreturn_t pmb887x_dif_panel_te_irq(int irq, void *data)
{
	struct pmb887x_dif_panel *panel = data;

	complete(&panel->te);

	return IRQ_HANDLED;
}

static int pmb887x_dif_panel_check_init_seq(struct device *dev, const u8 *seq,
					    size_t len)
{
	size_t pos = 0;

	while (pos < len) {
		switch (seq[pos]) {
		case PMB887X_DIF_INIT_CMD:
			if (pos + 3 > len)
				return dev_err_probe(dev, -EINVAL,
						     "init sequence: truncated command header at %zu\n",
						     pos);
			if (!seq[pos + 1])
				return dev_err_probe(dev, -EINVAL,
						     "init sequence: command at %zu has no command bytes\n",
						     pos);
			if (pos + 3 + seq[pos + 1] + seq[pos + 2] > len)
				return dev_err_probe(dev, -EINVAL,
						     "init sequence: command at %zu runs past the end\n",
						     pos);
			pos += 3 + seq[pos + 1] + seq[pos + 2];
			break;
		case PMB887X_DIF_INIT_DELAY:
			if (pos + 2 > len)
				return dev_err_probe(dev, -EINVAL,
						     "init sequence: truncated delay at %zu\n",
						     pos);
			pos += 2;
			break;
		default:
			return dev_err_probe(dev, -EINVAL,
					     "init sequence: unknown opcode %#04x at %zu\n",
					     seq[pos], pos);
		}
	}

	return 0;
}

static int pmb887x_dif_panel_parse_init_seq(struct pmb887x_dif_panel *panel,
					    struct device_node *np)
{
	struct device *dev = panel->dif->dev;
	int len, ret;
	u8 *seq;

	len = of_property_count_u8_elems(np, "infineon,init-sequence");
	if (len == -EINVAL)
		return 0;
	if (len < 0)
		return dev_err_probe(dev, len, "bad infineon,init-sequence\n");

	seq = devm_kmalloc(dev, len, GFP_KERNEL);
	if (!seq)
		return -ENOMEM;

	ret = of_property_read_u8_array(np, "infineon,init-sequence", seq, len);
	if (ret)
		return ret;

	ret = pmb887x_dif_panel_check_init_seq(dev, seq, len);
	if (ret)
		return ret;

	panel->init_seq = seq;
	panel->init_seq_len = len;

	return 0;
}

static void pmb887x_dif_panel_put_backlight(void *data)
{
	put_device(&((struct backlight_device *)data)->dev);
}

/*
 * devm_of_find_backlight() open-coded against the panel child node: the
 * backlight phandle lives there rather than on the interface node this
 * driver is bound to.
 */
static int pmb887x_dif_panel_get_backlight(struct pmb887x_dif_panel *panel,
					   struct device_node *np)
{
	struct device *dev = panel->dif->dev;
	struct backlight_device *bd;
	struct device_node *bl_np;

	bl_np = of_parse_phandle(np, "backlight", 0);
	if (!bl_np)
		return 0;

	bd = of_find_backlight_by_node(bl_np);
	of_node_put(bl_np);
	if (!bd)
		return dev_err_probe(dev, -EPROBE_DEFER,
				     "waiting for the backlight device\n");

	panel->backlight = bd;

	return devm_add_action_or_reset(dev, pmb887x_dif_panel_put_backlight, bd);
}

/**
 * pmb887x_dif_panel_parse - collect everything the panel child node describes
 * @dif: display interface
 * @np: the panel node
 *
 * This runs before the interface is brought up and deliberately leaves the
 * panel alone: the reset line is claimed in its deasserted state and no byte
 * is put on the bus, so a boot loader splash survives probing intact.
 *
 * Returns 0 on success or a negative errno.
 */
int pmb887x_dif_panel_parse(struct pmb887x_dif *dif, struct device_node *np)
{
	struct pmb887x_dif_panel *panel = &dif->panel;
	struct device *dev = dif->dev;
	struct gpio_desc *te;
	u32 reg, rotation = 0;
	int ret;

	panel->dif = dif;

	panel->desc = pmb887x_dif_panel_match(np);
	if (!panel->desc)
		return dev_err_probe(dev, -ENODEV, "unsupported panel %pOF\n", np);

	if (WARN_ON(panel->desc->cmd_width > PMB887X_DIF_PANEL_CMD_BYTES ||
		    panel->desc->param_width > sizeof(u16)))
		return -EINVAL;

	ret = of_property_read_u32(np, "reg", &reg);
	if (ret)
		return dev_err_probe(dev, ret, "panel %pOF has no chip select\n", np);
	if (reg != 1 && reg != 2)
		return dev_err_probe(dev, -EINVAL,
				     "chip select %u is not routed on this SoC\n", reg);
	dif->cs = DIF_CSREG_CS1 << (reg - 1);

	of_property_read_u32(np, "rotation", &rotation);
	if (rotation % 90 || rotation > 270)
		return dev_err_probe(dev, -EINVAL, "bad rotation %u\n", rotation);
	panel->rotation = rotation;

	panel->continuous_splash = of_property_read_bool(np, "infineon,continuous-splash");

	panel->write_only = of_property_read_bool(np, "write-only");
	if (!panel->write_only)
		dev_info(dev, "panel readback is not implemented, treating the bus as write-only\n");

	/*
	 * Logical low is the deasserted state of a reset-gpios line, so this
	 * claims the pad exactly as the boot-time pin configuration left it.
	 * Reset is only ever asserted from pmb887x_dif_panel_reset(), which
	 * runs when the panel is being initialised from scratch.
	 */
	panel->reset = devm_fwnode_gpiod_get(dev, of_fwnode_handle(np), "reset",
					     GPIOD_OUT_LOW, "panel-reset");
	if (IS_ERR(panel->reset)) {
		ret = PTR_ERR(panel->reset);
		if (ret != -ENOENT)
			return dev_err_probe(dev, ret, "failed to get the reset GPIO\n");
		panel->reset = NULL;
	}

	ret = pmb887x_dif_panel_get_backlight(panel, np);
	if (ret)
		return ret;

	ret = pmb887x_dif_panel_parse_init_seq(panel, np);
	if (ret)
		return ret;

	if (!panel->init_seq && !panel->continuous_splash)
		return dev_err_probe(dev, -EINVAL,
				     "panel %pOF needs an init sequence\n", np);

	init_completion(&panel->te);

	te = devm_fwnode_gpiod_get(dev, of_fwnode_handle(np), "te", GPIOD_IN,
				   "panel-te");
	if (IS_ERR(te)) {
		ret = PTR_ERR(te);
		if (ret != -ENOENT)
			return dev_err_probe(dev, ret, "failed to get the TE GPIO\n");
		return 0;
	}

	panel->te_irq = gpiod_to_irq(te);
	if (panel->te_irq < 0)
		return dev_err_probe(dev, panel->te_irq,
				     "the TE GPIO cannot raise interrupts\n");

	return devm_request_irq(dev, panel->te_irq, pmb887x_dif_panel_te_irq,
				IRQF_TRIGGER_RISING | IRQF_NO_AUTOEN,
				"panel-te", panel);
}
