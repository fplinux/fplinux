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

#include <fplinux/fplinux-ui-lights.h>
#include <fplinux/fplinux-ui-session.h>
#include <fplinux/fplinux-ui-vibration.h>

#include "fplinux-pm-canvas-core.h"
#include "fplinux-pm-canvas.h"

static struct {
	bool canvas_shown;
	bool flashing;
	bool screen_on;
	bool visible;
	unsigned int requested_level;
	uint32_t light_serial;
	lv_timer_t *flash_timer;
	lv_timer_t *light_retry;
} device;

static void apply_requested_level(void);

static void retry_light(lv_timer_t *timer)
{
	lv_timer_pause(timer);
	apply_requested_level();
}

static void schedule_light_retry(void)
{
	if (!device.visible)
		return;
	if (!device.light_retry)
		device.light_retry = lv_timer_create(retry_light, 250, NULL);
	if (device.light_retry) {
		lv_timer_reset(device.light_retry);
		lv_timer_resume(device.light_retry);
	}
}

static void apply_requested_level(void)
{
	struct fplinux_ui *ui = fplinux_pm_canvas_ui();
	struct fplinux_ui_light_effect effect = {
		.resources = FPLINUX_UI_LIGHT_SCREEN,
		.display_level = device.requested_level,
	};

	if (!device.visible || !effect.display_level || device.light_serial ||
	    !(fplinux_ui_light_effects_supported(ui) & FPLINUX_UI_LIGHT_SCREEN))
		return;
	if (!fplinux_ui_light_effects_apply(ui, &effect,
					    &device.light_serial) &&
	    errno == EAGAIN)
		schedule_light_retry();
}

void fplinux_pm_device_visibility(bool visible)
{
	if (device.visible == visible)
		return;
	device.visible = visible;
	if (visible) {
		apply_requested_level();
	} else {
		device.light_serial = 0;
		if (device.light_retry)
			lv_timer_pause(device.light_retry);
	}
}

void fplinux_pm_device_session(const struct fplinux_ui_session_message *reply)
{
	if (!reply || !device.light_serial ||
	    reply->serial != device.light_serial)
		return;
	if (reply->type == FPLINUX_UI_SESSION_LIGHT_EFFECT) {
		device.light_serial = 0;
		if (reply->light_effect.display_level != device.requested_level)
			apply_requested_level();
	} else if (reply->type == FPLINUX_UI_SESSION_ERROR &&
		   !strcmp(reply->name, "light_effect")) {
		device.light_serial = 0;
		/* At launch, the busy scene can still cover the first MIDlet frame. */
		if (!strcmp(reply->reason, "not-visible"))
			schedule_light_retry();
	}
}

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
	if (device.light_retry)
		lv_timer_delete(device.light_retry);
	device.light_retry = NULL;
	device.flashing = false;
	device.canvas_shown = false;
	if (device.requested_level && ui)
		fplinux_ui_light_effects_clear(ui, NULL);
	device.requested_level = 0;
	device.light_serial = 0;
	device.visible = false;
	update_screen_on();
	if (ui)
		fplinux_ui_vibration_stop(ui);
}

/* A guest request is transient; Session restores the user's brightness. */
KNIEXPORT KNI_RETURNTYPE_VOID
KNIDECL(com_nokia_mid_ui_DeviceControl_setBacklight0)
{
	unsigned int requested = KNI_GetParameterAsInt(1);

	device.requested_level = requested ? (requested + 9) / 10 : 1;
	apply_requested_level();
	KNI_ReturnVoid();
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
