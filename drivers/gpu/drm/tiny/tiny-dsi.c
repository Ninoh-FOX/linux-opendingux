// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * TinyDRM driver for standard DSI/DBI panels
 *
 * Copyright 2020 Paul Cercueil <paul@crapouillou.net>
 */

#include <linux/dma-mapping.h>
#include <linux/module.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_gem_cma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_mipi_dbi.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modeset_helper.h>
#include <drm/drm_panel.h>
#include <drm/drm_probe_helper.h>

#include <video/mipi_display.h>

struct tiny_dsi {
	struct drm_device drm;
	struct drm_connector connector;
	struct drm_simple_display_pipe pipe;

	struct mipi_dsi_device *dsi;
	struct drm_panel *panel;
};

#define mipi_dcs_command(dsi, cmd, seq...) \
({ \
	u8 d[] = { seq }; \
	mipi_dsi_dcs_write(dsi, cmd, d, ARRAY_SIZE(d)); \
})

static inline struct tiny_dsi *drm_to_tiny_dsi(struct drm_device *drm)
{
	return container_of(drm, struct tiny_dsi, drm);
}

static void tiny_dsi_fb_dirty(struct drm_framebuffer *fb, struct drm_rect *rect)
{
	struct drm_gem_object *gem = drm_gem_fb_get_obj(fb, 0);
	struct drm_gem_cma_object *cma_obj = to_drm_gem_cma_obj(gem);
	struct tiny_dsi *priv = drm_to_tiny_dsi(fb->dev);
	unsigned int height = rect->y2 - rect->y1;
	unsigned int width = rect->x2 - rect->x1;
	bool fb_convert;
	int idx, ret;
	void *tr;

	if (!drm_dev_enter(fb->dev, &idx))
		return;

	DRM_DEBUG_KMS("Flushing [FB:%d] " DRM_RECT_FMT "\n", fb->base.id, DRM_RECT_ARG(rect));

	fb_convert = width != fb->width || height != fb->height
		|| fb->format->format == DRM_FORMAT_XRGB8888;
	if (fb_convert) {
		tr = kzalloc(width * height * 2, GFP_KERNEL);

		/* TODO: swap pixels if needed */
		ret = mipi_dbi_buf_copy(tr, fb, rect, false);
		if (ret)
			goto err_msg;
	} else {
		tr = cma_obj->vaddr;
	}

	mipi_dcs_command(priv->dsi, MIPI_DCS_SET_COLUMN_ADDRESS,
			 (rect->x1 >> 8) & 0xff, rect->x1 & 0xff,
			 (rect->x2 >> 8) & 0xff, rect->x2 & 0xff);
	mipi_dcs_command(priv->dsi, MIPI_DCS_SET_PAGE_ADDRESS,
			 (rect->y1 >> 8) & 0xff, rect->y1 & 0xff,
			 (rect->y2 >> 8) & 0xff, rect->y2 & 0xff);

	ret = mipi_dsi_dcs_write(priv->dsi, MIPI_DCS_WRITE_MEMORY_START,
				 tr, width * height * 2);
err_msg:
	if (ret)
		dev_err_once(fb->dev->dev, "Failed to update display %d\n", ret);

	if (fb_convert)
		kfree(tr);
	drm_dev_exit(idx);
}

static void tiny_dsi_enable(struct drm_simple_display_pipe *pipe,
			    struct drm_crtc_state *crtc_state,
			    struct drm_plane_state *plane_state)
{
	struct tiny_dsi *priv = drm_to_tiny_dsi(pipe->crtc.dev);

	drm_panel_enable(priv->panel);
}

static void tiny_dsi_disable(struct drm_simple_display_pipe *pipe)
{
	struct tiny_dsi *priv = drm_to_tiny_dsi(pipe->crtc.dev);

	drm_panel_disable(priv->panel);
}

static void tiny_dsi_update(struct drm_simple_display_pipe *pipe,
			    struct drm_plane_state *old_state)
{
	struct drm_plane_state *state = pipe->plane.state;
	struct drm_rect rect;

	if (drm_atomic_helper_damage_merged(old_state, state, &rect))
		tiny_dsi_fb_dirty(state->fb, &rect);
}

static const struct drm_simple_display_pipe_funcs tiny_dsi_pipe_funcs = {
	.enable = tiny_dsi_enable,
	.disable = tiny_dsi_disable,
	.update = tiny_dsi_update,
	.prepare_fb = drm_gem_fb_simple_display_pipe_prepare_fb,
};

static int tiny_dsi_connector_get_modes(struct drm_connector *connector)
{
	struct tiny_dsi *priv = drm_to_tiny_dsi(connector->dev);

	return drm_panel_get_modes(priv->panel, connector);
}

static const struct drm_connector_helper_funcs tiny_dsi_connector_hfuncs = {
	.get_modes = tiny_dsi_connector_get_modes,
};

static const struct drm_connector_funcs tiny_dsi_connector_funcs = {
	.reset = drm_atomic_helper_connector_reset,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static const uint32_t tiny_dsi_formats[] = {
	DRM_FORMAT_RGB565,
	DRM_FORMAT_XRGB8888,
};

static const struct drm_mode_config_funcs tiny_dsi_mode_config_funcs = {
	.fb_create = drm_gem_fb_create_with_dirty,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

DEFINE_DRM_GEM_CMA_FOPS(tiny_dsi_fops);

static struct drm_driver tiny_dsi_driver = {
	.driver_features	= DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	.name			= "tiny-dsi",
	.desc			= "Tiny DSI",
	.date			= "20200605",
	.major			= 1,
	.minor			= 0,

	.fops			= &tiny_dsi_fops,
	DRM_GEM_CMA_DRIVER_OPS,
};

static void tiny_dsi_remove(void *drm)
{
	drm_dev_unplug(drm);
	drm_atomic_helper_shutdown(drm);
}

int mipi_dsi_register_tiny_driver(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct drm_device *drm;
	struct tiny_dsi *priv;
	static const uint64_t modifiers[] = {
		DRM_FORMAT_MOD_LINEAR,
		DRM_FORMAT_MOD_INVALID
	};
	int ret;

	/*
	 * Even though it's not the SPI device that does DMA (the master does),
	 * the dma mask is necessary for the dma_alloc_wc() in
	 * drm_gem_cma_create(). The dma_addr returned will be a physical
	 * address which might be different from the bus address, but this is
	 * not a problem since the address will not be used.
	 * The virtual address is used in the transfer and the SPI core
	 * re-maps it on the SPI master device using the DMA streaming API
	 * (spi_map_buf()).
	 */
	if (!dev->coherent_dma_mask) {
		ret = dma_coerce_mask_and_coherent(dev, DMA_BIT_MASK(32));
		if (ret) {
			dev_warn(dev, "Failed to set dma mask %d\n", ret);
			return ret;
		}
	}

	priv = devm_drm_dev_alloc(dev, &tiny_dsi_driver, struct tiny_dsi, drm);
	if (!priv)
		return ret;

	priv->dsi = dsi;
	drm = &priv->drm;

	priv->panel = of_drm_find_panel(dev->of_node);
	if (IS_ERR(priv->panel)) {
		dev_err(dev, "Unable to find panel\n");
		return PTR_ERR(priv->panel);
	}

	drm_mode_config_init(drm);

	drm->mode_config.preferred_depth = 16;

	drm->mode_config.funcs = &tiny_dsi_mode_config_funcs;
	drm->mode_config.min_width = 0;
	drm->mode_config.min_height = 0;
	drm->mode_config.max_width = 4096;
	drm->mode_config.max_height = 4096;

	drm_connector_helper_add(&priv->connector, &tiny_dsi_connector_hfuncs);
	ret = drm_connector_init(drm, &priv->connector, &tiny_dsi_connector_funcs,
				 DRM_MODE_CONNECTOR_DSI);
	if (ret) {
		dev_err(dev, "Unable to init connector\n");
		return ret;
	}

	ret = drm_simple_display_pipe_init(drm, &priv->pipe, &tiny_dsi_pipe_funcs,
					   tiny_dsi_formats, ARRAY_SIZE(tiny_dsi_formats),
					   modifiers, &priv->connector);
	if (ret) {
		dev_err(dev, "Unable to init display pipe\n");
		return ret;
	}

	drm_plane_enable_fb_damage_clips(&priv->pipe.plane);

	drm_mode_config_reset(drm);

	ret = drm_dev_register(drm, 0);
	if (ret) {
		dev_err(dev, "Failed to register DRM driver\n");
		return ret;
	}

	ret = devm_add_action_or_reset(dev, tiny_dsi_remove, drm);
	if (ret)
		return ret;

	drm_fbdev_generic_setup(drm, 0);

	return 0;
}
EXPORT_SYMBOL_GPL(mipi_dsi_register_tiny_driver);

MODULE_DESCRIPTION("DSI/DBI TinyDRM driver");
MODULE_AUTHOR("Paul Cercueil <paul@crapouillou.net>");
MODULE_LICENSE("GPL");
