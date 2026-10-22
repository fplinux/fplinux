/* SPDX-License-Identifier: GPL-2.0-only */
#include "calculator-observer.h"

#include <assert.h>
#include <errno.h>
#include <fplinux/fplinux-ui-prefs.h>
#include <fplinux/fplinux-ui-screen.h>
#include <lvgl/lvgl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

void __real_lv_label_set_text(lv_obj_t *label, const char *text);

/* Observe the real label after its text and font have reached LVGL. */
void __wrap_lv_label_set_text(lv_obj_t *label, const char *text)
{
	const char *descriptor = getenv("FPLINUX_CALCULATOR_OBSERVER_FD");
	struct calculator_number_observation observation = { 0 };
	const lv_font_t *font;
	lv_obj_t *parent;
	lv_point_t size;

	__real_lv_label_set_text(label, text);
	if (!descriptor || !text || !text[0] ||
	    (strcmp(text, "Error") &&
	     strspn(text, "0123456789.,-eE") != strlen(text)))
		return;
	assert(strlen(text) < sizeof(observation.text));
	lv_obj_update_layout(label);
	parent = lv_obj_get_parent(label);
	font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
	assert(parent && font);
	for (size_t index = 0; text[index]; ++index) {
		lv_font_glyph_dsc_t glyph;

		assert(lv_font_get_glyph_dsc(font, &glyph,
					     (unsigned char)text[index],
					     (unsigned char)text[index + 1]));
	}
	lv_text_get_size(&size, text, font, 0, 0, LV_COORD_MAX,
			 LV_TEXT_FLAG_NONE);
	strcpy(observation.text, text);
	observation.width = lv_obj_get_width(label);
	observation.height = lv_obj_get_height(label);
	observation.text_width = size.x;
	observation.text_height = size.y;
	observation.left = lv_obj_get_x(label);
	observation.top = lv_obj_get_y(label);
	observation.parent_width = lv_obj_get_width(parent);
	observation.parent_height = lv_obj_get_height(parent);
	assert(send(atoi(descriptor), &observation, sizeof(observation), 0) ==
	       (ssize_t)sizeof(observation));
}

struct fplinux_storage_result
__real_fplinux_ui_prefs_save(struct fplinux_ui_prefs *prefs);

struct fplinux_storage_result
__wrap_fplinux_ui_prefs_save(struct fplinux_ui_prefs *prefs)
{
	const char *gate = getenv("FPLINUX_CALCULATOR_SAVE_FAILURE");

	if (gate && access(gate, F_OK) == 0)
		return (struct fplinux_storage_result){
			.state = FPLINUX_STORAGE_UNPUBLISHED, .error = ENOSPC
		};
	return __real_fplinux_ui_prefs_save(prefs);
}

struct fplinux_ui_screen *
__real_fplinux_ui_alert_new(struct fplinux_ui *ui,
			    enum fplinux_ui_alert_kind kind, const char *title,
			    const char *text, unsigned int timeout_ms);

struct fplinux_ui_screen *
__wrap_fplinux_ui_alert_new(struct fplinux_ui *ui,
			    enum fplinux_ui_alert_kind kind, const char *title,
			    const char *text, unsigned int timeout_ms)
{
	struct fplinux_ui_screen *screen =
		__real_fplinux_ui_alert_new(ui, kind, title, text, timeout_ms);
	const char *descriptor = getenv("FPLINUX_CALCULATOR_OBSERVER_FD");
	struct calculator_number_observation observation = {
		.kind = CALCULATOR_ALERT,
	};

	if (screen && descriptor) {
		snprintf(observation.text, sizeof(observation.text), "%s",
			 text ? text : "");
		snprintf(observation.title, sizeof(observation.title), "%s",
			 title ? title : "");
		assert(send(atoi(descriptor), &observation, sizeof(observation),
			    0) == (ssize_t)sizeof(observation));
	}
	return screen;
}
