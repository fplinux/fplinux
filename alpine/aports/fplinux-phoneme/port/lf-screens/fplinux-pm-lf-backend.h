/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * fplinux-check: package-embedded
 *
 * Internal to port/lf-screens/: the libfplinux-ui calls of the screen
 * natives behind one table, so that a headless run can trace them. The
 * runtime backend is libfplinux-ui itself. The trace backend
 * (fplinux-pm-lf-trace.c), selected in the headless runner by
 * FPLINUX_PM_LF_TRACE=1, keeps a model of the screen stack, prints each
 * call as a "[lf] " line on stdout, and plays the scripted key presses of
 * FPLINUX_PM_LF_SCRIPT, so that the LF can be tested without a screen.
 */
#ifndef FPLINUX_PM_LF_BACKEND_H
#define FPLINUX_PM_LF_BACKEND_H

#include <stdbool.h>
#include <stddef.h>

#include <fplinux/fplinux-ui-screen.h>
#include <fplinux/fplinux-ui.h>

struct lf_backend {
	struct fplinux_ui *ui;
	bool (*replace)(struct fplinux_ui *ui,
			struct fplinux_ui_screen *screen);
	bool (*push)(struct fplinux_ui *ui, struct fplinux_ui_screen *screen);
	bool (*pop)(struct fplinux_ui *ui);
	struct fplinux_ui_screen *(*current)(const struct fplinux_ui *ui);
	void (*set_listener)(struct fplinux_ui_screen *screen,
			     fplinux_ui_command_cb command,
			     fplinux_ui_change_cb change, void *context);
	void (*set_data)(struct fplinux_ui_screen *screen, void *data);
	void *(*get_data)(const struct fplinux_ui_screen *screen);
	bool (*set_title)(struct fplinux_ui_screen *screen, const char *title);
	bool (*add_command)(struct fplinux_ui_screen *screen,
			    const struct fplinux_ui_command *command);
	void (*remove_command)(struct fplinux_ui_screen *screen, int id);
	bool (*content_size)(const struct fplinux_ui_screen *screen, int *width,
			     int *height);
	size_t (*command_item)(const struct fplinux_ui_screen *screen);
	struct fplinux_ui_screen *(*list_new)(struct fplinux_ui *ui,
					      const char *title,
					      enum fplinux_ui_list_kind kind);
	size_t (*list_insert)(struct fplinux_ui_screen *list, size_t index,
			      const char *text, const char *detail);
	bool (*list_delete)(struct fplinux_ui_screen *list, size_t index);
	void (*list_delete_all)(struct fplinux_ui_screen *list);
	bool (*list_set)(struct fplinux_ui_screen *list, size_t index,
			 const char *text, const char *detail);
	bool (*list_set_icon)(struct fplinux_ui_screen *list, size_t index,
			      const void *src,
			      enum fplinux_ui_list_icon_kind kind);
	size_t (*list_size)(const struct fplinux_ui_screen *list);
	size_t (*list_get_focused)(const struct fplinux_ui_screen *list);
	void (*list_set_focused)(struct fplinux_ui_screen *list, size_t index);
	bool (*list_is_selected)(const struct fplinux_ui_screen *list,
				 size_t index);
	void (*list_set_selected)(struct fplinux_ui_screen *list, size_t index,
				  bool selected);
	bool (*list_set_select_label)(struct fplinux_ui_screen *list,
				      const char *label);
	struct fplinux_ui_screen *(*alert_new)(struct fplinux_ui *ui,
					       enum fplinux_ui_alert_kind kind,
					       const char *title,
					       const char *text,
					       unsigned int timeout_ms);
	bool (*alert_set_text)(struct fplinux_ui_screen *alert,
			       const char *text);
	bool (*alert_set_progress)(struct fplinux_ui_screen *alert, int value,
				   int maximum);
};

/*
 * The runtime backend while the app has a runtime connection, the trace
 * backend in a traced headless run, otherwise NULL (natives do nothing).
 */
const struct lf_backend *lf_backend(void);

/* fplinux-pm-lf-trace.c */
bool lf_trace_enabled(void);
const struct lf_backend *lf_trace_backend(void);
/* Milliseconds until the next scripted step; -1 when none is left. */
int lf_trace_next_delay(void);
/* Plays the next scripted step (a press or a dump). */
void lf_trace_run_step(void);
/* Java has shown this peer (Display's current Displayable). */
void lf_trace_shown(const struct fplinux_ui_screen *screen);

#endif
