/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef FPLINUX_UI_FILE_PICKER_H
#define FPLINUX_UI_FILE_PICKER_H

#include <stdbool.h>

#include "fplinux-ui-storage.h"

struct fplinux_ui_file_picker;

enum fplinux_ui_file_picker_mode {
	FPLINUX_UI_FILE_PICKER_FILE,
	FPLINUX_UI_FILE_PICKER_FOLDER,
};

enum fplinux_ui_file_picker_status {
	FPLINUX_UI_FILE_PICKER_SELECTED,
	FPLINUX_UI_FILE_PICKER_CANCELLED,
	FPLINUX_UI_FILE_PICKER_ERROR,
};

struct fplinux_ui_file_picker_options {
	enum fplinux_ui_file_picker_mode mode;
	/* Absolute My files path. All strings and extensions are copied. */
	const char *home;
	const char *title;
	/* Direct folder action, for example "Play folder". NULL uses "Here". */
	const char *folder_action;
	/* NULL-terminated ASCII extensions without dots; NULL accepts all files. */
	const char *const *extensions;
	/* Folder destinations omit read-only volumes. */
	bool writable;
};

/*
 * SELECTED supplies an absolute UTF-8 path borrowed until the callback
 * returns. CANCELLED and ERROR have no path; ERROR supplies positive errno.
 * A picker remains open after SELECTED. The owner closes it explicitly.
 * All callbacks run on the UI thread and may close the picker.
 */
typedef void (*fplinux_ui_file_picker_cb)(
	struct fplinux_ui_file_picker *picker,
	enum fplinux_ui_file_picker_status status, const char *path, int error,
	void *context);

struct fplinux_ui_file_picker *fplinux_ui_file_picker_open(
	struct fplinux_ui *ui,
	const struct fplinux_ui_file_picker_options *options,
	fplinux_ui_file_picker_cb callback, void *context);

/* Forward storage results from the application's single storage handler. */
void fplinux_ui_file_picker_storage_result(
	struct fplinux_ui_file_picker *picker,
	const struct fplinux_volume_result *result);
/* Close invokes no callback. */
void fplinux_ui_file_picker_close(struct fplinux_ui_file_picker *picker);

#endif
