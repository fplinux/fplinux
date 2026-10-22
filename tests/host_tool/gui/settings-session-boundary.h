/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_TEST_SETTINGS_SESSION_BOUNDARY_H
#define FPLINUX_TEST_SETTINGS_SESSION_BOUNDARY_H

#include <fplinux/fplinux-ui.h>
#include <fplinux/fplinux-ui-session.h>

struct settings_session_boundary {
	void (*flush)(void *context, struct fplinux_ui *ui);
	void (*home)(void *context);
	void (*resume)(void *context);
	void (*recover_storage)(void *context);
	void *context;
};

int settings_session_test(bool theme,
			  const struct settings_session_boundary *boundary);
int settings_session_save_failure_test(
	const struct settings_session_boundary *boundary);
int settings_session_preview_retry_test(
	const struct settings_session_boundary *boundary);
bool settings_session_next_request(struct fplinux_ui *ui, size_t *index,
				   struct fplinux_ui_session_message *message);
void settings_session_receive(struct fplinux_ui *ui,
			      const struct fplinux_ui_session_message *message);

#endif
