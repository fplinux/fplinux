// SPDX-License-Identifier: GPL-2.0-only
/* UMS9117 atomic DRM driver for fixed command-mode panels. */
#include <linux/bitops.h>
#include <linux/dma-fence.h>
#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/property.h>

#include <drm/clients/drm_client_setup.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_color_mgmt.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_fbdev_dma.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_modeset_helper.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_vblank.h>

#include "ums9117-drm-internal.h"

static struct ums9117_drm *to_ums9117_drm(struct drm_device *drm)
{
	return container_of(drm, struct ums9117_drm, drm);
}

static void ums9117_drm_commit_error(struct ums9117_drm *udrm, int error)
{
	struct drm_pending_vblank_event *event;
	unsigned long flags;

	ums9117_drm_display_enter_error_locked(udrm, error);
	/* OUT_FENCE reports failure; an event alone only releases ownership. */
	spin_lock_irqsave(&udrm->drm.event_lock, flags);
	event = udrm->pipe.crtc.state->event;
	if (event && event->base.fence)
		dma_fence_set_error(event->base.fence, error);
	spin_unlock_irqrestore(&udrm->drm.event_lock, flags);
	dev_err_ratelimited(udrm->drm.dev, "display transfer failed: %pe\n",
			    ERR_PTR(error));
}

static enum drm_mode_status
ums9117_drm_mode_valid(struct drm_simple_display_pipe *pipe,
		       const struct drm_display_mode *mode)
{
	struct ums9117_drm *udrm = to_ums9117_drm(pipe->crtc.dev);

	return drm_crtc_helper_mode_valid_fixed(&pipe->crtc, mode, &udrm->mode);
}

static int ums9117_drm_pipe_check(struct drm_simple_display_pipe *pipe,
				  struct drm_plane_state *state,
				  struct drm_crtc_state *crtc_state)
{
	struct ums9117_drm *udrm = to_ums9117_drm(pipe->crtc.dev);
	struct drm_framebuffer *fb = state->fb;
	unsigned int i;
	u32 pitch;

	if (READ_ONCE(udrm->stopping))
		return -ENODEV;
	if (READ_ONCE(udrm->state) == UMS9117_DRM_PANEL_STATE_ERROR)
		return -EIO;
	if (fb->width != udrm->profile->width ||
	    fb->height != udrm->profile->height || state->src_x || state->src_y)
		return -EINVAL;
	pitch = fb->width * (fb->format->format == DRM_FORMAT_RGB565 ? 2 : 1);
	for (i = 0; i < fb->format->num_planes; i++) {
		dma_addr_t address = drm_fb_dma_get_gem_addr(fb, state, i);

		if (fb->pitches[i] != pitch ||
		    !IS_ALIGNED(address, sizeof(u32)))
			return -EINVAL;
	}
	return 0;
}

static void ums9117_drm_pipe_enable(struct drm_simple_display_pipe *pipe,
				    struct drm_crtc_state *crtc_state,
				    struct drm_plane_state *plane_state)
{
	struct ums9117_drm *udrm = to_ums9117_drm(pipe->crtc.dev);
	int ret, idx;

	if (!drm_dev_enter(&udrm->drm, &idx))
		return;
	mutex_lock(&udrm->panel_lock);
	ret = ums9117_drm_display_enable_locked(udrm, plane_state);
	if (ret)
		ums9117_drm_commit_error(udrm, ret);
	mutex_unlock(&udrm->panel_lock);
	drm_dev_exit(idx);
}

static void ums9117_drm_pipe_update(struct drm_simple_display_pipe *pipe,
				    struct drm_plane_state *old_state)
{
	struct ums9117_drm *udrm = to_ums9117_drm(pipe->crtc.dev);
	struct drm_plane_state *state = pipe->plane.state;
	struct drm_rect damage;
	int ret, idx;

	if (!pipe->crtc.state->active || !drm_dev_enter(&udrm->drm, &idx))
		return;
	mutex_lock(&udrm->panel_lock);
	/* A modeset submits its complete first frame from enable(). */
	if (udrm->state == UMS9117_DRM_PANEL_STATE_ACTIVE &&
	    drm_atomic_helper_damage_merged(old_state, state, &damage)) {
		if (state->fb->format->format == DRM_FORMAT_RGB565)
			ums9117_drm_rgb565_align_damage(&damage);
		else
			damage = DRM_RECT_INIT(0, 0, udrm->profile->width,
					       udrm->profile->height);
		ret = ums9117_drm_display_send_frame_locked(udrm, state,
							    &damage);
		if (ret)
			ums9117_drm_commit_error(udrm, ret);
	} else if (udrm->state == UMS9117_DRM_PANEL_STATE_ERROR) {
		ums9117_drm_commit_error(udrm, -EIO);
	}
	mutex_unlock(&udrm->panel_lock);
	drm_dev_exit(idx);
}

static void ums9117_drm_pipe_disable(struct drm_simple_display_pipe *pipe)
{
	struct ums9117_drm *udrm = to_ums9117_drm(pipe->crtc.dev);
	int ret;

	mutex_lock(&udrm->panel_lock);
	ret = ums9117_drm_display_blank_locked(udrm);
	if (ret)
		ums9117_drm_commit_error(udrm, ret);
	mutex_unlock(&udrm->panel_lock);
}

static const struct drm_simple_display_pipe_funcs ums9117_drm_pipe_funcs = {
	.mode_valid = ums9117_drm_mode_valid,
	.check = ums9117_drm_pipe_check,
	.enable = ums9117_drm_pipe_enable,
	.disable = ums9117_drm_pipe_disable,
	.update = ums9117_drm_pipe_update,
};

static int ums9117_drm_get_modes(struct drm_connector *connector)
{
	struct ums9117_drm *udrm = to_ums9117_drm(connector->dev);

	return drm_connector_helper_get_modes_fixed(connector, &udrm->mode);
}

static const struct drm_connector_helper_funcs ums9117_drm_connector_helpers = {
	.get_modes = ums9117_drm_get_modes,
};

static const struct drm_connector_funcs ums9117_drm_connector_funcs = {
	.reset = drm_atomic_helper_connector_reset,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static const struct drm_mode_config_funcs ums9117_drm_mode_config_funcs = {
	.fb_create = drm_gem_fb_create_with_dirty,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

DEFINE_DRM_GEM_DMA_FOPS(ums9117_drm_fops);

#if IS_ENABLED(CONFIG_DRM_FBDEV_EMULATION)
static int ums9117_drm_fbdev_probe(struct drm_fb_helper *helper,
				   struct drm_fb_helper_surface_size *sizes)
{
	int ret = drm_fbdev_dma_driver_fbdev_probe(helper, sizes);

	/* VT_PROCESS clients must release input ownership before suspend. */
	if (!ret)
		helper->info->skip_vt_switch = false;
	return ret;
}
#endif

static const struct drm_driver ums9117_drm_driver = {
	.driver_features = DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	.fops = &ums9117_drm_fops,
	DRM_GEM_DMA_DRIVER_OPS,
#if IS_ENABLED(CONFIG_DRM_FBDEV_EMULATION)
	.fbdev_probe = ums9117_drm_fbdev_probe,
#endif
	.name = "ums9117",
	.desc = "UMS9117 command-mode display",
	.major = 1,
	.minor = 0,
};

static ssize_t audit_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct ums9117_drm *udrm = dev_get_drvdata(dev);
	struct ums9117_drm_stats stats;
	unsigned long flags;
	ssize_t len;

	spin_lock_irqsave(&udrm->lock, flags);
	stats = udrm->stats;
	len = sysfs_emit(
		buf,
		"init_mode=cold-reset\n"
		"completion_mode=irq\n"
		"timeout_mode=finite-to-error\n"
		"damage_mode=rgb565-merged-rectangle\n"
		"lifecycle_mode=wled+dcs-display+sleep\n"
		"profile=%s\n"
		"transport=%s\n"
		"panel_state=%u\n"
		"in_flight=%u\n"
		"wled_state=%s\n"
		"transport_faulted=%u\n"
		"frames_started=%llu\n"
		"frames_done_irq=%llu\n"
		"frame_timeouts=%llu\n"
		"irq_spurious=%llu\n"
		"irq_missed=%llu\n"
		"blank_count=%llu\n"
		"blank_completed=%llu\n"
		"wake_count=%llu\n"
		"dcs_errors=%llu\n"
		"dcs_timeouts=%llu\n"
		"wled_errors=%llu\n"
		"fail_dark_failures=%llu\n"
		"present_nv16=%llu\n"
		"present_rgb565=%llu\n"
		"last_transfer_ns=%llu\n"
		"last_y_address=%pad\n"
		"last_uv_address=%pad\n"
		"last_transfer_x=%d\n"
		"last_transfer_y=%d\n"
		"last_transfer_width=%d\n"
		"last_transfer_height=%d\n"
		"last_source_pitch_pixels=%u\n"
		"last_transfer_pixels=%d\n"
		"last_error_errno=%d\n"
		"last_error_dcs_command=0x%02x\n"
		"last_error_irq_status=0x%08x\n"
		"last_error_irq_raw=0x%08x\n",
		udrm->profile->name,
		ums9117_drm_uses_spi(udrm) ? "spi1-3wire" : "lcm-dbi",
		udrm->state, udrm->in_flight ? 1U : 0U,
		!udrm->wled_known ? "unknown" :
		udrm->wled_on	  ? "on" :
				    "off",
		udrm->transport_faulted ? 1U : 0U, stats.frames_started,
		stats.frames_done_irq, stats.frame_timeouts, stats.irq_spurious,
		stats.irq_missed, stats.blank_count, stats.blank_completed,
		stats.wake_count, stats.dcs_errors, stats.dcs_timeouts,
		stats.wled_errors, stats.fail_dark_failures, stats.present_nv16,
		stats.present_rgb565, udrm->last_transfer_ns,
		&udrm->last_y_address, &udrm->last_uv_address,
		udrm->last_transfer_rect.x1, udrm->last_transfer_rect.y1,
		drm_rect_width(&udrm->last_transfer_rect),
		drm_rect_height(&udrm->last_transfer_rect),
		udrm->last_source_pitch_pixels,
		drm_rect_width(&udrm->last_transfer_rect) *
			drm_rect_height(&udrm->last_transfer_rect),
		udrm->last_error_errno, udrm->last_dcs_command,
		stats.last_error_irq_status, stats.last_error_irq_raw);
	spin_unlock_irqrestore(&udrm->lock, flags);
	return len;
}
static DEVICE_ATTR_RO(audit);

static struct attribute *ums9117_drm_attrs[] = {
	&dev_attr_audit.attr,
	NULL,
};
static const struct attribute_group ums9117_drm_group = {
	.attrs = ums9117_drm_attrs,
};

static void ums9117_drm_release_dma_pool(struct drm_device *drm, void *data)
{
	of_reserved_mem_device_release(drm->dev);
}

int ums9117_drm_probe(struct platform_device *pdev)
{
	static const u32 formats[] = { DRM_FORMAT_RGB565, DRM_FORMAT_NV16 };
	static const u64 modifiers[] = {
		DRM_FORMAT_MOD_LINEAR,
		DRM_FORMAT_MOD_INVALID,
	};
	struct device *dev = &pdev->dev;
	const struct ums9117_drm_profile *profile = device_get_match_data(dev);
	struct ums9117_drm *udrm;
	struct drm_device *drm;
	int ret;

	if (!profile || !profile->name || !profile->width || !profile->height ||
	    !profile->init || !profile->init_count)
		return -EINVAL;
	if (!(IS_ENABLED(CONFIG_DRM_UMS9117_SPI) &&
	      profile->transport == UMS9117_DRM_TRANSPORT_SPI1_3WIRE) &&
	    !(IS_ENABLED(CONFIG_DRM_UMS9117_LCM) &&
	      profile->transport == UMS9117_DRM_TRANSPORT_LCM_DBI))
		return dev_err_probe(dev, -EINVAL,
				     "panel transport not built\n");
	if ((profile->wake_finish &&
	     !IS_ENABLED(CONFIG_DRM_UMS9117_WAKE_REINIT)) ||
	    (!profile->wake_finish &&
	     !IS_ENABLED(CONFIG_DRM_UMS9117_WAKE_SLEEP_OUT)))
		return dev_err_probe(dev, -EINVAL,
				     "panel wake strategy not built\n");
	udrm = devm_drm_dev_alloc(dev, &ums9117_drm_driver, struct ums9117_drm,
				  drm);
	if (IS_ERR(udrm))
		return PTR_ERR(udrm);
	drm = &udrm->drm;
	udrm->profile = profile;
	udrm->state = UMS9117_DRM_PANEL_STATE_COLD_INIT;
	spin_lock_init(&udrm->lock);
	mutex_init(&udrm->panel_lock);
	init_completion(&udrm->frame_done);
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;
	ret = of_reserved_mem_device_init(dev);
	if (ret)
		return dev_err_probe(dev, ret,
				     "could not attach display DMA pool\n");
	/* GEM handles may outlive unbind; keep their allocator until release. */
	ret = drmm_add_action_or_reset(drm, ums9117_drm_release_dma_pool, NULL);
	if (ret)
		return ret;
	ret = ums9117_drm_display_init(udrm, pdev);
	if (ret)
		return ret;
	ret = drmm_mode_config_init(drm);
	if (ret)
		return ret;
	udrm->mode = (struct drm_display_mode){
		DRM_SIMPLE_MODE(profile->width, profile->height, 0, 0),
	};
	drm->mode_config.funcs = &ums9117_drm_mode_config_funcs;
	drm->mode_config.min_width = profile->width;
	drm->mode_config.max_width = profile->width;
	drm->mode_config.min_height = profile->height;
	drm->mode_config.max_height = profile->height;
	drm_connector_helper_add(&udrm->connector,
				 &ums9117_drm_connector_helpers);
	ret = drm_connector_init(drm, &udrm->connector,
				 &ums9117_drm_connector_funcs,
				 ums9117_drm_uses_spi(udrm) ?
					 DRM_MODE_CONNECTOR_SPI :
					 DRM_MODE_CONNECTOR_Unknown);
	if (ret)
		return ret;
	ret = drm_simple_display_pipe_init(
		drm, &udrm->pipe, &ums9117_drm_pipe_funcs, formats,
		profile->native_nv16 ? ARRAY_SIZE(formats) : 1, modifiers,
		&udrm->connector);
	if (ret)
		return ret;
	ret = drm_plane_create_color_properties(&udrm->pipe.plane,
						BIT(DRM_COLOR_YCBCR_BT601),
						BIT(DRM_COLOR_YCBCR_FULL_RANGE),
						DRM_COLOR_YCBCR_BT601,
						DRM_COLOR_YCBCR_FULL_RANGE);
	if (ret)
		return ret;
	drm_plane_enable_fb_damage_clips(&udrm->pipe.plane);
	drm_mode_config_reset(drm);
	platform_set_drvdata(pdev, udrm);
	ret = ums9117_drm_display_register_backlight(udrm, pdev);
	if (ret)
		return ret;
	ret = devm_device_add_group(dev, &ums9117_drm_group);
	if (ret)
		return ret;
	ret = drm_dev_register(drm, 0);
	if (ret)
		return ret;
	/* DONE completes transfers; there is no panel vblank counter. */
	drm_client_setup_with_fourcc(drm, DRM_FORMAT_RGB565);
	return 0;
}
EXPORT_SYMBOL_GPL(ums9117_drm_probe);

void ums9117_drm_shutdown(struct platform_device *pdev)
{
	struct ums9117_drm *udrm = platform_get_drvdata(pdev);

	drm_atomic_helper_shutdown(&udrm->drm);
	ums9117_drm_display_shutdown(udrm);
}
EXPORT_SYMBOL_GPL(ums9117_drm_shutdown);

void ums9117_drm_remove(struct platform_device *pdev)
{
	struct ums9117_drm *udrm = platform_get_drvdata(pdev);

	drm_dev_unplug(&udrm->drm);
	ums9117_drm_shutdown(pdev);
}
EXPORT_SYMBOL_GPL(ums9117_drm_remove);

static int __maybe_unused ums9117_drm_suspend(struct device *dev)
{
	struct ums9117_drm *udrm = dev_get_drvdata(dev);
	int ret;

	ret = drm_mode_config_helper_suspend(&udrm->drm);
	if (!ret && READ_ONCE(udrm->state) == UMS9117_DRM_PANEL_STATE_ERROR)
		ret = -EIO;
	return ret;
}

static int __maybe_unused ums9117_drm_resume(struct device *dev)
{
	struct ums9117_drm *udrm = dev_get_drvdata(dev);
	int ret;

	ret = drm_mode_config_helper_resume(&udrm->drm);
	if (!ret && READ_ONCE(udrm->state) == UMS9117_DRM_PANEL_STATE_ERROR)
		ret = -EIO;
	return ret;
}

DEFINE_SIMPLE_DEV_PM_OPS(ums9117_drm_pm_ops, ums9117_drm_suspend,
			 ums9117_drm_resume);

MODULE_DESCRIPTION("UMS9117 atomic DRM display core");
MODULE_LICENSE("GPL");
