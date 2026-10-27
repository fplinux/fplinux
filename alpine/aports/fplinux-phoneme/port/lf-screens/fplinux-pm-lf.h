/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * fplinux-check: package-embedded
 *
 * Shared native side of the LCDUI look and feel (port/lf-screens/, used by
 * port/lf-form/ and port/canvas/ too). Peers are libfplinux-ui screens;
 * javax.microedition.lcdui.LFNative and DisplayableLFImpl own the screen
 * stack, the screen listener and the screen data.
 *
 * Natives run inside a VM slice on the runtime thread. Without a runtime
 * (the headless runner) fplinux_pm_app_ui() is NULL and every native must
 * do nothing.
 */
#ifndef FPLINUX_PM_LF_H
#define FPLINUX_PM_LF_H

#include <kni.h>
#include <lvgl/lvgl.h>

struct fplinux_ui_screen;

/* The id of the MIDlet's one Display (gForegroundDisplayId). */
#define FPLINUX_PM_LF_DISPLAY_ID 1

/* PEER_CHANGED_EVENT subtypes; 16 and up belong to the Form LF. */
#define FPLINUX_PM_LF_PEER_ITEM_CHANGED 1
#define FPLINUX_PM_LF_PEER_LIST_SELECT 2

/* A malloc()ed UTF-8 copy of a Java String; NULL for null or no memory. */
char *fplinux_pm_lf_utf8(jobject string);

/* The serial DisplayableLFImpl attached to a peer screen; 0 when none. */
int fplinux_pm_lf_serial(const struct fplinux_ui_screen *screen);

/*
 * Queues MIDP_PEER_CHANGED_EVENT for the current FormLF:
 * uCallPeerStateChanged(serial, sub_type, index, hint). Only from runtime
 * callbacks, outside a slice; nothing happens once the VM stopped running.
 */
void fplinux_pm_lf_post_peer_changed(int serial, int sub_type, int index,
				     int hint);

/*
 * An LVGL image of a javax.microedition.lcdui.ImageData: RGB565, or
 * RGB565A8 when the image has alpha. Returns the pixel copy the descriptor
 * points at, which the caller frees after the runtime call that copies the
 * image (fplinux_ui_list_set_icon() and the like); NULL for a null handle,
 * an empty image or no memory.
 */
void *fplinux_pm_lf_image(jobject image_data, lv_image_dsc_t *dsc);

#endif
