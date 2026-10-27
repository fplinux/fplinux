/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_LAUNCHER_TASKS_H
#define FPLINUX_LAUNCHER_TASKS_H

#include <stdbool.h>
#include <stddef.h>

#include "fplinux-ui-screen.h"
#include "fplinux-ui-session.h"

struct fplinux_ui;

struct fplinux_launcher_task_icon {
	const void *source;
	enum fplinux_ui_list_icon_kind kind;
};

struct fplinux_launcher_tasks_listener {
	/* Borrowed image source; the List copies it into the row. */
	struct fplinux_launcher_task_icon (*icon)(const char *app_id,
						  void *context);
	void (*view_changed)(void *context);
	void *context;
};

struct fplinux_launcher_tasks {
	struct fplinux_ui *ui;
	struct fplinux_ui_screen *list;
	struct fplinux_ui_screen *alert;
	struct fplinux_launcher_tasks_listener listener;
	struct fplinux_ui_session_task tasks[FPLINUX_UI_SESSION_TASK_MAX];
	/* Confirmation keeps the instance selected before later snapshots. */
	struct fplinux_ui_session_task confirmed;
	size_t count;
	bool known;
	bool querying;
	bool request_pending;
	bool blocked;
};

void fplinux_launcher_tasks_init(
	struct fplinux_launcher_tasks *tasks, struct fplinux_ui *ui,
	const struct fplinux_launcher_tasks_listener *listener);
bool fplinux_launcher_tasks_is_open(const struct fplinux_launcher_tasks *tasks);
void fplinux_launcher_tasks_open(struct fplinux_launcher_tasks *tasks);
/* Includes an Options menu opened by the native List. */
void fplinux_launcher_tasks_close(struct fplinux_launcher_tasks *tasks);
void fplinux_launcher_tasks_set_blocked(struct fplinux_launcher_tasks *tasks,
					bool blocked);
/* Receives decoded session packets; true when Tasks handled the packet. */
bool fplinux_launcher_tasks_receive(
	struct fplinux_launcher_tasks *tasks,
	const struct fplinux_ui_session_message *message);

#endif
