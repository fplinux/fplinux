// SPDX-License-Identifier: GPL-2.0-only
/*
 * fplinux-check: package-embedded
 *
 * fplinux-phoneme device services of LCDUI:
 * - Display.vibrate(duration) plays pulses of at most 3000 ms through
 *   fplinux_ui_vibration_play(); duration 0 stops (natives of the port's
 *   com.sun.midp.lcdui.DisplayDeviceAccess);
 * - Display.flashBacklight(duration) keeps the display on that long, as the
 *   session manager owns the lights;
 * - the display also stays on while a Canvas is shown;
 * - the annunciator port (highlevelui/annunciator/include) that upstream
 *   lcdui_vibrate.c and lcdui_indicators.c still call: requests succeed.
 * Headless (no runtime) vibration and light report false.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <kni.h>
#include <anc_indicators.h>
#include <anc_vibrate.h>

#include <fplinux/fplinux-ui-vibration.h>

#include "fplinux-pm-canvas-core.h"
#include "fplinux-pm-canvas.h"

static struct {
	bool canvas_shown;
	bool flashing;
	bool screen_on;
	lv_timer_t *flash_timer;
} device;

static void update_screen_on(void)
{
	struct fplinux_ui *ui = fplinux_pm_canvas_ui();
	bool wanted = device.canvas_shown || device.flashing;

	if (!ui || wanted == device.screen_on)
		return;
	if (fplinux_ui_keep_screen_on(ui, wanted))
		device.screen_on = wanted;
}

void fplinux_pm_device_canvas_shown(bool shown)
{
	device.canvas_shown = shown;
	update_screen_on();
}

static void flash_ended(lv_timer_t *timer)
{
	lv_timer_pause(timer);
	device.flashing = false;
	update_screen_on();
}

void fplinux_pm_device_cleanup(void)
{
	struct fplinux_ui *ui = fplinux_pm_canvas_ui();

	if (device.flash_timer)
		lv_timer_delete(device.flash_timer);
	device.flash_timer = NULL;
	device.flashing = false;
	device.canvas_shown = false;
	update_screen_on();
	if (ui)
		fplinux_ui_vibration_stop(ui);
}

/* private static native boolean vibrate1(int duration); */
KNIEXPORT KNI_RETURNTYPE_BOOLEAN
KNIDECL(com_sun_midp_lcdui_DisplayDeviceAccess_vibrate1)
{
	struct fplinux_ui *ui = fplinux_pm_canvas_ui();
	jint duration = KNI_GetParameterAsInt(1);
	unsigned int pattern[FPLINUX_UI_VIBRATION_PATTERN_MAX];
	size_t count;
	jboolean result = KNI_FALSE;

	if (ui && fplinux_ui_vibration_supported(ui)) {
		count = fplinux_pm_vibration_pattern(
			duration > 0 ? (unsigned int)duration : 0u, pattern,
			FPLINUX_UI_VIBRATION_PATTERN_MAX);
		if (!count)
			result = fplinux_ui_vibration_stop(ui) ? KNI_TRUE :
								 KNI_FALSE;
		else if (fplinux_ui_vibration_play(ui, pattern, count, false))
			result = KNI_TRUE;
		else if (errno != EAGAIN)
			fprintf(stderr, "fplinux-phoneme: vibration: %s\n",
				strerror(errno));
	}
	KNI_ReturnBoolean(result);
}

/* private static native boolean flashBacklight1(int duration); */
KNIEXPORT KNI_RETURNTYPE_BOOLEAN
KNIDECL(com_sun_midp_lcdui_DisplayDeviceAccess_flashBacklight1)
{
	jint duration = KNI_GetParameterAsInt(1);

	if (!fplinux_pm_canvas_ui())
		KNI_ReturnBoolean(KNI_FALSE);
	if (duration > 0 && !device.flash_timer)
		device.flash_timer =
			lv_timer_create(flash_ended, (uint32_t)duration, NULL);
	if (duration > 0 && device.flash_timer) {
		lv_timer_set_period(device.flash_timer, (uint32_t)duration);
		lv_timer_reset(device.flash_timer);
		lv_timer_resume(device.flash_timer);
		device.flashing = true;
	} else {
		if (device.flash_timer)
			lv_timer_pause(device.flash_timer);
		device.flashing = false;
	}
	update_screen_on();
	KNI_ReturnBoolean(KNI_TRUE);
}

/* ---- annunciator port ------------------------------------------------- */

jboolean anc_start_vibrate(void)
{
	return KNI_TRUE;
}

jboolean anc_stop_vibrate(void)
{
	return KNI_TRUE;
}

jboolean anc_show_backlight(AncBacklightState mode)
{
	(void)mode;
	return KNI_TRUE;
}

void anc_show_trusted_indicator(jboolean trusted)
{
	(void)trusted;
}

void anc_toggle_home_icon(jboolean isHomeOn)
{
	(void)isHomeOn;
}
