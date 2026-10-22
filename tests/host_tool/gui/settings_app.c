/* SPDX-License-Identifier: GPL-2.0-only */
#define _POSIX_C_SOURCE 200809L
#include "fplinux-language.h"
#include "settings-session-boundary.h"

#include <fplinux/fplinux-ui.h>
#include <fplinux/fplinux-ui-locale.h>
#include <fplinux/fplinux-ui-screen.h>
#include <fplinux/fplinux-ui-session.h>

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * The real Settings application drives this test-owned screen stack and
 * session outbox. Rendering, Wayland, timer scheduling and the installed
 * language store are controlled boundaries, not exercised runtimes.
 */
struct row {
	char text[128];
	char detail[128];
	int value;
	time_t date;
	bool selected;
	bool image;
};

struct fplinux_ui_screen {
	char title[128];
	char message[128];
	struct row rows[16];
	size_t count;
	size_t focused;
	struct fplinux_ui_command commands[4];
	size_t command_count;
	fplinux_ui_command_cb command;
	fplinux_ui_change_cb change;
	void *context;
	bool is_form;
	char image_source[256];
	unsigned int image_attempts;
};

struct fplinux_ui {
	fplinux_ui_event_cb event;
	void *context;
	struct fplinux_ui_screen *stack[8];
	size_t depth;
	struct fplinux_ui_session_message sent[64];
	size_t sent_count;
	struct fplinux_ui_theme theme;
	struct fplinux_ui_metrics metrics;
	char locale[4];
	char input_locale[4];
	unsigned int alerts;
	bool visible;
	enum fplinux_ui_lifecycle lifecycle;
};

struct lv_timer_t {
	void *data;
};

static const char *scenario;
static bool resources_opened;
static int selected_profile;
static bool dark_theme;
static const char *expected_logo;
static const struct settings_session_boundary *session_boundary;
int fplinux_settings_main(int argc, char **argv);

static void copy_text(char *target, size_t size, const char *text)
{
	int length = snprintf(target, size, "%s", text ? text : "");

	assert(length >= 0 && (size_t)length < size);
}

static struct fplinux_ui_screen *new_screen(const char *title)
{
	struct fplinux_ui_screen *screen = calloc(1, sizeof(*screen));

	assert(screen);
	copy_text(screen->title, sizeof(screen->title), title);
	return screen;
}

static struct fplinux_ui_screen *current(struct fplinux_ui *ui,
					 const char *title)
{
	assert(ui->depth);
	assert(!strcmp(ui->stack[ui->depth - 1]->title, title));
	return ui->stack[ui->depth - 1];
}

static size_t row_named(const struct fplinux_ui_screen *screen,
			const char *text)
{
	for (size_t i = 0; i < screen->count; i++)
		if (!strcmp(screen->rows[i].text, text))
			return i;
	assert(!"requested visible row is missing");
	return 0;
}

static void open_row(struct fplinux_ui *ui, const char *text)
{
	struct fplinux_ui_screen *screen = ui->stack[ui->depth - 1];

	screen->focused = row_named(screen, text);
	assert(screen->command);
	screen->command(ui, screen, FPLINUX_UI_SELECT_COMMAND, screen->context);
}

static void command(struct fplinux_ui *ui, enum fplinux_ui_command_type type)
{
	struct fplinux_ui_screen *screen = ui->stack[ui->depth - 1];

	assert(screen->command);
	for (size_t i = 0; i < screen->command_count; i++) {
		if (screen->commands[i].type == type) {
			bool alert = screen->message[0];

			if (alert)
				--ui->depth;
			screen->command(ui, screen, screen->commands[i].id,
					screen->context);
			if (alert)
				free(screen);
			return;
		}
	}
	assert(!"requested visible command is missing");
}

static void select_row(struct fplinux_ui *ui, const char *text)
{
	struct fplinux_ui_screen *screen = ui->stack[ui->depth - 1];
	size_t index = row_named(screen, text);

	assert(screen->change);
	screen->change(ui, screen, index, screen->context);
}

static void choose_row(struct fplinux_ui *ui, const char *text)
{
	size_t sent = ui->sent_count;

	select_row(ui, text);
	if (!strcmp(ui->stack[ui->depth - 1]->message,
		    "Changing this setting closes other apps. Unsaved changes may be lost.")) {
		assert(ui->sent_count == sent);
		command(ui, FPLINUX_UI_COMMAND_SCREEN);
	}
}

static void deliver_reply(struct fplinux_ui *ui,
			  const struct fplinux_ui_session_message *message)
{
	struct fplinux_ui_event event = {
		.type = FPLINUX_UI_EVENT_SESSION,
		.session = message,
	};

	if (message->applied) {
		if (!strcmp(message->name, "input_locale"))
			copy_text(ui->input_locale, sizeof(ui->input_locale),
				  message->value);
		else if (!strcmp(message->name, "locale"))
			copy_text(ui->locale, sizeof(ui->locale),
				  message->value);
		else if (!strcmp(message->name, "theme"))
			ui->theme.kind = !strcmp(message->value, "dark") ?
						 FPLINUX_UI_THEME_DARK :
						 FPLINUX_UI_THEME_LIGHT;
	}
	ui->event(ui, &event, ui->context);
}

static void reply(struct fplinux_ui *ui,
		  enum fplinux_ui_session_message_type type, const char *name,
		  const char *value, const char *reason)
{
	struct fplinux_ui_session_message message = {
		.type = type,
		.applied = type == FPLINUX_UI_SESSION_SETTING ||
			   !strcmp(reason, "save-failed"),
		.storage_error = !strcmp(reason, "save-failed") ? EIO : 0,
	};

	copy_text(message.name, sizeof(message.name), name);
	copy_text(message.value, sizeof(message.value), value);
	copy_text(message.reason, sizeof(message.reason), reason);
	deliver_reply(ui, &message);
}

static void set_lifecycle(struct fplinux_ui *ui,
			  enum fplinux_ui_lifecycle lifecycle)
{
	struct fplinux_ui_event event = { .type = FPLINUX_UI_EVENT_LIFECYCLE };

	if (ui->lifecycle == lifecycle)
		return;
	ui->lifecycle = lifecycle;
	ui->event(ui, &event, ui->context);
}

static void set_visible(struct fplinux_ui *ui, bool visible)
{
	struct fplinux_ui_event event = {
		.type = FPLINUX_UI_EVENT_VISIBILITY,
		.visible = visible,
	};

	if (ui->visible == visible)
		return;
	ui->visible = visible;
	ui->event(ui, &event, ui->context);
}

static void expect_sent(const struct fplinux_ui *ui, size_t index,
			const char *name, const char *value)
{
	assert(index < ui->sent_count);
	assert(ui->sent[index].type == FPLINUX_UI_SESSION_SETTING);
	assert(!strcmp(ui->sent[index].name, name));
	assert(!strcmp(ui->sent[index].value, value));
}

static bool has_sent(const struct fplinux_ui *ui, const char *name,
		     const char *value)
{
	for (size_t i = 0; i < ui->sent_count; i++)
		if (!strcmp(ui->sent[i].name, name) &&
		    !strcmp(ui->sent[i].value, value))
			return true;
	return false;
}

static const char *detail(struct fplinux_ui *ui, const char *title,
			  const char *label)
{
	struct fplinux_ui_screen *screen = current(ui, title);

	return screen->rows[row_named(screen, label)].detail;
}

static void open_brightness(struct fplinux_ui *ui)
{
	struct fplinux_ui_screen *form;

	reply(ui, FPLINUX_UI_SESSION_SETTING, "brightness", "4", "");
	open_row(ui, "Display");
	open_row(ui, "Brightness");
	form = current(ui, "Brightness");
	assert(form->rows[row_named(form, "Level")].value == 4);
}

static void preview(struct fplinux_ui *ui, int value)
{
	struct fplinux_ui_screen *form = current(ui, "Brightness");
	size_t item = row_named(form, "Level");

	form->rows[item].value = value;
	assert(form->change);
	form->change(ui, form, item, form->context);
}

static void close_notice(struct fplinux_ui *ui)
{
	struct fplinux_ui_screen *alert = ui->stack[--ui->depth];

	assert(alert->message[0] && alert->command_count == 1);
	assert(!strcmp(alert->commands[0].label, "Close"));
	/* The real alert leaves the stack before invoking its listener. */
	alert->command(ui, alert, alert->commands[0].id, alert->context);
	free(alert);
}

static void test_errors(struct fplinux_ui *ui)
{
	static const struct {
		const char *name;
		const char *value;
		const char *reason;
		const char *title;
		const char *text;
	} cases[] = {
		{ "screen_off_seconds", "30", "unavailable", "Screen timeout",
		  "This setting is unavailable now. Try again later." },
		{ "auto_lock_seconds", "5", "failed", "Auto-lock",
		  "The setting was not changed. Try again." },
		{ "locale", "ru", "failed", "Language",
		  "The setting was not changed. Try again." },
		{ "input_locale", "en", "failed", "Input language",
		  "The setting was not changed. Try again." },
		{ "theme", "dark", "save-failed", "Theme",
		  "The setting is applied for this session. It was not saved.\nStorage input/output error" },
		{ "brightness", "7", "failed", "Brightness",
		  "The setting was not changed. Try again." },
		{ "time", "2026-10-02 12:00:00", "failed", "Date and time",
		  "Check the date and time. Try again." },
	};

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		size_t sent = ui->sent_count;
		struct fplinux_ui_screen *alert;

		reply(ui, FPLINUX_UI_SESSION_ERROR, cases[i].name,
		      cases[i].value, cases[i].reason);
		alert = current(ui, cases[i].title);
		assert(!strcmp(alert->message, cases[i].text));
		if (strcmp(cases[i].name, "time") &&
		    strcmp(cases[i].reason, "save-failed")) {
			assert(ui->sent_count == sent + 1);
			expect_sent(ui, sent, cases[i].name, "");
		} else {
			assert(ui->sent_count == sent);
		}
		close_notice(ui);
	}
}

static void test_notice_survives_reply(struct fplinux_ui *ui, bool theme)
{
	const char *name = theme ? "theme" : "locale";
	const char *value = theme ? "dark" : "fr";
	const char *title = theme ? "Theme" : "Language";

	open_row(ui, theme ? "Display" : "Time & language");
	open_row(ui, title);
	choose_row(ui, theme ? "Dark" : "Français");
	assert(!current(ui, title)->message[0]);
	assert(ui->depth == 3);
	reply(ui, FPLINUX_UI_SESSION_ERROR, name, value, "save-failed");
	assert(ui->depth == 3);
	assert(!strcmp(ui->stack[1]->title,
		       theme ? "Display" : "Time & language"));
	assert(!strcmp(ui->stack[1]->rows[theme ? 3 : 1].detail,
		       theme ? "Dark" : "Français"));
	assert(!strcmp(
		current(ui, title)->message,
		"The setting is applied for this session. It was not saved.\nStorage input/output error"));
	reply(ui, FPLINUX_UI_SESSION_SETTING, name, value, "");
	assert(!strcmp(
		current(ui, title)->message,
		"The setting is applied for this session. It was not saved.\nStorage input/output error"));
	close_notice(ui);
	current(ui, theme ? "Display" : "Time & language");
	assert(ui->depth == 2);
	/* A later successful update must not recreate the old notice. */
	reply(ui, FPLINUX_UI_SESSION_SETTING, name, value, "");
	current(ui, theme ? "Display" : "Time & language");
}

enum setting_outcome {
	SETTING_APPLIED,
	SETTING_FAILED,
	SETTING_SAVE_FAILED,
};

static void test_choice_reply(struct fplinux_ui *ui, bool theme,
			      enum setting_outcome outcome, bool queued_back)
{
	const char *group = theme ? "Display" : "Time & language";
	const char *name = theme ? "theme" : "locale";
	const char *title = theme ? "Theme" : "Language";
	const char *before = theme ? "Light" : "English";
	const char *after = theme ? "Dark" : "Français";
	const char *old_value = theme ? "light" : "en";
	const char *new_value = theme ? "dark" : "fr";
	struct fplinux_ui_screen *selector;
	struct fplinux_ui_screen *list;
	size_t sent = ui->sent_count;
	bool failed = outcome == SETTING_FAILED;

	open_row(ui, group);
	open_row(ui, title);
	choose_row(ui, before);
	current(ui, group);
	assert(ui->sent_count == sent);
	open_row(ui, title);
	command(ui, FPLINUX_UI_COMMAND_BACK);
	current(ui, group);
	assert(ui->sent_count == sent);
	open_row(ui, title);
	selector = current(ui, title);
	choose_row(ui, after);
	assert(current(ui, title) == selector && ui->depth == 3);
	assert(ui->sent_count == sent + 1);
	expect_sent(ui, sent, name, new_value);
	choose_row(ui, after);
	assert(current(ui, title) == selector && ui->sent_count == sent + 1);
	if (queued_back) {
		/* Input may drain Select and both Back keys before a reply. */
		command(ui, FPLINUX_UI_COMMAND_BACK);
		command(ui, FPLINUX_UI_COMMAND_BACK);
		assert(current(ui, title) == selector && ui->depth == 3);
		assert(ui->sent_count == sent + 1);
	}
	if (outcome != SETTING_APPLIED) {
		const char *notice =
			failed ?
				"The setting was not changed. Try again." :
				"The setting is applied for this session. It was not saved.\nStorage input/output error";

		reply(ui, FPLINUX_UI_SESSION_ERROR, name, new_value,
		      failed ? "failed" : "save-failed");
		assert(!strcmp(current(ui, title)->message, notice));
		if (failed)
			expect_sent(ui, sent + 1, name, "");
		else
			assert(ui->sent_count == sent + 1);
		/* An applied value and the query reply both leave the notice. */
		reply(ui, FPLINUX_UI_SESSION_SETTING, name,
		      failed ? old_value : new_value, "");
		assert(!strcmp(current(ui, title)->message, notice));
		if (!failed) {
			reply(ui, FPLINUX_UI_SESSION_SETTING, name, new_value,
			      "");
			assert(!strcmp(current(ui, title)->message, notice));
		}
		close_notice(ui);
	} else {
		reply(ui, FPLINUX_UI_SESSION_SETTING, name, new_value, "");
	}
	list = current(ui, group);
	assert(ui->depth == 2 && list->focused == row_named(list, title));
	assert(!strcmp(detail(ui, group, title), failed ? before : after));
	open_row(ui, title);
	list = current(ui, title);
	assert(list->rows[row_named(list, failed ? before : after)].selected);
	assert(!list->rows[row_named(list, failed ? after : before)].selected);
	assert(list->focused == row_named(list, failed ? before : after));
	if (queued_back) {
		command(ui, FPLINUX_UI_COMMAND_BACK);
		current(ui, group);
		command(ui, FPLINUX_UI_COMMAND_BACK);
		current(ui, "Settings");
		assert(ui->depth == 1);
	}
}

static void test_choice_home_before_request(struct fplinux_ui *ui, bool theme)
{
	const char *group = theme ? "Display" : "Time & language";
	const char *title = theme ? "Theme" : "Language";
	const char *after = theme ? "Dark" : "Français";
	const char *name = theme ? "theme" : "locale";
	const char *value = theme ? "dark" : "fr";
	struct fplinux_ui_screen *selector;
	size_t sent;
	unsigned int alerts;

	session_boundary->flush(session_boundary->context, ui);
	open_row(ui, group);
	open_row(ui, title);
	selector = current(ui, title);
	choose_row(ui, after);
	alerts = ui->alerts;
	sent = ui->sent_count;
	expect_sent(ui, sent - 1, name, value);
	command(ui, FPLINUX_UI_COMMAND_BACK);
	assert(current(ui, title) == selector);
	/* Home travels separately and reaches the manager before this outbox. */
	session_boundary->home(session_boundary->context);
	session_boundary->flush(session_boundary->context, ui);
	assert(ui->lifecycle == FPLINUX_UI_BACKGROUND && !ui->visible);
	assert(ui->alerts == alerts + 1);
	assert(!strcmp(current(ui, title)->message,
		       "The setting was not changed. Try again."));
	close_notice(ui);
	/* A rejected background query must not leave this selector waiting. */
	session_boundary->resume(session_boundary->context);
	session_boundary->flush(session_boundary->context, ui);
	assert(ui->lifecycle == FPLINUX_UI_FOREGROUND && ui->visible);
	if (ui->depth == 3)
		command(ui, FPLINUX_UI_COMMAND_BACK);
	current(ui, group);
	assert(ui->depth == 2);
	assert(!strcmp(detail(ui, group, title), theme ? "Light" : "English"));
	open_row(ui, title);
	sent = ui->sent_count;
	choose_row(ui, after);
	assert(ui->sent_count == sent + 1);
	expect_sent(ui, sent, name, value);
	/* The second selection retains the visual barrier until its reply. */
	selector = current(ui, title);
	command(ui, FPLINUX_UI_COMMAND_BACK);
	assert(current(ui, title) == selector && ui->depth == 3);
}

static void test_change_warning_can_cancel(struct fplinux_ui *ui, bool theme)
{
	const char *title = theme ? "Theme" : "Language";
	const char *before = theme ? "Light" : "English";
	size_t sent = ui->sent_count;

	open_row(ui, theme ? "Display" : "Time & language");
	open_row(ui, title);
	select_row(ui, theme ? "Dark" : "Français");
	assert(ui->sent_count == sent);
	assert(!strcmp(
		current(ui, title)->message,
		"Changing this setting closes other apps. Unsaved changes may be lost."));
	command(ui, FPLINUX_UI_COMMAND_CANCEL);
	struct fplinux_ui_screen *selector = current(ui, title);

	assert(ui->depth == 3 &&
	       selector->focused == row_named(selector, before));
	assert(selector->rows[row_named(selector, before)].selected);
	assert(ui->sent_count == sent);
}

static void
test_pending_choice_recovers_after_visibility_only(struct fplinux_ui *ui,
						   bool theme)
{
	const char *group = theme ? "Display" : "Time & language";
	const char *title = theme ? "Theme" : "Language";
	const char *name = theme ? "theme" : "locale";
	const char *before = theme ? "light" : "en";
	const char *after = theme ? "dark" : "fr";

	open_row(ui, group);
	open_row(ui, title);
	choose_row(ui, theme ? "Dark" : "Français");
	size_t sent = ui->sent_count;

	set_visible(ui, false);
	reply(ui, FPLINUX_UI_SESSION_ERROR, name, after, "not-foreground");
	assert(ui->lifecycle == FPLINUX_UI_FOREGROUND);
	assert(ui->sent_count == sent);
	close_notice(ui);
	command(ui, FPLINUX_UI_COMMAND_BACK);
	assert(ui->depth == 3);
	set_visible(ui, true);
	assert(ui->sent_count == sent + 1);
	expect_sent(ui, sent, name, "");
	reply(ui, FPLINUX_UI_SESSION_SETTING, name, before, "");
	current(ui, group);
	assert(ui->depth == 2);
}

static void test_brightness_save_retries_current_value(struct fplinux_ui *ui)
{
	open_brightness(ui);
	preview(ui, 8);
	command(ui, FPLINUX_UI_COMMAND_OK);
	reply(ui, FPLINUX_UI_SESSION_ERROR, "brightness", "8", "save-failed");
	assert(!strcmp(ui->stack[1]->rows[0].detail, "8"));
	close_notice(ui);
	open_row(ui, "Brightness");
	size_t sent = ui->sent_count;

	command(ui, FPLINUX_UI_COMMAND_OK);
	assert(ui->sent_count == sent + 1);
	expect_sent(ui, sent, "brightness", "8");
}

static void
test_save_failure_preserves_state_and_cause(struct fplinux_ui *ui,
					    enum fplinux_storage_state state,
					    int error, const char *expected)
{
	struct fplinux_ui_session_message message = {
		.type = FPLINUX_UI_SESSION_ERROR,
		.applied = true,
		.storage_state = state,
		.storage_error = error,
	};

	open_row(ui, "Display");
	open_row(ui, "Theme");
	choose_row(ui, "Dark");
	size_t sent = ui->sent_count;

	copy_text(message.name, sizeof(message.name), "theme");
	copy_text(message.value, sizeof(message.value), "dark");
	copy_text(message.reason, sizeof(message.reason), "save-failed");
	deliver_reply(ui, &message);
	assert(ui->sent_count == sent);
	assert(!strcmp(current(ui, "Theme")->message, expected));
	assert(!strcmp(ui->stack[1]->rows[3].detail, "Dark"));
	/* A later value reply can rebuild the view without acknowledging this error. */
	reply(ui, FPLINUX_UI_SESSION_SETTING, "locale", "en", "");
	assert(!strcmp(ui->stack[1]->title, "Time & language"));
	assert(!strcmp(current(ui, "Theme")->message, expected));
	close_notice(ui);
	current(ui, "Time & language");
}

static void test_session_save_failure_rebuilds_before_ack(struct fplinux_ui *ui)
{
	session_boundary->flush(session_boundary->context, ui);
	open_row(ui, "Display");
	open_row(ui, "Theme");
	choose_row(ui, "Dark");
	size_t sent = ui->sent_count;

	session_boundary->flush(session_boundary->context, ui);
	assert(ui->theme.kind == FPLINUX_UI_THEME_DARK);
	assert(ui->sent_count == sent);
	assert(ui->depth == 3 && !strcmp(ui->stack[1]->title, "Display"));
	assert(!strcmp(ui->stack[1]->rows[3].detail, "Dark"));
	assert(!strcmp(
		current(ui, "Theme")->message,
		"The setting is applied for this session. It was not saved.\nStorage input/output error"));
}

static void test_session_preview_cancel_preserves_retry(struct fplinux_ui *ui)
{
	struct fplinux_ui_screen *form;
	size_t sent;

	session_boundary->flush(session_boundary->context, ui);
	open_row(ui, "Display");
	open_row(ui, "Screen timeout");
	select_row(ui, "1 min");
	session_boundary->flush(session_boundary->context, ui);
	current(ui, "Screen timeout");
	close_notice(ui);
	assert(!strcmp(detail(ui, "Display", "Screen timeout"), "1 min"));
	open_row(ui, "Brightness");
	form = current(ui, "Brightness");
	assert(form->rows[row_named(form, "Level")].value == 7);
	preview(ui, 8);
	session_boundary->flush(session_boundary->context, ui);
	command(ui, FPLINUX_UI_COMMAND_CANCEL);
	session_boundary->flush(session_boundary->context, ui);
	assert(!strcmp(detail(ui, "Display", "Brightness"), "7"));
	session_boundary->recover_storage(session_boundary->context);
	open_row(ui, "Screen timeout");
	sent = ui->sent_count;
	select_row(ui, "1 min");
	assert(ui->sent_count == sent + 1);
	expect_sent(ui, sent, "screen_off_seconds", "60");
	session_boundary->flush(session_boundary->context, ui);
	open_row(ui, "Screen timeout");
	sent = ui->sent_count;
	select_row(ui, "1 min");
	assert(ui->sent_count == sent);
	current(ui, "Display");
}

bool settings_session_next_request(struct fplinux_ui *ui, size_t *index,
				   struct fplinux_ui_session_message *message)
{
	if (*index == ui->sent_count)
		return false;
	assert(*index < ui->sent_count);
	*message = ui->sent[(*index)++];
	return true;
}

void settings_session_receive(struct fplinux_ui *ui,
			      const struct fplinux_ui_session_message *message)
{
	if (message->type == FPLINUX_UI_SESSION_LIFECYCLE) {
		enum fplinux_ui_lifecycle lifecycle =
			message->lifecycle == FPLINUX_UI_SESSION_FOREGROUND ?
				FPLINUX_UI_FOREGROUND :
				FPLINUX_UI_BACKGROUND;

		set_lifecycle(ui, lifecycle);
	} else if (message->type == FPLINUX_UI_SESSION_VISIBILITY) {
		set_visible(ui, message->visible);
	} else if (message->type == FPLINUX_UI_SESSION_SETTING ||
		   message->type == FPLINUX_UI_SESSION_ERROR) {
		deliver_reply(ui, message);
	}
}

static void test_preview_failure(struct fplinux_ui *ui)
{
	struct fplinux_ui_screen *form;
	size_t sent;

	open_brightness(ui);
	form = current(ui, "Brightness");
	preview(ui, 7);
	sent = ui->sent_count;
	expect_sent(ui, sent - 1, "brightness_preview", "7");
	reply(ui, FPLINUX_UI_SESSION_ERROR, "brightness_preview", "7",
	      "unavailable");
	assert(form->rows[row_named(form, "Level")].value == 4);
	assert(!strcmp(current(ui, "Brightness")->message,
		       "This setting is unavailable now. Try again later."));
	assert(ui->sent_count == sent + 1);
	expect_sent(ui, sent, "brightness_preview", "");
	assert(ui->alerts == 1);
	/* Replies for already queued preview steps do not add more notices. */
	reply(ui, FPLINUX_UI_SESSION_ERROR, "brightness_preview", "8",
	      "unavailable");
	reply(ui, FPLINUX_UI_SESSION_ERROR, "brightness_preview", "",
	      "unavailable");
	assert(ui->alerts == 1 && ui->sent_count == sent + 1);
	close_notice(ui);
	command(ui, FPLINUX_UI_COMMAND_CANCEL);
	assert(ui->sent_count == sent + 1);
	assert(!strcmp(detail(ui, "Display", "Brightness"), "4"));
}

static void test_brightness_finish(struct fplinux_ui *ui, bool save)
{
	size_t sent;

	open_brightness(ui);
	preview(ui, 8);
	sent = ui->sent_count;
	expect_sent(ui, sent - 1, "brightness_preview", "8");
	command(ui, save ? FPLINUX_UI_COMMAND_OK : FPLINUX_UI_COMMAND_CANCEL);
	assert(ui->sent_count == sent + 1);
	expect_sent(ui, sent, save ? "brightness" : "brightness_preview",
		    save ? "8" : "");
	current(ui, "Display");
	/* The stored level changes only with a manager reply. */
	assert(!strcmp(detail(ui, "Display", "Brightness"), "4"));
	if (save) {
		reply(ui, FPLINUX_UI_SESSION_SETTING, "brightness", "8", "");
		assert(!strcmp(detail(ui, "Display", "Brightness"), "8"));
	}
}

static void test_brightness_background(struct fplinux_ui *ui, bool save)
{
	struct fplinux_ui_screen *form;
	size_t sent;

	open_brightness(ui);
	form = current(ui, "Brightness");
	preview(ui, 8);
	expect_sent(ui, ui->sent_count - 1, "brightness_preview", "8");
	set_lifecycle(ui, FPLINUX_UI_BACKGROUND);
	assert(form->rows[row_named(form, "Level")].value == 4);
	/* An acknowledgement already in flight cannot revive the preview. */
	reply(ui, FPLINUX_UI_SESSION_SETTING, "brightness_preview", "8", "");
	set_visible(ui, false);
	set_lifecycle(ui, FPLINUX_UI_FOREGROUND);
	set_visible(ui, true);
	assert(current(ui, "Brightness") == form && ui->depth == 3);
	assert(form->rows[row_named(form, "Level")].value == 4);
	sent = ui->sent_count;
	command(ui, save ? FPLINUX_UI_COMMAND_OK : FPLINUX_UI_COMMAND_CANCEL);
	assert(ui->sent_count == sent + (save ? 1 : 0));
	if (save)
		expect_sent(ui, sent, "brightness", "4");
	assert(!strcmp(detail(ui, "Display", "Brightness"), "4"));
}

static void test_brightness_hidden(struct fplinux_ui *ui)
{
	struct fplinux_ui_screen *form;
	size_t sent;

	open_brightness(ui);
	form = current(ui, "Brightness");
	preview(ui, 8);
	sent = ui->sent_count;
	/* A temporary overlay or lock hides the foreground task. */
	set_visible(ui, false);
	assert(fplinux_ui_lifecycle(ui) == FPLINUX_UI_FOREGROUND);
	reply(ui, FPLINUX_UI_SESSION_SETTING, "brightness_preview", "8", "");
	set_visible(ui, true);
	assert(current(ui, "Brightness") == form && ui->depth == 3);
	assert(form->rows[row_named(form, "Level")].value == 8);
	assert(ui->sent_count == sent);
	command(ui, FPLINUX_UI_COMMAND_OK);
	assert(ui->sent_count == sent + 1);
	expect_sent(ui, sent, "brightness", "8");
}

static void test_input_language(struct fplinux_ui *ui)
{
	struct fplinux_ui_screen *list;
	size_t sent = ui->sent_count;

	assert(has_sent(ui, "input_locale", ""));
	open_row(ui, "Input");
	assert(!strcmp(detail(ui, "Input", "Input language"), "Русский"));
	open_row(ui, "Input language");
	list = current(ui, "Input language");
	/* System-only, unavailable and unusable-map definitions are omitted. */
	assert(list->count == 2);
	assert(!strcmp(list->rows[0].text, "English"));
	assert(!strcmp(list->rows[1].text, "Русский"));
	assert(!list->rows[0].selected && list->rows[1].selected);
	assert(list->focused == 1);
	choose_row(ui, "Русский");
	current(ui, "Input");
	assert(ui->sent_count == sent);
	open_row(ui, "Input language");
	choose_row(ui, "English");
	assert(ui->sent_count == sent + 1);
	expect_sent(ui, sent, "input_locale", "en");
	assert(!strcmp(fplinux_ui_locale(ui), "en"));
	assert(!strcmp(fplinux_ui_input_locale(ui), "ru"));
	reply(ui, FPLINUX_UI_SESSION_ERROR, "input_locale", "en", "failed");
	assert(!strcmp(current(ui, "Input language")->message,
		       "The setting was not changed. Try again."));
	expect_sent(ui, sent + 1, "input_locale", "");
	close_notice(ui);
	reply(ui, FPLINUX_UI_SESSION_ERROR, "input_locale", "", "unavailable");
	assert(!strcmp(detail(ui, "Input", "Input language"), "Unavailable"));
	reply(ui, FPLINUX_UI_SESSION_SETTING, "input_locale", "en", "");
	assert(!strcmp(detail(ui, "Input", "Input language"), "English"));
	open_row(ui, "Input language");
	list = current(ui, "Input language");
	assert(list->count == 2);
	assert(!strcmp(list->rows[0].text, "English"));
	assert(!strcmp(list->rows[1].text, "Русский"));
	assert(list->rows[0].selected && !list->rows[1].selected);
	assert(list->focused == 0);
	command(ui, FPLINUX_UI_COMMAND_BACK);
	command(ui, FPLINUX_UI_COMMAND_BACK);
	open_row(ui, "Time & language");
	open_row(ui, "Language");
	list = current(ui, "Language");
	assert(list->count == 2);
	assert(!strcmp(list->rows[0].text, "English"));
	assert(!strcmp(list->rows[1].text, "Français"));
}

static void test_about(struct fplinux_ui *ui)
{
	struct fplinux_ui_screen *root = current(ui, "Settings");
	struct fplinux_ui_screen *form;
	size_t sent = ui->sent_count;
	size_t about = row_named(root, "About");
	bool missing_logo = !strcmp(scenario, "about-missing-logo");

	open_row(ui, "About");
	form = current(ui, "About");
	assert(ui->depth == 2 && form->is_form && !form->change);
	assert(form->image_attempts == 1);
	assert(!strcmp(form->image_source, expected_logo));
	assert(form->rows[0].image == !missing_logo);
	assert(form->rows[row_named(form, "System")].detail[0]);
	assert(form->rows[row_named(form, "Kernel")].detail[0]);
	assert(form->rows[row_named(form, "Free memory")].detail[0]);
	for (size_t i = 0; i < form->count; i++)
		if (!form->rows[i].image)
			printf("%s: %s\n", form->rows[i].text,
			       form->rows[i].detail);
	command(ui, FPLINUX_UI_COMMAND_BACK);
	assert(current(ui, "Settings") == root && ui->depth == 1);
	assert(root->focused == about && ui->sent_count == sent);
}

struct fplinux_ui *fplinux_ui_open(const char *app_id,
				   fplinux_ui_event_cb callback, void *context)
{
	struct fplinux_ui *ui = calloc(1, sizeof(*ui));

	assert(!strcmp(app_id, "org.fplinux.settings"));
	resources_opened = true;
	assert(ui);
	ui->event = callback;
	ui->context = context;
	copy_text(ui->locale, sizeof(ui->locale), "en");
	copy_text(ui->input_locale, sizeof(ui->input_locale), "ru");
	ui->theme.kind = dark_theme ? FPLINUX_UI_THEME_DARK :
				      FPLINUX_UI_THEME_LIGHT;
	ui->metrics.profile = selected_profile;
	ui->visible = true;
	ui->lifecycle = FPLINUX_UI_FOREGROUND;
	return ui;
}

int fplinux_ui_run(struct fplinux_ui *ui)
{
	struct fplinux_ui_event close = { .type = FPLINUX_UI_EVENT_CLOSE };

	if (session_boundary && !strcmp(scenario, "save-failed-session"))
		test_session_save_failure_rebuilds_before_ack(ui);
	else if (session_boundary && !strcmp(scenario, "preview-cancel-retry"))
		test_session_preview_cancel_preserves_retry(ui);
	else if (session_boundary)
		test_choice_home_before_request(ui, !strcmp(scenario, "theme"));
	else if (!strcmp(scenario, "about") ||
		 !strcmp(scenario, "about-missing-logo"))
		test_about(ui);
	else if (!strcmp(scenario, "errors"))
		test_errors(ui);
	else if (!strcmp(scenario, "preview-failure"))
		test_preview_failure(ui);
	else if (!strcmp(scenario, "cancel"))
		test_brightness_finish(ui, false);
	else if (!strcmp(scenario, "save"))
		test_brightness_finish(ui, true);
	else if (!strcmp(scenario, "preview-background-save"))
		test_brightness_background(ui, true);
	else if (!strcmp(scenario, "preview-background-cancel"))
		test_brightness_background(ui, false);
	else if (!strcmp(scenario, "preview-hidden"))
		test_brightness_hidden(ui);
	else if (!strcmp(scenario, "input"))
		test_input_language(ui);
	else if (!strcmp(scenario, "brightness-retry-current"))
		test_brightness_save_retries_current_value(ui);
	else if (!strcmp(scenario, "save-failed-no-space"))
		test_save_failure_preserves_state_and_cause(
			ui, FPLINUX_STORAGE_UNPUBLISHED, ENOSPC,
			"The setting is applied for this session. It was not saved.\nNot enough storage space");
	else if (!strcmp(scenario, "save-failed-read-only"))
		test_save_failure_preserves_state_and_cause(
			ui, FPLINUX_STORAGE_UNPUBLISHED, EROFS,
			"The setting is applied for this session. It was not saved.\nStorage is read-only");
	else if (!strcmp(scenario, "save-failed-published"))
		test_save_failure_preserves_state_and_cause(
			ui, FPLINUX_STORAGE_PUBLISHED_UNSYNCED, EIO,
			"The setting is applied. Saving is not confirmed.\nStorage input/output error");
	else if (!strcmp(scenario, "theme-warning-cancel"))
		test_change_warning_can_cancel(ui, true);
	else if (!strcmp(scenario, "locale-warning-cancel"))
		test_change_warning_can_cancel(ui, false);
	else if (!strcmp(scenario, "theme-visibility-recovery"))
		test_pending_choice_recovers_after_visibility_only(ui, true);
	else if (!strcmp(scenario, "locale-visibility-recovery"))
		test_pending_choice_recovers_after_visibility_only(ui, false);
	else if (!strcmp(scenario, "theme-save-failed"))
		test_notice_survives_reply(ui, true);
	else if (!strcmp(scenario, "locale-save-failed"))
		test_notice_survives_reply(ui, false);
	else if (!strcmp(scenario, "theme-applied"))
		test_choice_reply(ui, true, SETTING_APPLIED, false);
	else if (!strcmp(scenario, "locale-applied"))
		test_choice_reply(ui, false, SETTING_APPLIED, false);
	else if (!strcmp(scenario, "theme-failed"))
		test_choice_reply(ui, true, SETTING_FAILED, false);
	else if (!strcmp(scenario, "locale-failed"))
		test_choice_reply(ui, false, SETTING_FAILED, false);
	else if (!strcmp(scenario, "theme-pending-back-applied"))
		test_choice_reply(ui, true, SETTING_APPLIED, true);
	else if (!strcmp(scenario, "locale-pending-back-applied"))
		test_choice_reply(ui, false, SETTING_APPLIED, true);
	else if (!strcmp(scenario, "theme-pending-back-failed"))
		test_choice_reply(ui, true, SETTING_FAILED, true);
	else if (!strcmp(scenario, "locale-pending-back-failed"))
		test_choice_reply(ui, false, SETTING_FAILED, true);
	else if (!strcmp(scenario, "theme-pending-back-save-failed"))
		test_choice_reply(ui, true, SETTING_SAVE_FAILED, true);
	else if (!strcmp(scenario, "locale-pending-back-save-failed"))
		test_choice_reply(ui, false, SETTING_SAVE_FAILED, true);
	else
		assert(!"unknown scenario");
	ui->event(ui, &close, ui->context);
	return 0;
}

int settings_session_save_failure_test(
	const struct settings_session_boundary *boundary)
{
	char *arguments[] = { "fplinux-settings", NULL };

	scenario = "save-failed-session";
	session_boundary = boundary;
	int result = fplinux_settings_main(1, arguments);

	session_boundary = NULL;
	return result;
}

int settings_session_preview_retry_test(
	const struct settings_session_boundary *boundary)
{
	char *arguments[] = { "fplinux-settings", NULL };
	int result;

	scenario = "preview-cancel-retry";
	session_boundary = boundary;
	result = fplinux_settings_main(1, arguments);
	session_boundary = NULL;
	return result;
}

int settings_session_test(bool theme,
			  const struct settings_session_boundary *boundary)
{
	char *arguments[] = { "fplinux-settings", NULL };
	int result;

	scenario = theme ? "theme" : "locale";
	session_boundary = boundary;
	result = fplinux_settings_main(1, arguments);
	session_boundary = NULL;
	return result;
}

void fplinux_ui_close(struct fplinux_ui *ui)
{
	while (ui->depth)
		free(ui->stack[--ui->depth]);
	free(ui);
}

const struct fplinux_ui_theme *fplinux_ui_theme(const struct fplinux_ui *ui)
{
	return &ui->theme;
}

/* The controlled UI boundary supplies the common error copy used by the app. */
const char *fplinux_ui_storage_error(struct fplinux_storage_result result)
{
	switch (result.error) {
	case ENOSPC:
		return "Not enough storage space";
	case EROFS:
		return "Storage is read-only";
	case EIO:
		return "Storage input/output error";
	default:
		assert(!"unexpected storage error at controlled UI boundary");
		return "";
	}
}

const struct fplinux_ui_metrics *fplinux_ui_metrics(const struct fplinux_ui *ui)
{
	return &ui->metrics;
}

const char *fplinux_ui_locale(const struct fplinux_ui *ui)
{
	return ui->locale;
}

const char *fplinux_ui_input_locale(const struct fplinux_ui *ui)
{
	return ui->input_locale;
}

bool fplinux_ui_is_visible(const struct fplinux_ui *ui)
{
	return ui->visible;
}

enum fplinux_ui_lifecycle fplinux_ui_lifecycle(const struct fplinux_ui *ui)
{
	return ui->lifecycle;
}

const void *fplinux_ui_glyph(struct fplinux_ui *ui, const char *name,
			     enum fplinux_ui_glyph_size size)
{
	(void)ui;
	assert(size == FPLINUX_UI_GLYPH_ROW);
	return name;
}

bool fplinux_ui_send(struct fplinux_ui *ui,
		     const struct fplinux_ui_session_message *message)
{
	assert(ui->sent_count < 64);
	ui->sent[ui->sent_count++] = *message;
	return true;
}

void fplinux_ui_push(struct fplinux_ui *ui, struct fplinux_ui_screen *screen)
{
	assert(ui->depth < 8);
	ui->stack[ui->depth++] = screen;
}

bool fplinux_ui_pop(struct fplinux_ui *ui)
{
	if (ui->depth <= 1)
		return false;
	free(ui->stack[--ui->depth]);
	return true;
}

void fplinux_ui_replace(struct fplinux_ui *ui, struct fplinux_ui_screen *screen)
{
	assert(ui->depth);
	free(ui->stack[ui->depth - 1]);
	ui->stack[ui->depth - 1] = screen;
}

void fplinux_ui_go_back(struct fplinux_ui *ui)
{
	(void)fplinux_ui_pop(ui);
}

struct fplinux_ui_screen *fplinux_ui_list_new(struct fplinux_ui *ui,
					      const char *title,
					      enum fplinux_ui_list_kind kind)
{
	(void)ui;
	(void)kind;
	return new_screen(title);
}

size_t fplinux_ui_list_append(struct fplinux_ui_screen *list, const char *text,
			      const char *detail)
{
	size_t index = list->count++;

	assert(index < 16);
	copy_text(list->rows[index].text, sizeof(list->rows[index].text), text);
	copy_text(list->rows[index].detail, sizeof(list->rows[index].detail),
		  detail);
	return index;
}

bool fplinux_ui_list_set(struct fplinux_ui_screen *list, size_t index,
			 const char *text, const char *detail)
{
	assert(index < list->count);
	copy_text(list->rows[index].text, sizeof(list->rows[index].text), text);
	copy_text(list->rows[index].detail, sizeof(list->rows[index].detail),
		  detail);
	return true;
}

void fplinux_ui_list_set_focused(struct fplinux_ui_screen *list, size_t index)
{
	assert(index < list->count);
	list->focused = index;
}

size_t fplinux_ui_list_get_focused(const struct fplinux_ui_screen *list)
{
	return list->focused;
}

void fplinux_ui_list_set_selected(struct fplinux_ui_screen *list, size_t index,
				  bool selected)
{
	assert(index < list->count);
	list->rows[index].selected = selected;
}

bool fplinux_ui_list_set_select_label(struct fplinux_ui_screen *list,
				      const char *label)
{
	(void)list;
	(void)label;
	return true;
}

bool fplinux_ui_list_set_icon(struct fplinux_ui_screen *list, size_t index,
			      const void *src)
{
	(void)list;
	(void)index;
	(void)src;
	return true;
}

bool fplinux_ui_screen_add_command(struct fplinux_ui_screen *screen,
				   const struct fplinux_ui_command *command)
{
	assert(screen->command_count < 4);
	if (command->label && !strcmp(command->label, "Save"))
		assert(command->icon && !strcmp(command->icon, "save"));
	if (command->label && !strcmp(command->label, "Change"))
		assert(command->icon && !strcmp(command->icon, "edit"));
	if (command->type == FPLINUX_UI_COMMAND_CANCEL)
		assert(command->icon && !strcmp(command->icon, "cancel"));
	screen->commands[screen->command_count++] = *command;
	return true;
}

void fplinux_ui_screen_set_listener(struct fplinux_ui_screen *screen,
				    fplinux_ui_command_cb command,
				    fplinux_ui_change_cb change, void *context)
{
	screen->command = command;
	screen->change = change;
	screen->context = context;
}

struct fplinux_ui_screen *fplinux_ui_form_new(struct fplinux_ui *ui,
					      const char *title)
{
	struct fplinux_ui_screen *form = new_screen(title);

	(void)ui;
	form->is_form = true;
	return form;
}

size_t fplinux_ui_form_append_image(struct fplinux_ui_screen *form,
				    const char *label, const void *src)
{
	size_t item;

	assert(form->is_form && src);
	form->image_attempts++;
	copy_text(form->image_source, sizeof(form->image_source), src);
	if (!strcmp(scenario, "about-missing-logo")) {
		errno = ENOENT;
		return SIZE_MAX;
	}
	item = fplinux_ui_list_append(form, label, NULL);
	form->rows[item].image = true;
	return item;
}

size_t fplinux_ui_form_append_gauge(struct fplinux_ui_screen *form,
				    const char *label, int minimum, int maximum,
				    int value, bool interactive)
{
	size_t index = fplinux_ui_list_append(form, label, NULL);

	assert(minimum == 1 && maximum == 10 && interactive);
	form->rows[index].value = value;
	return index;
}

size_t fplinux_ui_form_append_string(struct fplinux_ui_screen *form,
				     const char *label, const char *text)
{
	return fplinux_ui_list_append(form, label, text);
}

int fplinux_ui_form_get_value(const struct fplinux_ui_screen *form, size_t item)
{
	assert(item < form->count);
	return form->rows[item].value;
}

void fplinux_ui_form_set_value(struct fplinux_ui_screen *form, size_t item,
			       int value)
{
	assert(item < form->count);
	form->rows[item].value = value;
}

size_t fplinux_ui_form_append_date(struct fplinux_ui_screen *form,
				   const char *label,
				   enum fplinux_ui_date_mode mode, time_t value)
{
	size_t item = fplinux_ui_list_append(form, label, NULL);

	(void)mode;
	form->rows[item].date = value;
	return item;
}

time_t fplinux_ui_form_get_date(const struct fplinux_ui_screen *form,
				size_t item)
{
	assert(item < form->count);
	return form->rows[item].date;
}

void fplinux_ui_form_set_date(struct fplinux_ui_screen *form, size_t item,
			      time_t value)
{
	assert(item < form->count);
	form->rows[item].date = value;
}

struct fplinux_ui_screen *fplinux_ui_alert_new(struct fplinux_ui *ui,
					       enum fplinux_ui_alert_kind kind,
					       const char *title,
					       const char *text,
					       unsigned int timeout_ms)
{
	struct fplinux_ui_screen *alert = new_screen(title);

	(void)kind;
	assert(timeout_ms == 0);
	ui->alerts++;
	copy_text(alert->message, sizeof(alert->message), text);
	return alert;
}

bool fplinux_ui_format_date(const struct fplinux_ui *ui, time_t when,
			    enum fplinux_ui_date_style style, char *out,
			    size_t size)
{
	(void)ui;
	(void)when;
	copy_text(out, size,
		  style == FPLINUX_UI_DATE_CLOCK ? "00:17" : "10/02/2026");
	return true;
}

bool fplinux_ui_format_number(const struct fplinux_ui *ui, double value,
			      int decimals, char *out, size_t size)
{
	int length;

	(void)ui;
	assert(decimals == 1);
	length = snprintf(out, size, "%.1f", value);
	return length >= 0 && (size_t)length < size;
}

/* The real multi-tap loader validates these controlled installed definitions. */
bool fplinux_language_load(const char *directory, const char *code,
			   struct fplinux_language *language)
{
	(void)directory;
	memset(language, 0, sizeof(*language));
	if (!strcmp(code, "xx"))
		return false;
	copy_text(language->code, sizeof(language->code), code);
	if (!strcmp(code, "en")) {
		copy_text(language->name, sizeof(language->name), "English");
		language->system = true;
		language->input = true;
	} else if (!strcmp(code, "ru")) {
		copy_text(language->name, sizeof(language->name), "Русский");
		language->input = true;
	} else if (!strcmp(code, "fr")) {
		copy_text(language->name, sizeof(language->name), "Français");
		language->system = true;
	} else if (!strcmp(code, "de")) {
		copy_text(language->name, sizeof(language->name), "Deutsch");
		language->input = true;
	} else {
		assert(!"unknown controlled language");
	}
	for (unsigned int key = 0; key < 10; key++)
		snprintf(language->multitap[key],
			 sizeof(language->multitap[key]), "%u", key);
	if (!strcmp(code, "de"))
		copy_text(language->multitap[2], sizeof(language->multitap[2]),
			  "abc");
	return true;
}

size_t fplinux_language_list(const char *directory,
			     char codes[][FPLINUX_LANGUAGE_CODE_BYTES],
			     size_t capacity)
{
	static const char *const installed[] = { "de", "en", "fr", "ru", "xx" };

	(void)directory;
	for (size_t i = 0; i < 5 && i < capacity; i++)
		copy_text(codes[i], FPLINUX_LANGUAGE_CODE_BYTES, installed[i]);
	return 5;
}

void fplinux_language_english(struct fplinux_language *language)
{
	assert(fplinux_language_load(NULL, "en", language));
}

lv_timer_t *lv_timer_create(lv_timer_cb_t callback, uint32_t period, void *data)
{
	lv_timer_t *timer = malloc(sizeof(*timer));

	(void)callback;
	(void)period;
	assert(timer);
	timer->data = data;
	return timer;
}

void *lv_timer_get_user_data(lv_timer_t *timer)
{
	return timer->data;
}

void lv_timer_delete(lv_timer_t *timer)
{
	free(timer);
}

void lv_timer_pause(lv_timer_t *timer)
{
	(void)timer;
}

void lv_timer_resume(lv_timer_t *timer)
{
	(void)timer;
}

void lv_timer_reset(lv_timer_t *timer)
{
	(void)timer;
}

void lv_timer_set_period(lv_timer_t *timer, uint32_t period)
{
	(void)timer;
	(void)period;
}

int main(int argc, char **argv)
{
	char *application_argv[] = { "fplinux-settings", NULL };
	int result;

	if (argc > 1 && !strcmp(argv[1], "cli")) {
		result = fplinux_settings_main(argc - 1, argv + 1);
		assert(!resources_opened);
		return result;
	}
	assert(argc >= 2);
	scenario = argv[1];
	if (!strcmp(scenario, "about") ||
	    !strcmp(scenario, "about-missing-logo")) {
		assert(argc == 5);
		selected_profile = atoi(argv[2]);
		assert(selected_profile >= 0 && selected_profile < 5);
		assert(!strcmp(argv[3], "light") || !strcmp(argv[3], "dark"));
		dark_theme = !strcmp(argv[3], "dark");
		expected_logo = argv[4];
	} else {
		assert(argc == 2);
	}
	assert(fplinux_settings_main(1, application_argv) == 0);
	printf("settings-app: %s passed\n", scenario);
	return 0;
}
