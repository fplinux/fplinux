/* SPDX-License-Identifier: 0BSD */
#ifndef APIDEMO_FORMS_H
#define APIDEMO_FORMS_H

#include "apidemo.h"

#include <time.h>

/* Command ids of the demo screens; each screen uses a few of them. */
enum {
	COMMAND_CHANGE = 1,
	COMMAND_REMOVE_TEXT,
	COMMAND_SHOW,
	COMMAND_CALL,
	COMMAND_LOCK,
	COMMAND_MASK,
	COMMAND_ANY,
	COMMAND_NUMERIC,
	COMMAND_DECIMAL,
	COMMAND_PHONE,
	COMMAND_CHOOSE_ALL,
	COMMAND_RESET,
	COMMAND_START,
	COMMAND_NOW,
	COMMAND_MOVE_UP,
	COMMAND_MOVE_DOWN,
	COMMAND_HIDE,
	COMMAND_DELETE,
	COMMAND_INSERT,
	COMMAND_SHOW_HIDDEN,
	COMMAND_FOCUS_LAST,
	COMMAND_LIST_ITEMS,
	COMMAND_DELETE_ALL,
	COMMAND_SEND,
	COMMAND_TODAY,
	COMMAND_MUTE,
	COMMAND_PLAY,
	COMMAND_CALLS,
	COMMAND_OPEN,
	COMMAND_COPY,
	COMMAND_VIEW,
	COMMAND_SHOW_FOCUS,
	COMMAND_CHANGE_BUTTON,
	COMMAND_ARM,
};

void apidemo_forms_add_command(struct fplinux_ui_screen *screen, int id,
			       enum fplinux_ui_command_type type,
			       const char *label, int priority,
			       const void *icon);
void apidemo_forms_append_text(char *text, size_t size, const char *format, ...)
	__attribute__((format(printf, 3, 4)));
void apidemo_forms_describe_result(char *text, size_t size, bool done,
				   int error);
size_t apidemo_forms_append_status(struct fplinux_ui_screen *form);
void apidemo_forms_show_status(struct fplinux_ui_screen *form, size_t status,
			       const char *format, ...)
	__attribute__((format(printf, 3, 4)));
time_t apidemo_forms_sample_time(void);

void apidemo_forms_open_strings(struct apidemo *app,
				const struct apidemo_entry *entry);
void apidemo_forms_open_fields(struct apidemo *app,
			       const struct apidemo_entry *entry);
void apidemo_forms_open_constraint(struct apidemo *app,
				   const struct apidemo_entry *entry);
void apidemo_forms_open_choices(struct apidemo *app,
				const struct apidemo_entry *entry);
void apidemo_forms_open_gauges(struct apidemo *app,
			       const struct apidemo_entry *entry);
void apidemo_forms_open_dates(struct apidemo *app,
			      const struct apidemo_entry *entry);
void apidemo_forms_open_images(struct apidemo *app,
			       const struct apidemo_entry *entry);
void apidemo_forms_open_items(struct apidemo *app,
			      const struct apidemo_entry *entry);
void apidemo_forms_open_item_commands(struct apidemo *app,
				      const struct apidemo_entry *entry);
void apidemo_forms_open_links(struct apidemo *app,
			      const struct apidemo_entry *entry);
void apidemo_forms_visibility(struct apidemo *app, bool visible);
void apidemo_forms_cleanup(struct apidemo *app);

#endif
