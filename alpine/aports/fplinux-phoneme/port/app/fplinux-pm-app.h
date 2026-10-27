/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * What the app shell offers the VM's natives (LCDUI peers, Canvas). They run
 * inside fplinux_pm_vm_slice() on the runtime thread, so they may call
 * libfplinux-ui directly; they must never call the VM from a runtime
 * callback, only queue an event and let the wakeup hook schedule a slice.
 *
 * The screen stack: while the MIDlet runs, the app keeps its suite screen at
 * the bottom, with a "Starting" alert over it until the
 * MIDlet shows something. The manager owns the delayed busy indicator.
 * Display.setCurrent() replaces the top screen with fplinux_ui_replace(),
 * so the first call replaces that alert or the suite; the MIDlet's
 * screens never pop below their own first one.
 */
#ifndef FPLINUX_PM_APP_H
#define FPLINUX_PM_APP_H

#include <stdbool.h>

#define FPLINUX_PM_APP_ID "org.fplinux.phoneme"

struct fplinux_ui;

/* The app's runtime connection; NULL before fplinux_ui_open() succeeds. */
struct fplinux_ui *fplinux_pm_app_ui(void);

/* A real LF screen or completed Canvas frame ends the launch indicator. */
void fplinux_pm_app_loaded(void);

/*
 * Canvas.setFullScreenMode(): asks the runtime for the whole display (true)
 * or for the integrated layout with the status and soft-key bars (false).
 * The switch is asynchronous and ends with a resize; false when the runtime
 * refuses it. The app starts integrated, so a MIDlet without a full-screen
 * Canvas never switches.
 */
bool fplinux_pm_app_set_fullscreen(bool fullscreen);

/*
 * Called after each FPLINUX_UI_EVENT_RESIZE, once fplinux_ui_width(),
 * fplinux_ui_height() and fplinux_ui_is_fullscreen() report the new surface:
 * the place to queue Canvas.sizeChanged(). One handler; NULL removes it.
 */
typedef void (*fplinux_pm_app_resize_fn)(void *context);
void fplinux_pm_app_set_resize_handler(fplinux_pm_app_resize_fn handler,
				       void *context);

#endif
