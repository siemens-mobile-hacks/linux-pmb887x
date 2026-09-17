// SPDX-License-Identifier: GPL-2.0
/*
 * KMS layer and platform glue for the Infineon PMB8876 DIFv2 display
 * interface.
 *
 * Copyright (C) 2026 Alula
 */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include <drm/clients/drm_client_setup.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_fbdev_dma.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_print.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_rect.h>

#include "pmb887x_dif.h"

/* RGB565 first: it is what the bit multiplexer converts most efficiently. */
static const u32 pmb887x_dif_plane_formats[] = {
	DRM_FORMAT_RGB565,
	DRM_FORMAT_XRGB8888,
};

static const u64 pmb887x_dif_plane_format_modifiers[] = {
	DRM_FORMAT_MOD_LINEAR,
	DRM_FORMAT_MOD_INVALID,
};

static int pmb887x_dif_plane_atomic_check(struct drm_plane *plane,
					  struct drm_atomic_commit *state)
{
	struct drm_plane_state *new_state = drm_atomic_get_new_plane_state(state, plane);
	const struct pmb887x_dif_format *fmt;
	struct drm_crtc_state *crtc_state = NULL;
	struct drm_framebuffer *fb;
	int ret;

	if (new_state->crtc)
		crtc_state = drm_atomic_get_new_crtc_state(state, new_state->crtc);

	ret = drm_atomic_helper_check_plane_state(new_state, crtc_state,
						  DRM_PLANE_NO_SCALING,
						  DRM_PLANE_NO_SCALING,
						  false, false);
	if (ret || !new_state->visible)
		return ret;

	fb = new_state->fb;
	fmt = pmb887x_dif_find_format(fb->format->format);
	if (!fmt)
		return -EINVAL;

	/*
	 * Pixels go to the panel as whole 32-bit framebuffer words, so every
	 * row has to start on a word boundary and hold a whole number of the
	 * pixel groups one word carries.
	 */
	if (fb->pitches[0] % sizeof(u32) || fb->width % fmt->pixels_per_word)
		return -EINVAL;

	return 0;
}

static void pmb887x_dif_plane_atomic_update(struct drm_plane *plane,
					    struct drm_atomic_commit *state)
{
	struct drm_plane_state *old_state = drm_atomic_get_old_plane_state(state, plane);
	struct drm_plane_state *new_state = plane->state;
	struct drm_device *drm = plane->dev;
	struct pmb887x_dif *dif = to_pmb887x_dif(drm);
	struct drm_rect rect;
	int idx, ret;

	/*
	 * Planes are committed before the CRTC is enabled, so on the enabling
	 * commit there is nothing to flush into yet - pmb887x_dif_panel_enable()
	 * writes the first frame itself once the panel is alive.
	 */
	if (!new_state->fb || !dif->enabled)
		return;

	if (!drm_dev_enter(drm, &idx))
		return;

	if (drm_atomic_helper_damage_merged(old_state, new_state, &rect)) {
		ret = pmb887x_dif_panel_flush(dif, new_state->fb, &rect);
		if (ret)
			drm_err_once(drm, "failed to flush the display: %d\n", ret);
	}

	drm_dev_exit(idx);
}

static const struct drm_plane_helper_funcs pmb887x_dif_plane_helper_funcs = {
	.prepare_fb = drm_gem_plane_helper_prepare_fb,
	.atomic_check = pmb887x_dif_plane_atomic_check,
	.atomic_update = pmb887x_dif_plane_atomic_update,
};

static const struct drm_plane_funcs pmb887x_dif_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = drm_plane_cleanup,
	.reset = drm_atomic_helper_plane_reset,
	.atomic_duplicate_state = drm_atomic_helper_plane_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_plane_destroy_state,
};

static enum drm_mode_status pmb887x_dif_crtc_mode_valid(struct drm_crtc *crtc,
							const struct drm_display_mode *mode)
{
	struct pmb887x_dif *dif = to_pmb887x_dif(crtc->dev);

	return drm_crtc_helper_mode_valid_fixed(crtc, mode, &dif->mode);
}

static int pmb887x_dif_crtc_atomic_check(struct drm_crtc *crtc,
					 struct drm_atomic_commit *state)
{
	struct drm_crtc_state *crtc_state = drm_atomic_get_new_crtc_state(state, crtc);
	int ret;

	if (crtc_state->enable) {
		ret = drm_atomic_helper_check_crtc_primary_plane(crtc_state);
		if (ret)
			return ret;
	}

	return drm_atomic_add_affected_planes(state, crtc);
}

static void pmb887x_dif_crtc_atomic_enable(struct drm_crtc *crtc,
					   struct drm_atomic_commit *state)
{
	struct drm_device *drm = crtc->dev;
	struct pmb887x_dif *dif = to_pmb887x_dif(drm);
	struct drm_plane_state *plane_state;
	int idx, ret;

	if (!drm_dev_enter(drm, &idx))
		return;

	plane_state = drm_atomic_get_new_plane_state(state, crtc->primary);

	ret = pmb887x_dif_panel_enable(dif, plane_state ? plane_state->fb : NULL);
	if (ret)
		drm_err(drm, "failed to enable the panel: %d\n", ret);
	else
		dif->enabled = true;

	drm_dev_exit(idx);
}

static void pmb887x_dif_crtc_atomic_disable(struct drm_crtc *crtc,
					    struct drm_atomic_commit *state)
{
	struct pmb887x_dif *dif = to_pmb887x_dif(crtc->dev);

	/*
	 * Not guarded by drm_dev_enter(): the display still has to be turned
	 * off on a regular driver unload, and the interface is memory mapped
	 * for as long as the platform device lives.
	 */
	dif->enabled = false;
	pmb887x_dif_panel_disable(dif);
}

static const struct drm_crtc_helper_funcs pmb887x_dif_crtc_helper_funcs = {
	.mode_valid = pmb887x_dif_crtc_mode_valid,
	.atomic_check = pmb887x_dif_crtc_atomic_check,
	.atomic_enable = pmb887x_dif_crtc_atomic_enable,
	.atomic_disable = pmb887x_dif_crtc_atomic_disable,
};

static const struct drm_crtc_funcs pmb887x_dif_crtc_funcs = {
	.reset = drm_atomic_helper_crtc_reset,
	.destroy = drm_crtc_cleanup,
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
};

static const struct drm_encoder_funcs pmb887x_dif_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
};

static int pmb887x_dif_connector_get_modes(struct drm_connector *connector)
{
	struct pmb887x_dif *dif = to_pmb887x_dif(connector->dev);

	return drm_connector_helper_get_modes_fixed(connector, &dif->mode);
}

static const struct drm_connector_helper_funcs pmb887x_dif_connector_helper_funcs = {
	.get_modes = pmb887x_dif_connector_get_modes,
};

static const struct drm_connector_funcs pmb887x_dif_connector_funcs = {
	.reset = drm_atomic_helper_connector_reset,
	.destroy = drm_connector_cleanup,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static const struct drm_mode_config_funcs pmb887x_dif_mode_config_funcs = {
	.fb_create = drm_gem_fb_create_with_dirty,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

static const struct drm_mode_config_helper_funcs pmb887x_dif_mode_config_helper_funcs = {
	.atomic_commit_tail = drm_atomic_helper_commit_tail_rpm,
};

DEFINE_DRM_GEM_DMA_FOPS(pmb887x_dif_fops);

static const struct drm_driver pmb887x_dif_driver = {
	.driver_features	= DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	.fops			= &pmb887x_dif_fops,
	DRM_GEM_DMA_DRIVER_OPS_VMAP,
	DRM_FBDEV_DMA_DRIVER_OPS,
	.name			= "pmb887x-dif",
	.desc			= "Infineon PMB8876 DIFv2",
	.major			= 1,
	.minor			= 0,
};

static int pmb887x_dif_modeset_init(struct pmb887x_dif *dif)
{
	struct drm_device *drm = &dif->drm;
	int ret;

	ret = drmm_mode_config_init(drm);
	if (ret)
		return ret;

	drm->mode_config.min_width = dif->mode.hdisplay;
	drm->mode_config.max_width = dif->mode.hdisplay;
	drm->mode_config.min_height = dif->mode.vdisplay;
	drm->mode_config.max_height = dif->mode.vdisplay;
	drm->mode_config.preferred_depth = 16;
	drm->mode_config.funcs = &pmb887x_dif_mode_config_funcs;
	drm->mode_config.helper_private = &pmb887x_dif_mode_config_helper_funcs;

	ret = drm_universal_plane_init(drm, &dif->plane, 0,
				       &pmb887x_dif_plane_funcs,
				       pmb887x_dif_plane_formats,
				       ARRAY_SIZE(pmb887x_dif_plane_formats),
				       pmb887x_dif_plane_format_modifiers,
				       DRM_PLANE_TYPE_PRIMARY, NULL);
	if (ret)
		return ret;
	drm_plane_helper_add(&dif->plane, &pmb887x_dif_plane_helper_funcs);
	drm_plane_enable_fb_damage_clips(&dif->plane);

	ret = drm_crtc_init_with_planes(drm, &dif->crtc, &dif->plane, NULL,
					&pmb887x_dif_crtc_funcs, NULL);
	if (ret)
		return ret;
	drm_crtc_helper_add(&dif->crtc, &pmb887x_dif_crtc_helper_funcs);

	ret = drm_encoder_init(drm, &dif->encoder, &pmb887x_dif_encoder_funcs,
			       DRM_MODE_ENCODER_NONE, NULL);
	if (ret)
		return ret;
	dif->encoder.possible_crtcs = drm_crtc_mask(&dif->crtc);

	ret = drm_connector_init(drm, &dif->connector,
				 &pmb887x_dif_connector_funcs,
				 DRM_MODE_CONNECTOR_DPI);
	if (ret)
		return ret;
	drm_connector_helper_add(&dif->connector,
				 &pmb887x_dif_connector_helper_funcs);

	ret = drm_connector_attach_encoder(&dif->connector, &dif->encoder);
	if (ret)
		return ret;

	drm_mode_config_reset(drm);

	return 0;
}

static void pmb887x_dif_init_mode(struct pmb887x_dif *dif)
{
	const struct pmb887x_dif_panel_desc *desc = dif->panel.desc;
	struct drm_display_mode mode = {
		DRM_SIMPLE_MODE(desc->width, desc->height,
				desc->width_mm, desc->height_mm),
	};

	/* Rotation happens inside the panel, so only the reported size flips. */
	if (dif->panel.rotation == 90 || dif->panel.rotation == 270) {
		swap(mode.hdisplay, mode.vdisplay);
		swap(mode.hsync_start, mode.vsync_start);
		swap(mode.hsync_end, mode.vsync_end);
		swap(mode.htotal, mode.vtotal);
		swap(mode.width_mm, mode.height_mm);
	}

	dif->mode = mode;
}

static int pmb887x_dif_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *panel_np;
	struct pmb887x_dif *dif;
	struct drm_device *drm;
	int ret;

	ret = pmb887x_dif_bitmux_selftest(dev);
	if (ret)
		return ret;

	dif = devm_drm_dev_alloc(dev, &pmb887x_dif_driver, struct pmb887x_dif, drm);
	if (IS_ERR(dif))
		return PTR_ERR(dif);

	drm = &dif->drm;
	dif->dev = dev;
	init_completion(&dif->tx_done);

	ret = drmm_mutex_init(drm, &dif->lock);
	if (ret)
		return ret;

	panel_np = of_get_next_available_child(dev->of_node, NULL);
	if (!panel_np)
		return dev_err_probe(dev, -ENODEV, "no panel child node\n");

	ret = pmb887x_dif_panel_parse(dif, panel_np);
	of_node_put(panel_np);
	if (ret)
		return ret;

	pmb887x_dif_init_mode(dif);

	/* A damage rectangle is at most one span per framebuffer row. */
	ret = pmb887x_dif_acquire(dif, pdev,
				  max(dif->panel.desc->width,
				      dif->panel.desc->height));
	if (ret)
		return ret;

	ret = pmb887x_dif_hw_init(dif);
	if (ret)
		return ret;

	ret = pmb887x_dif_modeset_init(dif);
	if (ret)
		return ret;

	ret = drm_dev_register(drm, 0);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, drm);

	drm_client_setup(drm, NULL);

	dev_info(dev, "%s panel on chip select %u, %ux%u, %s transfers\n",
		 dif->panel.desc->name, (unsigned int)__ffs(dif->cs),
		 dif->mode.hdisplay, dif->mode.vdisplay,
		 dif->tx_chan ? "DMA" : "polled");

	return 0;
}

static void pmb887x_dif_remove(struct platform_device *pdev)
{
	struct drm_device *drm = platform_get_drvdata(pdev);

	drm_dev_unplug(drm);
	drm_atomic_helper_shutdown(drm);
	pmb887x_dif_hw_stop(to_pmb887x_dif(drm));
}

static void pmb887x_dif_shutdown(struct platform_device *pdev)
{
	drm_atomic_helper_shutdown(platform_get_drvdata(pdev));
}

static const struct of_device_id pmb887x_dif_of_match[] = {
	{ .compatible = "infineon,pmb8876-dif" },
	{ }
};
MODULE_DEVICE_TABLE(of, pmb887x_dif_of_match);

static struct platform_driver pmb887x_dif_platform_driver = {
	.driver = {
		.name = "pmb887x-dif",
		.of_match_table = pmb887x_dif_of_match,
	},
	.probe = pmb887x_dif_probe,
	.remove = pmb887x_dif_remove,
	.shutdown = pmb887x_dif_shutdown,
};
module_platform_driver(pmb887x_dif_platform_driver);

MODULE_DESCRIPTION("Infineon PMB8876 DIFv2 DRM driver");
MODULE_AUTHOR("Alula");
MODULE_LICENSE("GPL");
