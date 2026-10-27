/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * fplinux-check: package-embedded
 *
 * Shared state of the phoneME Canvas port (port/canvas/): the display port
 * with its Canvas peers (fplinux-pm-screen.c), keys and key repeat
 * (fplinux-pm-keys.c), vibration and the display light
 * (fplinux-pm-device.c). Everything runs on the runtime thread: natives
 * inside a VM slice, runtime callbacks outside one. Without a runtime
 * (fplinux_pm_app_ui() == NULL, the headless runner) nothing is drawn and
 * the calls below do nothing.
 */
#ifndef FPLINUX_PM_CANVAS_H
#define FPLINUX_PM_CANVAS_H

#include <stdbool.h>

#include <fplinux/fplinux-ui.h>
#include <fplinux/fplinux-ui-screen.h>

/* fplinux-pm-screen.c */

/* The runtime of this VM run, NULL headless; set by lcdlf_ui_init(). */
struct fplinux_ui *fplinux_pm_canvas_ui(void);

/* The Canvas peer whose Canvas is shown, so keys may reach it. */
bool fplinux_pm_canvas_is_active(const struct fplinux_ui_screen *screen);

/* fplinux-pm-keys.c */

/* The key handler of every Canvas peer: MIDP key events and repeats. */
void fplinux_pm_canvas_key(struct fplinux_ui *ui,
			   struct fplinux_ui_screen *canvas,
			   const struct fplinux_ui_key *key, void *context);

/* Stops the key repeat, e.g. when the Canvas goes away. */
void fplinux_pm_keys_stop(void);
void fplinux_pm_keys_cancel(enum fplinux_ui_key_source source);

/* Releases the repeat timer; called when the display port finishes. */
void fplinux_pm_keys_cleanup(void);

/* fplinux-pm-device.c */

/* A Canvas is shown: the display stays on meanwhile. */
void fplinux_pm_device_canvas_shown(bool shown);

/* Stops vibration and light requests; called when the port finishes. */
void fplinux_pm_device_cleanup(void);

#endif
