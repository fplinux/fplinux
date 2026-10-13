// SPDX-License-Identifier: GPL-2.0-only
/* UMS9117 command-mode panel, backlight and completed frame operations. */
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/ktime.h>
#include <linux/mfd/syscon.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <video/mipi_display.h>

#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>

#include "ums9117-drm-internal.h"

#define UMS9117_LCDC_CTRL 0x000
#define UMS9117_LCDC_DISP_SIZE 0x004
#define UMS9117_LCDC_LCM_START 0x008
#define UMS9117_LCDC_LCM_SIZE 0x00c
#define UMS9117_LCDC_BG_COLOR 0x010
#define UMS9117_LCDC_IMG_CTRL 0x020
#define UMS9117_LCDC_IMG_Y_BASE 0x024
#define UMS9117_LCDC_IMG_UV_BASE 0x028
#define UMS9117_LCDC_IMG_SIZE_XY 0x02c
#define UMS9117_LCDC_IMG_PITCH 0x030
#define UMS9117_LCDC_IMG_DISP_XY 0x034
#define UMS9117_LCDC_CAP_CTRL 0x0e0
#define UMS9117_LCDC_CAP_BASE 0x0e4
#define UMS9117_LCDC_Y2R_CTRL 0x100
#define UMS9117_LCDC_Y2R_CONTRAST 0x104
#define UMS9117_LCDC_Y2R_SATURATION 0x108
#define UMS9117_LCDC_Y2R_BRIGHTNESS 0x10c
#define UMS9117_LCDC_IRQ_EN 0x110
#define UMS9117_LCDC_IRQ_CLR 0x114
#define UMS9117_LCDC_IRQ_STATUS 0x118
#define UMS9117_LCDC_IRQ_RAW 0x11c
#define UMS9117_LCDC_IRQ_DONE BIT(0)
#define UMS9117_LCDC_CTRL_RUN BIT(3)
#define UMS9117_LCDC_CTRL_RGB_MODE_MASK (7U << 5)

#define SC2720_BLTC_CTRL 0x180
#define SC2720_BLTC_CURRENT0 0x1b8
#define SC2720_BLTC_CURRENT1 0x1bc
#define SC2720_BLTC_CURRENT2 0x1c0
#define SC2720_BLTC_CURRENT3 0x1c4
#define SC2720_BLTC_WLED_PRESCALER 0x1c8
#define SC2720_BLTC_WLED_DUTY 0x1cc
#define SC2720_BLTC_PD_CTRL 0x1d8
#define SC2720_BLTC_CURRENT_LEVEL_MASK GENMASK(5, 0)
#define SC2720_BLTC_PD_CTRL_SOFTWARE_POWER_DOWN BIT(0)
#define SC2720_MODULE_EN0 0xc08
#define SC2720_RTC_CLK_EN0 0xc10
#define SC2720_LDO_PD_CTRL 0xdec
#define SC2720_MODULE_EN0_WLED_MODULE_ENABLE BIT(9)
#define SC2720_RTC_CLK_EN0_WLED_CLOCK_ENABLE BIT(7)
#define SC2720_LDO_PD_CTRL_WLED_POWER_DOWN_MASK (BIT(2) | BIT(0))

#define UMS9117_AON_PANEL_RESET_SET 0x160c
#define UMS9117_AON_PANEL_RESET_CLEAR 0x260c
#define UMS9117_AP_AHB_LCDC_GATE BIT(11)
#define UMS9117_AP_AHB_LCM_GATE BIT(12)
#define UMS9117_AP_AHB_LCDC_RESET BIT(1)

#define UMS9117_DRM_FRAME_TIMEOUT_MS 250U
#define UMS9117_DRM_WLED_DISABLE_ATTEMPTS 3U
#define UMS9117_DRM_WLED_DISABLE_RETRY_US 5000U

static void ums9117_drm_stop_lcdc(struct ums9117_drm *udrm)
{
	writel(readl(udrm->lcdc + UMS9117_LCDC_IRQ_EN) & ~UMS9117_LCDC_IRQ_DONE,
	       udrm->lcdc + UMS9117_LCDC_IRQ_EN);
	writel(readl(udrm->lcdc + UMS9117_LCDC_CTRL) &
		       ~(BIT(0) | UMS9117_LCDC_CTRL_RUN),
	       udrm->lcdc + UMS9117_LCDC_CTRL);
	writel(UMS9117_LCDC_IRQ_DONE, udrm->lcdc + UMS9117_LCDC_IRQ_CLR);
	readl(udrm->lcdc + UMS9117_LCDC_IRQ_RAW);
}

static void ums9117_drm_set_wled_state(struct ums9117_drm *udrm, bool known,
				       bool on, unsigned int brightness)
{
	unsigned long flags;

	spin_lock_irqsave(&udrm->lock, flags);
	udrm->wled_known = known;
	udrm->wled_on = known && on;
	if (udrm->backlight_max_brightness)
		udrm->backlight_applied = known ? brightness : 0;
	spin_unlock_irqrestore(&udrm->lock, flags);
}

static int ums9117_drm_wled_set(struct ums9117_drm *udrm, bool on,
				unsigned int brightness)
{
	static const u32 current_regs[UMS9117_DRM_WLED_CHANNEL_COUNT] = {
		SC2720_BLTC_CURRENT0,
		SC2720_BLTC_CURRENT1,
		SC2720_BLTC_CURRENT2,
		SC2720_BLTC_CURRENT3,
	};
	unsigned int value;
	u16 active_mask = 0;
	unsigned int i;
	int ret;

	if (on && udrm->backlight_max_brightness &&
	    (!brightness || brightness > udrm->backlight_max_brightness))
		return -EINVAL;
	ret = regmap_update_bits(udrm->pmic, SC2720_MODULE_EN0,
				 SC2720_MODULE_EN0_WLED_MODULE_ENABLE,
				 SC2720_MODULE_EN0_WLED_MODULE_ENABLE);
	if (!ret)
		ret = regmap_update_bits(udrm->pmic, SC2720_RTC_CLK_EN0,
					 SC2720_RTC_CLK_EN0_WLED_CLOCK_ENABLE,
					 SC2720_RTC_CLK_EN0_WLED_CLOCK_ENABLE);
	if (!ret)
		ret = regmap_update_bits(
			udrm->pmic, SC2720_LDO_PD_CTRL,
			SC2720_LDO_PD_CTRL_WLED_POWER_DOWN_MASK, 0);
	if (!ret && !on)
		ret = regmap_write(udrm->pmic, SC2720_BLTC_CTRL, 0);
	if (!ret && !on) {
		ret = regmap_read(udrm->pmic, SC2720_BLTC_PD_CTRL, &value);
		if (!ret)
			ret = regmap_write(
				udrm->pmic, SC2720_BLTC_PD_CTRL,
				value | SC2720_BLTC_PD_CTRL_SOFTWARE_POWER_DOWN);
	}
	if (!ret && on) {
		for (i = 0; !ret && i < ARRAY_SIZE(current_regs); i++) {
			u32 current_level = udrm->wled_levels[i];

			if (udrm->wled_levels[i])
				active_mask |= 0xc << (i * 4);
			if (udrm->backlight_max_brightness &&
			    udrm->wled_levels[i])
				current_level = brightness;
			ret = regmap_read(udrm->pmic, current_regs[i], &value);
			if (!ret)
				ret = regmap_write(
					udrm->pmic, current_regs[i],
					(value &
					 ~SC2720_BLTC_CURRENT_LEVEL_MASK) |
						current_level);
		}
	}
	if (!ret && on)
		ret = regmap_update_bits(udrm->pmic, SC2720_BLTC_WLED_PRESCALER,
					 0xff, 0);
	if (!ret && on)
		ret = regmap_write(udrm->pmic, SC2720_BLTC_WLED_DUTY, 0);
	if (!ret && on) {
		ret = regmap_read(udrm->pmic, SC2720_BLTC_PD_CTRL, &value);
		if (!ret)
			ret = regmap_write(
				udrm->pmic, SC2720_BLTC_PD_CTRL,
				value & ~SC2720_BLTC_PD_CTRL_SOFTWARE_POWER_DOWN);
	}
	if (!ret && on)
		ret = regmap_write(udrm->pmic, SC2720_BLTC_CTRL, active_mask);
	if (ret)
		udrm->stats.wled_errors++;
	ums9117_drm_set_wled_state(udrm, !ret, on, on ? brightness : 0);
	return ret;
}

static int ums9117_drm_wled_off_bounded(struct ums9117_drm *udrm)
{
	unsigned int attempt;
	int ret = -EBUSY;

	for (attempt = 0; attempt < UMS9117_DRM_WLED_DISABLE_ATTEMPTS;
	     attempt++) {
		ret = ums9117_drm_wled_set(udrm, false, 0);
		if (ret != -EBUSY)
			break;
		usleep_range(UMS9117_DRM_WLED_DISABLE_RETRY_US,
			     UMS9117_DRM_WLED_DISABLE_RETRY_US + 1000);
	}
	return ret;
}

static int ums9117_drm_dcs_common(struct ums9117_drm *udrm, u8 command,
				  const u8 *data, size_t length, bool force)
{
	int ret;

	if (!force && READ_ONCE(udrm->transport_faulted))
		return -EIO;
	if (ums9117_drm_uses_spi(udrm))
		ret = ums9117_drm_spi_dcs(udrm, command, data, length);
	else if (ums9117_drm_uses_lcm(udrm))
		ret = ums9117_drm_lcm_dcs(udrm, command, data, length);
	else
		ret = -ENODEV;
	if (ret) {
		unsigned long flags;

		spin_lock_irqsave(&udrm->lock, flags);
		udrm->transport_faulted = true;
		udrm->last_dcs_command = command;
		udrm->stats.dcs_errors++;
		if (ret == -ETIMEDOUT)
			udrm->stats.dcs_timeouts++;
		spin_unlock_irqrestore(&udrm->lock, flags);
	}
	return ret;
}

static int ums9117_drm_dcs(struct ums9117_drm *udrm, u8 command, const u8 *data,
			   size_t length)
{
	return ums9117_drm_dcs_common(udrm, command, data, length, false);
}

/* Best-effort display-off must still reach the transport after a prior fault. */
static int ums9117_drm_dcs_force(struct ums9117_drm *udrm, u8 command)
{
	return ums9117_drm_dcs_common(udrm, command, NULL, 0, true);
}

static int ums9117_drm_fail_dark(struct ums9117_drm *udrm)
{
	unsigned long flags;
	int wled_ret;
	int dcs_ret;

	/* Do both bounded operations: either one may be the only one that works. */
	wled_ret = ums9117_drm_wled_off_bounded(udrm);
	dcs_ret = ums9117_drm_dcs_force(udrm, MIPI_DCS_SET_DISPLAY_OFF);
	if (!dcs_ret && udrm->profile->display_off_ms)
		msleep(udrm->profile->display_off_ms);
	if (wled_ret && dcs_ret) {
		spin_lock_irqsave(&udrm->lock, flags);
		udrm->stats.fail_dark_failures++;
		spin_unlock_irqrestore(&udrm->lock, flags);
	}
	return wled_ret ? wled_ret : dcs_ret;
}

static int ums9117_drm_run_commands(struct ums9117_drm *udrm,
				    const struct ums9117_drm_command *commands,
				    unsigned int count)
{
	unsigned int i;
	int ret;

	for (i = 0; i < count; i++) {
		const struct ums9117_drm_command *command = &commands[i];

		if (command->length > ARRAY_SIZE(command->data))
			return -EINVAL;
		if (command->command || command->length) {
			ret = ums9117_drm_dcs(udrm, command->command,
					      command->data, command->length);
			if (ret)
				return ret;
		}
		if (command->delay_ms)
			msleep(command->delay_ms);
	}
	return 0;
}

static int ums9117_drm_begin_transport_frame(struct ums9117_drm *udrm,
					     const struct drm_rect *damage)
{
	u16 x_last = damage->x2 - 1;
	u16 y_last = damage->y2 - 1;
	u8 columns[] = { damage->x1 >> 8, damage->x1, x_last >> 8, x_last };
	u8 pages[] = { damage->y1 >> 8, damage->y1, y_last >> 8, y_last };
	int ret;

	/* RAMWR restarts the selected window, including after a partial frame. */
	ret = ums9117_drm_dcs(udrm, MIPI_DCS_SET_COLUMN_ADDRESS, columns,
			      sizeof(columns));
	if (!ret)
		ret = ums9117_drm_dcs(udrm, MIPI_DCS_SET_PAGE_ADDRESS, pages,
				      sizeof(pages));
	if (ret)
		return ret;
	if (ums9117_drm_uses_spi(udrm))
		return ums9117_drm_spi_begin_frame(
			udrm, drm_rect_width(damage) * drm_rect_height(damage));
	if (ums9117_drm_uses_lcm(udrm))
		return ums9117_drm_lcm_begin_frame(udrm);
	return -ENODEV;
}

void ums9117_drm_display_enter_error_locked(struct ums9117_drm *udrm, int error)
{
	unsigned long flags;

	spin_lock_irqsave(&udrm->lock, flags);
	udrm->last_error_errno = error;
	udrm->state = UMS9117_DRM_PANEL_STATE_ERROR;
	udrm->in_flight = false;
	spin_unlock_irqrestore(&udrm->lock, flags);
	ums9117_drm_stop_lcdc(udrm);
	ums9117_drm_fail_dark(udrm);
}

static unsigned int ums9117_drm_cached_backlight_level(struct ums9117_drm *udrm)
{
	unsigned long flags;
	unsigned int brightness;

	spin_lock_irqsave(&udrm->lock, flags);
	brightness = udrm->backlight_effective;
	spin_unlock_irqrestore(&udrm->lock, flags);
	return brightness;
}

#if IS_ENABLED(CONFIG_BACKLIGHT_CLASS_DEVICE)
static int
ums9117_drm_backlight_update_status(struct backlight_device *backlight)
{
	struct ums9117_drm *udrm = bl_get_data(backlight);
	unsigned long flags;
	int effective = backlight_get_brightness(backlight);
	bool apply = false;
	int ret = 0;

	/* The backlight core already holds its update lock. */
	mutex_lock(&udrm->panel_lock);
	spin_lock_irqsave(&udrm->lock, flags);
	udrm->backlight_effective = effective;
	if (udrm->stopping)
		ret = -ENODEV;
	else if (udrm->state == UMS9117_DRM_PANEL_STATE_ERROR)
		ret = -EIO;
	else if (udrm->state == UMS9117_DRM_PANEL_STATE_ACTIVE)
		apply = true;
	spin_unlock_irqrestore(&udrm->lock, flags);
	if (apply) {
		ret = ums9117_drm_wled_set(udrm, effective != 0, effective);
		if (ret)
			ums9117_drm_display_enter_error_locked(udrm, ret);
	}
	mutex_unlock(&udrm->panel_lock);
	return ret;
}

static int
ums9117_drm_backlight_get_brightness(struct backlight_device *backlight)
{
	struct ums9117_drm *udrm = bl_get_data(backlight);
	unsigned long flags;
	int brightness;

	spin_lock_irqsave(&udrm->lock, flags);
	if (udrm->stopping)
		brightness = -ENODEV;
	else if (!udrm->wled_known)
		brightness = -EIO;
	else
		brightness = udrm->backlight_applied;
	spin_unlock_irqrestore(&udrm->lock, flags);
	return brightness;
}

static const struct backlight_ops ums9117_drm_backlight_ops = {
	.update_status = ums9117_drm_backlight_update_status,
	.get_brightness = ums9117_drm_backlight_get_brightness,
};
#endif

static int ums9117_drm_reset_panel(struct ums9117_drm *udrm, u16 phase_ms)
{
	int ret;

	ret = regmap_write(udrm->aon_apb, UMS9117_AON_PANEL_RESET_SET, BIT(0));
	if (ret)
		return ret;
	msleep(phase_ms);
	ret = regmap_write(udrm->aon_apb, UMS9117_AON_PANEL_RESET_CLEAR,
			   BIT(0));
	if (ret)
		return ret;
	msleep(phase_ms);
	ret = regmap_write(udrm->aon_apb, UMS9117_AON_PANEL_RESET_SET, BIT(0));
	if (ret)
		return ret;
	msleep(phase_ms);
	return 0;
}

static int ums9117_drm_wake_panel(struct ums9117_drm *udrm)
{
	const struct ums9117_drm_profile *profile = udrm->profile;
	int ret;

	if (IS_ENABLED(CONFIG_DRM_UMS9117_WAKE_REINIT) &&
	    profile->wake_finish) {
		writel(readl(udrm->lcdc + UMS9117_LCDC_CTRL) | BIT(0),
		       udrm->lcdc + UMS9117_LCDC_CTRL);
		ret = ums9117_drm_reset_panel(udrm,
					      profile->wake_reset_phase_ms);
		if (!ret)
			ret = ums9117_drm_run_commands(udrm, profile->init,
						       profile->init_count);
		if (!ret)
			ret = ums9117_drm_run_commands(
				udrm, profile->wake_finish,
				profile->wake_finish_count);
		return ret;
	}
	if (IS_ENABLED(CONFIG_DRM_UMS9117_WAKE_SLEEP_OUT) &&
	    !profile->wake_finish) {
		ret = ums9117_drm_dcs(udrm, MIPI_DCS_EXIT_SLEEP_MODE, NULL, 0);
		if (ret)
			return ret;
		msleep(profile->sleep_out_ms);
		return ums9117_drm_dcs(udrm, MIPI_DCS_SET_DISPLAY_ON, NULL, 0);
	}
	return -EINVAL;
}

static void __iomem *ums9117_drm_ioremap_shared(struct platform_device *pdev,
						const char *name)
{
	struct resource *resource;

	resource = platform_get_resource_byname(pdev, IORESOURCE_MEM, name);
	if (!resource || resource_size(resource) < sizeof(u32))
		return IOMEM_ERR_PTR(-EINVAL);
	return devm_ioremap(&pdev->dev, resource->start,
			    resource_size(resource));
}

static int ums9117_drm_configure_pin_group(struct platform_device *pdev,
					   const char *mux_name,
					   const char *conf_name)
{
	struct device_node *np = pdev->dev.of_node;
	struct resource *mux_resource;
	struct resource *conf_resource;
	void __iomem *pinmux;
	void __iomem *pinconf;
	char mux_values_name[40];
	char mux_offsets_name[40];
	char conf_values_name[40];
	char conf_offsets_name[40];
	u32 *mux_values;
	u32 *mux_offsets;
	u32 *conf_values;
	u32 *conf_offsets;
	int mux_count;
	int conf_count;
	unsigned int i;

	snprintf(mux_values_name, sizeof(mux_values_name), "sprd,%s-values",
		 mux_name);
	snprintf(mux_offsets_name, sizeof(mux_offsets_name), "sprd,%s-offsets",
		 mux_name);
	snprintf(conf_values_name, sizeof(conf_values_name), "sprd,%s-values",
		 conf_name);
	snprintf(conf_offsets_name, sizeof(conf_offsets_name),
		 "sprd,%s-offsets", conf_name);
	mux_count = of_property_count_u32_elems(np, mux_values_name);
	conf_count = of_property_count_u32_elems(np, conf_values_name);
	if (mux_count <= 0 || conf_count <= 0 || mux_count != conf_count ||
	    mux_count != of_property_count_u32_elems(np, mux_offsets_name) ||
	    conf_count != of_property_count_u32_elems(np, conf_offsets_name))
		return -EINVAL;
	mux_values = devm_kmalloc_array(&pdev->dev, mux_count,
					sizeof(*mux_values), GFP_KERNEL);
	mux_offsets = devm_kmalloc_array(&pdev->dev, mux_count,
					 sizeof(*mux_offsets), GFP_KERNEL);
	conf_values = devm_kmalloc_array(&pdev->dev, conf_count,
					 sizeof(*conf_values), GFP_KERNEL);
	conf_offsets = devm_kmalloc_array(&pdev->dev, conf_count,
					  sizeof(*conf_offsets), GFP_KERNEL);
	if (!mux_values || !mux_offsets || !conf_values || !conf_offsets ||
	    of_property_read_u32_array(np, mux_values_name, mux_values,
				       mux_count) ||
	    of_property_read_u32_array(np, mux_offsets_name, mux_offsets,
				       mux_count) ||
	    of_property_read_u32_array(np, conf_values_name, conf_values,
				       conf_count) ||
	    of_property_read_u32_array(np, conf_offsets_name, conf_offsets,
				       conf_count))
		return -EINVAL;
	pinmux = devm_platform_ioremap_resource_byname(pdev, mux_name);
	if (IS_ERR(pinmux))
		return PTR_ERR(pinmux);
	pinconf = devm_platform_ioremap_resource_byname(pdev, conf_name);
	if (IS_ERR(pinconf))
		return PTR_ERR(pinconf);
	mux_resource =
		platform_get_resource_byname(pdev, IORESOURCE_MEM, mux_name);
	conf_resource =
		platform_get_resource_byname(pdev, IORESOURCE_MEM, conf_name);
	if (!mux_resource || !conf_resource ||
	    resource_size(mux_resource) < sizeof(u32) ||
	    resource_size(conf_resource) < sizeof(u32))
		return -EINVAL;
	for (i = 0; i < mux_count; i++) {
		if (!IS_ALIGNED(mux_offsets[i], sizeof(u32)) ||
		    !IS_ALIGNED(conf_offsets[i], sizeof(u32)) ||
		    mux_offsets[i] >
			    resource_size(mux_resource) - sizeof(u32) ||
		    conf_offsets[i] >
			    resource_size(conf_resource) - sizeof(u32))
			return -EINVAL;
		writel(mux_values[i], pinmux + mux_offsets[i]);
		writel(conf_values[i], pinconf + conf_offsets[i]);
	}
	readl(pinconf + conf_offsets[conf_count - 1]);
	return 0;
}

static int ums9117_drm_configure_pins(struct ums9117_drm *udrm,
				      struct platform_device *pdev)
{
	int ret = ums9117_drm_configure_pin_group(pdev, "pinmux", "pinconf");

	if (ret || !ums9117_drm_uses_lcm(udrm))
		return ret;
	return ums9117_drm_configure_pin_group(pdev, "pinmux-data",
					       "pinconf-data");
}

static irqreturn_t ums9117_drm_lcdc_irq(int irq, void *data)
{
	struct ums9117_drm *udrm = data;
	unsigned long flags;
	u32 status = readl(udrm->lcdc + UMS9117_LCDC_IRQ_STATUS);

	if (!(status & UMS9117_LCDC_IRQ_DONE))
		return IRQ_NONE;
	writel(UMS9117_LCDC_IRQ_DONE, udrm->lcdc + UMS9117_LCDC_IRQ_CLR);
	readl(udrm->lcdc + UMS9117_LCDC_IRQ_RAW);
	spin_lock_irqsave(&udrm->lock, flags);
	if (udrm->in_flight) {
		udrm->in_flight = false;
		udrm->stats.frames_done_irq++;
		complete(&udrm->frame_done);
	} else {
		udrm->stats.irq_spurious++;
	}
	spin_unlock_irqrestore(&udrm->lock, flags);
	return IRQ_HANDLED;
}

static int ums9117_drm_wait_frame(struct ums9117_drm *udrm)
{
	unsigned long flags;
	int ret = 0;

	if (!wait_for_completion_timeout(
		    &udrm->frame_done,
		    msecs_to_jiffies(UMS9117_DRM_FRAME_TIMEOUT_MS)))
		ret = -ETIMEDOUT;
	if (ret) {
		spin_lock_irqsave(&udrm->lock, flags);
		udrm->stats.frame_timeouts++;
		udrm->stats.last_error_irq_status =
			readl(udrm->lcdc + UMS9117_LCDC_IRQ_STATUS);
		udrm->stats.last_error_irq_raw =
			readl(udrm->lcdc + UMS9117_LCDC_IRQ_RAW);
		if (udrm->stats.last_error_irq_raw & UMS9117_LCDC_IRQ_DONE)
			udrm->stats.irq_missed++;
		spin_unlock_irqrestore(&udrm->lock, flags);
	}
	/* Release GEM references only after DONE or the bounded stop path. */
	if (ret)
		ums9117_drm_stop_lcdc(udrm);
	else
		writel(readl(udrm->lcdc + UMS9117_LCDC_IRQ_EN) &
			       ~UMS9117_LCDC_IRQ_DONE,
		       udrm->lcdc + UMS9117_LCDC_IRQ_EN);
	synchronize_irq(udrm->irq);
	return ret;
}

int ums9117_drm_display_send_frame_locked(struct ums9117_drm *udrm,
					  struct drm_plane_state *state,
					  const struct drm_rect *damage)
{
	struct drm_framebuffer *fb = state->fb;
	unsigned int width = drm_rect_width(damage);
	unsigned int height = drm_rect_height(damage);
	dma_addr_t y_address = drm_fb_dma_get_gem_addr(fb, state, 0);
	dma_addr_t uv_address = 0;
	unsigned long flags;
	u64 started_ns;
	u32 value;
	bool nv16 = fb->format->format == DRM_FORMAT_NV16;
	u32 pitch_pixels = fb->pitches[0] / fb->format->cpp[0];
	int ret;

	ret = ums9117_drm_begin_transport_frame(udrm, damage);
	if (ret)
		return ret;
	if (nv16)
		uv_address = drm_fb_dma_get_gem_addr(fb, state, 1);
	else
		y_address += damage->y1 * fb->pitches[0] + damage->x1 * 2;
	/* GEM DMA uses write-combined buffers; publish pixels before RUN. */
	wmb();
	writel(readl(udrm->lcdc + UMS9117_LCDC_CTRL) | BIT(0),
	       udrm->lcdc + UMS9117_LCDC_CTRL);
	writel(width | height << 16, udrm->lcdc + UMS9117_LCDC_DISP_SIZE);
	writel(0, udrm->lcdc + UMS9117_LCDC_LCM_START);
	writel(width | height << 16, udrm->lcdc + UMS9117_LCDC_LCM_SIZE);
	writel(0, udrm->lcdc + UMS9117_LCDC_BG_COLOR);
	writel(nv16 ? 0x2101 : udrm->rgb_img_ctrl,
	       udrm->lcdc + UMS9117_LCDC_IMG_CTRL);
	writel(y_address >> 2, udrm->lcdc + UMS9117_LCDC_IMG_Y_BASE);
	writel(uv_address >> 2, udrm->lcdc + UMS9117_LCDC_IMG_UV_BASE);
	writel(width | height << 16, udrm->lcdc + UMS9117_LCDC_IMG_SIZE_XY);
	writel(pitch_pixels, udrm->lcdc + UMS9117_LCDC_IMG_PITCH);
	writel(0, udrm->lcdc + UMS9117_LCDC_IMG_DISP_XY);
	writel(nv16 ? 1 : udrm->rgb_y2r_ctrl,
	       udrm->lcdc + UMS9117_LCDC_Y2R_CTRL);
	writel(nv16 ? 64 : udrm->rgb_y2r_contrast,
	       udrm->lcdc + UMS9117_LCDC_Y2R_CONTRAST);
	writel(nv16 ? 64 : udrm->rgb_y2r_saturation,
	       udrm->lcdc + UMS9117_LCDC_Y2R_SATURATION);
	writel(nv16 ? 0 : udrm->rgb_y2r_brightness,
	       udrm->lcdc + UMS9117_LCDC_Y2R_BRIGHTNESS);
	value = readl(udrm->lcdc + UMS9117_LCDC_CAP_CTRL);
	value &= ~(3 << 6);
	value |= ums9117_drm_uses_lcm(udrm) ? 2 << 6 : 0;
	value |= 0x20;
	writel(value, udrm->lcdc + UMS9117_LCDC_CAP_CTRL);
	writel(udrm->stream_phys >> 2, udrm->lcdc + UMS9117_LCDC_CAP_BASE);
	/* Complete the LCDC programming before arming and starting a frame. */
	wmb();
	reinit_completion(&udrm->frame_done);
	writel(UMS9117_LCDC_IRQ_DONE, udrm->lcdc + UMS9117_LCDC_IRQ_CLR);
	writel(UMS9117_LCDC_IRQ_DONE, udrm->lcdc + UMS9117_LCDC_IRQ_EN);
	value = readl(udrm->lcdc + UMS9117_LCDC_CTRL);
	value &= ~UMS9117_LCDC_CTRL_RGB_MODE_MASK;
	value &= ~udrm->profile->lcdc_ctrl_clear;
	value |= udrm->profile->lcdc_ctrl_set;
	spin_lock_irqsave(&udrm->lock, flags);
	udrm->in_flight = true;
	udrm->stats.frames_started++;
	udrm->last_y_address = y_address;
	udrm->last_uv_address = uv_address;
	udrm->last_transfer_rect = *damage;
	udrm->last_source_pitch_pixels = pitch_pixels;
	started_ns = ktime_get_ns();
	writel(value | UMS9117_LCDC_CTRL_RUN, udrm->lcdc + UMS9117_LCDC_CTRL);
	spin_unlock_irqrestore(&udrm->lock, flags);
	ret = ums9117_drm_wait_frame(udrm);
	if (!ret) {
		spin_lock_irqsave(&udrm->lock, flags);
		udrm->last_transfer_ns = ktime_get_ns() - started_ns;
		if (nv16)
			udrm->stats.present_nv16++;
		else
			udrm->stats.present_rgb565++;
		spin_unlock_irqrestore(&udrm->lock, flags);
	}
	return ret;
}

static int ums9117_drm_cold_init(struct ums9117_drm *udrm)
{
	int ret;

	ret = ums9117_drm_wled_off_bounded(udrm);
	if (!ret)
		ret = ums9117_drm_reset_panel(udrm,
					      udrm->profile->reset_phase_ms);
	if (!ret)
		msleep(udrm->profile->reset_release_ms);
	if (!ret) {
		if (ums9117_drm_uses_spi(udrm))
			ret = ums9117_drm_spi_post_reset(udrm);
		else if (ums9117_drm_uses_lcm(udrm))
			ret = ums9117_drm_lcm_post_reset(udrm);
		else
			ret = -ENODEV;
	}
	if (!ret)
		ret = ums9117_drm_run_commands(udrm, udrm->profile->init,
					       udrm->profile->init_count);
	if (!ret)
		ret = ums9117_drm_run_commands(
			udrm, udrm->profile->init_finish,
			udrm->profile->init_finish_count);
	return ret;
}

int ums9117_drm_display_enable_locked(struct ums9117_drm *udrm,
				      struct drm_plane_state *plane_state)
{
	struct drm_rect damage = DRM_RECT_INIT(0, 0, udrm->profile->width,
					       udrm->profile->height);
	unsigned int brightness;
	unsigned long flags;
	bool cold;
	int ret;

	cold = udrm->state == UMS9117_DRM_PANEL_STATE_COLD_INIT;
	if (udrm->stopping || udrm->state == UMS9117_DRM_PANEL_STATE_ERROR)
		return -EIO;
	spin_lock_irqsave(&udrm->lock, flags);
	udrm->state = UMS9117_DRM_PANEL_STATE_WAKING;
	spin_unlock_irqrestore(&udrm->lock, flags);
	ret = cold ? ums9117_drm_cold_init(udrm) : ums9117_drm_wake_panel(udrm);
	if (!ret)
		ret = ums9117_drm_display_send_frame_locked(udrm, plane_state,
							    &damage);
	if (!ret) {
		brightness = ums9117_drm_cached_backlight_level(udrm);
		ret = ums9117_drm_wled_set(udrm,
					   !udrm->backlight_max_brightness ||
						   brightness != 0,
					   brightness);
	}
	if (ret)
		return ret;
	spin_lock_irqsave(&udrm->lock, flags);
	udrm->state = UMS9117_DRM_PANEL_STATE_ACTIVE;
	if (!cold)
		udrm->stats.wake_count++;
	spin_unlock_irqrestore(&udrm->lock, flags);
	return 0;
}

int ums9117_drm_display_blank_locked(struct ums9117_drm *udrm)
{
	unsigned long flags;
	int ret;

	ums9117_drm_stop_lcdc(udrm);
	synchronize_irq(udrm->irq);
	if (udrm->state == UMS9117_DRM_PANEL_STATE_ERROR) {
		ums9117_drm_fail_dark(udrm);
		return 0;
	}
	spin_lock_irqsave(&udrm->lock, flags);
	udrm->state = UMS9117_DRM_PANEL_STATE_BLANKING;
	udrm->stats.blank_count++;
	spin_unlock_irqrestore(&udrm->lock, flags);
	ret = ums9117_drm_fail_dark(udrm);
	if (!ret)
		ret = ums9117_drm_dcs(udrm, MIPI_DCS_ENTER_SLEEP_MODE, NULL, 0);
	if (ret)
		return ret;
	msleep(udrm->profile->sleep_in_ms);
	spin_lock_irqsave(&udrm->lock, flags);
	udrm->state = UMS9117_DRM_PANEL_STATE_BLANKED;
	udrm->stats.blank_completed++;
	spin_unlock_irqrestore(&udrm->lock, flags);
	return 0;
}

static int ums9117_drm_map_common_resources(struct ums9117_drm *udrm,
					    struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	unsigned int i;
	int ret;

	udrm->lcdc = devm_platform_ioremap_resource_byname(pdev, "lcdc");
	if (IS_ERR(udrm->lcdc))
		return PTR_ERR(udrm->lcdc);
	udrm->ap_ahb_gate_set =
		ums9117_drm_ioremap_shared(pdev, "ap-ahb-gate-set");
	udrm->ap_ahb_reset_set =
		ums9117_drm_ioremap_shared(pdev, "ap-ahb-reset-set");
	udrm->ap_ahb_reset_clear =
		ums9117_drm_ioremap_shared(pdev, "ap-ahb-reset-clear");
	if (IS_ERR(udrm->ap_ahb_gate_set) || IS_ERR(udrm->ap_ahb_reset_set) ||
	    IS_ERR(udrm->ap_ahb_reset_clear))
		return -EINVAL;
	udrm->aon_apb =
		syscon_regmap_lookup_by_phandle(dev->of_node, "sprd,aon-apb");
	if (IS_ERR(udrm->aon_apb))
		return PTR_ERR(udrm->aon_apb);
	udrm->pmic = syscon_regmap_lookup_by_phandle(dev->of_node, "sprd,pmic");
	if (IS_ERR(udrm->pmic)) {
		/* The SPI PMIC publishes its regmap after probing, not via MMIO. */
		ret = PTR_ERR(udrm->pmic);
		return ret == -EINVAL ? -EPROBE_DEFER : ret;
	}
	ret = of_property_read_u32_array(dev->of_node,
					 "sprd,wled-current-levels",
					 udrm->wled_levels,
					 ARRAY_SIZE(udrm->wled_levels));
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(udrm->wled_levels); i++) {
		if (udrm->wled_levels[i] > SC2720_BLTC_CURRENT_LEVEL_MASK)
			return -EINVAL;
		if (!udrm->profile->wled_backlight_name ||
		    !udrm->wled_levels[i])
			continue;
		if (!udrm->backlight_max_brightness)
			udrm->backlight_max_brightness = udrm->wled_levels[i];
		else if (udrm->wled_levels[i] != udrm->backlight_max_brightness)
			return -EINVAL;
	}
	if (udrm->profile->wled_backlight_name &&
	    !udrm->backlight_max_brightness)
		return -EINVAL;
	udrm->backlight_effective = udrm->backlight_max_brightness;
	if (of_find_property(dev->of_node, "sprd,wled-default-level", NULL)) {
		ret = of_property_read_u32(dev->of_node,
					   "sprd,wled-default-level",
					   &udrm->backlight_effective);
		if (ret)
			return ret;
		if (!udrm->backlight_effective ||
		    udrm->backlight_effective > udrm->backlight_max_brightness)
			return -EINVAL;
	}
	return ums9117_drm_configure_pins(udrm, pdev);
}

int ums9117_drm_display_init(struct ums9117_drm *udrm,
			     struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	int ret;

	ret = ums9117_drm_map_common_resources(udrm, pdev);
	if (ret)
		return ret;
	if (ums9117_drm_uses_spi(udrm))
		ret = ums9117_drm_spi_init_transport(udrm, pdev);
	else if (ums9117_drm_uses_lcm(udrm))
		ret = ums9117_drm_lcm_init_transport(udrm, pdev);
	else
		ret = dev_err_probe(dev, -EINVAL,
				    "panel transport not built\n");
	if (ret)
		return ret;
	udrm->irq = platform_get_irq(pdev, 0);
	if (udrm->irq < 0)
		return udrm->irq;
	writel(UMS9117_AP_AHB_LCDC_GATE | UMS9117_AP_AHB_LCM_GATE,
	       udrm->ap_ahb_gate_set);
	usleep_range(1000, 2000);
	if (ums9117_drm_uses_spi(udrm))
		ret = ums9117_drm_spi_enable_transport(udrm);
	else if (ums9117_drm_uses_lcm(udrm))
		ret = ums9117_drm_lcm_enable_transport(udrm);
	else
		ret = -ENODEV;
	if (ret)
		return ret;
	writel(UMS9117_AP_AHB_LCDC_RESET, udrm->ap_ahb_reset_set);
	usleep_range(10000, 11000);
	writel(UMS9117_AP_AHB_LCDC_RESET, udrm->ap_ahb_reset_clear);
	ums9117_drm_stop_lcdc(udrm);
	udrm->rgb_img_ctrl = readl(udrm->lcdc + UMS9117_LCDC_IMG_CTRL);
	udrm->rgb_img_ctrl &= ~(BIT(1) | (0xf << 4) | (3 << 8));
	udrm->rgb_img_ctrl |= BIT(0) | (5 << 4) | (2 << 8);
	udrm->rgb_y2r_ctrl = readl(udrm->lcdc + UMS9117_LCDC_Y2R_CTRL);
	udrm->rgb_y2r_contrast = readl(udrm->lcdc + UMS9117_LCDC_Y2R_CONTRAST);
	udrm->rgb_y2r_saturation =
		readl(udrm->lcdc + UMS9117_LCDC_Y2R_SATURATION);
	udrm->rgb_y2r_brightness =
		readl(udrm->lcdc + UMS9117_LCDC_Y2R_BRIGHTNESS);
	ret = devm_request_irq(dev, udrm->irq, ums9117_drm_lcdc_irq, 0,
			       dev_name(dev), udrm);
	if (ret)
		return ret;
	ret = ums9117_drm_wled_off_bounded(udrm);
	if (ret)
		return ret;
	return 0;
}

int ums9117_drm_display_register_backlight(struct ums9117_drm *udrm,
					   struct platform_device *pdev)
{
	struct backlight_properties properties = {
		.type = BACKLIGHT_RAW,
		.scale = BACKLIGHT_SCALE_UNKNOWN,
	};

	if (!udrm->profile->wled_backlight_name)
		return 0;
	if (!udrm->backlight_max_brightness)
		return -EINVAL;
	properties.max_brightness = udrm->backlight_max_brightness;
	properties.brightness = udrm->backlight_effective;
	properties.power = BACKLIGHT_POWER_ON;
	udrm->backlight = devm_backlight_device_register(
		&pdev->dev, udrm->profile->wled_backlight_name, &pdev->dev,
		udrm, &ums9117_drm_backlight_ops, &properties);
	if (IS_ERR(udrm->backlight)) {
		int ret = PTR_ERR(udrm->backlight);

		udrm->backlight = NULL;
		return ret;
	}
	return 0;
}

void ums9117_drm_display_shutdown(struct ums9117_drm *udrm)
{
	unsigned long flags;

	mutex_lock(&udrm->panel_lock);
	spin_lock_irqsave(&udrm->lock, flags);
	udrm->stopping = true;
	spin_unlock_irqrestore(&udrm->lock, flags);
	ums9117_drm_stop_lcdc(udrm);
	synchronize_irq(udrm->irq);
	ums9117_drm_fail_dark(udrm);
	mutex_unlock(&udrm->panel_lock);
}
