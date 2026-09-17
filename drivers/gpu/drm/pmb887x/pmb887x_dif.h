/* SPDX-License-Identifier: GPL-2.0 */
/*
 * DRM driver for the Infineon PMB8876 (S-Gold2) DIFv2 display interface
 * driving an MCU-parallel (Intel 8080 style) LCD.
 *
 * Copyright (C) 2026 Alula
 */
#ifndef _PMB887X_DIF_H
#define _PMB887X_DIF_H

#include <linux/completion.h>
#include <linux/mod_devicetable.h>
#include <linux/mutex.h>
#include <linux/scatterlist.h>
#include <linux/types.h>

#include <drm/drm_connector.h>
#include <drm/drm_crtc.h>
#include <drm/drm_device.h>
#include <drm/drm_encoder.h>
#include <drm/drm_modes.h>
#include <drm/drm_plane.h>

struct backlight_device;
struct clk;
struct device_node;
struct dma_chan;
struct drm_framebuffer;
struct drm_rect;
struct gpio_desc;
struct platform_device;

#define PMB887X_DIF_BITMUX_BITS		32

/* Widest command index and parameter list any supported controller uses. */
#define PMB887X_DIF_PANEL_CMD_BYTES	2
#define PMB887X_DIF_PANEL_MAX_PARAMS	4

/*
 * One contiguous run of framebuffer bytes. A damage rectangle becomes one
 * span per row, or a single span when it spans the full width of a linear
 * framebuffer.
 */
struct pmb887x_dif_span {
	u32 offset;
	u32 len;
};

/**
 * struct pmb887x_dif_format - a framebuffer format the bit multiplexer can
 *	turn into the panel's wire format with no CPU involvement
 * @fourcc: DRM_FORMAT_* this describes
 * @bsconf: DIF_CSREG_BSCONF_* value, i.e. how many bus bytes one 32-bit
 *	TXD stage emits
 * @pixels_per_word: pixels carried by one 32-bit stage; damage rectangles
 *	are aligned to this
 * @bitmux: for each of the 32 output bits, the input bit that feeds it
 */
struct pmb887x_dif_format {
	u32 fourcc;
	u32 bsconf;
	u8 pixels_per_word;
	u8 bitmux[PMB887X_DIF_BITMUX_BITS];
};

struct pmb887x_dif_panel;

/**
 * struct pmb887x_dif_panel_area - a damage rectangle in panel coordinates
 * @x1: first column, inclusive
 * @y1: first row, inclusive
 * @x2: last column, inclusive
 * @y2: last row, inclusive
 * @cursor_x: column the address counter starts at
 * @cursor_y: row the address counter starts at
 *
 * The generic layer turns the framebuffer damage rectangle into the panel's
 * own orientation and picks the corner the scan direction starts from, so a
 * backend only has to put these six numbers into its registers.
 */
struct pmb887x_dif_panel_area {
	u16 x1;
	u16 y1;
	u16 x2;
	u16 y2;
	u16 cursor_x;
	u16 cursor_y;
};

/**
 * struct pmb887x_dif_panel_ops - what a controller family does differently
 * @set_rotation: program the address counter direction for panel->rotation
 * @set_window: point the GRAM window and cursor at @area
 * @power_off: blank the panel, leaving it addressable
 *
 * Everything here runs once per damage rectangle at most, never per row and
 * never per pixel: the pixel stream itself goes straight from the framebuffer
 * to pmb887x_dif_blit() with no call through this vtable.
 *
 * All three are called with the interface lock held.
 */
struct pmb887x_dif_panel_ops {
	int (*set_rotation)(struct pmb887x_dif_panel *panel);
	int (*set_window)(struct pmb887x_dif_panel *panel,
			  const struct pmb887x_dif_panel_area *area);
	int (*power_off)(struct pmb887x_dif_panel *panel);
};

/**
 * struct pmb887x_dif_panel_desc - per-panel constants
 * @name: human readable panel name
 * @width: panel width in pixels, in the panel's own orientation
 * @height: panel height in pixels, in the panel's own orientation
 * @width_mm: physical width
 * @height_mm: physical height
 * @cmd_width: bytes a command index occupies on the wire, big endian
 * @param_width: bytes one command parameter occupies, big endian
 * @gram_cmd: command that starts a write into the panel's frame memory
 * @bgr_is_rgb: the module wires the colour order inverted, so the controller's
 *	BGR bit has to be set to get normal RGB output
 * @ops: controller family backing this panel
 * @data: register map or other constants the backend needs
 */
struct pmb887x_dif_panel_desc {
	const char *name;
	u16 width;
	u16 height;
	u16 width_mm;
	u16 height_mm;
	u8 cmd_width;
	u8 param_width;
	u32 gram_cmd;
	bool bgr_is_rgb;
	const struct pmb887x_dif_panel_ops *ops;
	const void *data;
};

/**
 * struct pmb887x_dif_panel - the panel hanging off the interface
 * @dif: interface it is wired to
 * @desc: panel constants, from the backend's match table
 * @rotation: rotation in degrees, from the device tree
 * @reset: reset line, may be NULL
 * @backlight: backlight device, may be NULL
 * @te_irq: tearing effect interrupt, <= 0 if the panel has no TE line
 * @te: completion the TE interrupt raises
 * @init_seq: init sequence from the device tree
 * @init_seq_len: length of @init_seq in bytes
 * @write_only: the bus is never read back
 * @continuous_splash: leave the panel alone on the first enable
 */
struct pmb887x_dif_panel {
	struct pmb887x_dif *dif;
	const struct pmb887x_dif_panel_desc *desc;

	unsigned int rotation;
	struct gpio_desc *reset;
	struct backlight_device *backlight;
	int te_irq;
	struct completion te;
	const u8 *init_seq;
	size_t init_seq_len;
	bool write_only;
	bool continuous_splash;
};

struct pmb887x_dif {
	struct drm_device drm;
	struct device *dev;

	void __iomem *base;
	phys_addr_t txd_phys;
	struct clk *clk;

	/* serialises every command and pixel transaction on the parallel bus */
	struct mutex lock;

	struct dma_chan *tx_chan;
	struct completion tx_done;
	struct sg_table sgt;

	struct pmb887x_dif_span *spans;
	unsigned int span_max;

	/* currently programmed bit multiplexer configuration, NULL if none */
	const struct pmb887x_dif_format *format;

	/* chip select every transaction is issued with, i.e. the panel's */
	u32 cs;

	struct pmb887x_dif_panel panel;
	bool enabled;

	struct drm_display_mode mode;
	struct drm_plane plane;
	struct drm_crtc crtc;
	struct drm_encoder encoder;
	struct drm_connector connector;
};

static inline struct pmb887x_dif *to_pmb887x_dif(struct drm_device *drm)
{
	return container_of(drm, struct pmb887x_dif, drm);
}

/* transport (pmb887x_dif.c) */
int pmb887x_dif_acquire(struct pmb887x_dif *dif, struct platform_device *pdev,
			unsigned int nspans);
int pmb887x_dif_hw_init(struct pmb887x_dif *dif);
void pmb887x_dif_hw_stop(struct pmb887x_dif *dif);
int pmb887x_dif_bitmux_selftest(struct device *dev);
const struct pmb887x_dif_format *pmb887x_dif_find_format(u32 fourcc);
int pmb887x_dif_set_format(struct pmb887x_dif *dif,
			   const struct pmb887x_dif_format *fmt);
int pmb887x_dif_write(struct pmb887x_dif *dif, const u8 *cmd, size_t ncmd,
		      const u8 *par, size_t npar);
int pmb887x_dif_sync(struct pmb887x_dif *dif);
int pmb887x_dif_blit(struct pmb887x_dif *dif, const u8 *cmd, size_t ncmd,
		     const struct pmb887x_dif_format *fmt, dma_addr_t dma,
		     const void *vaddr, const struct pmb887x_dif_span *spans,
		     unsigned int nspans);

/* panel (pmb887x_dif_panel.c) */
int pmb887x_dif_panel_parse(struct pmb887x_dif *dif, struct device_node *np);
int pmb887x_dif_panel_enable(struct pmb887x_dif *dif,
			     struct drm_framebuffer *fb);
void pmb887x_dif_panel_disable(struct pmb887x_dif *dif);
int pmb887x_dif_panel_flush(struct pmb887x_dif *dif, struct drm_framebuffer *fb,
			    struct drm_rect *rect);

/* command framing, for the backends */
int pmb887x_dif_panel_cmd(struct pmb887x_dif_panel *panel, u32 cmd,
			  const u16 *params, unsigned int nparams);

static inline int pmb887x_dif_panel_write_reg(struct pmb887x_dif_panel *panel,
					      u32 reg, u16 value)
{
	return pmb887x_dif_panel_cmd(panel, reg, &value, 1);
}

/* panel backends */
extern const struct of_device_id pmb887x_panel_ili9320_of_match[];

#endif /* _PMB887X_DIF_H */
