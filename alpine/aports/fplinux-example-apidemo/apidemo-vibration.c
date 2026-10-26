/* SPDX-License-Identifier: 0BSD */
/*
 * Vibration: patterns for the vibrator. The session manager plays them only
 * while the application is visible and stops the vibrator when it is hidden
 * or leaves. Returning never restarts a pattern without Play.
 */
#include "apidemo.h"

#include <fplinux/fplinux-ui-vibration.h>

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define TEXT_BYTES 256

enum { COMMAND_STOP = 1, COMMAND_PLAY };

/*
 * A pattern alternates pauses and pulses in milliseconds, a pause first:
 * {wait, on, off, on, ...}. A wait of 0 starts at once.
 */
static const unsigned int short_pulse[] = { 0, 100 };
static const unsigned int long_pulse[] = { 0, 1000 };
static const unsigned int two_pulses[] = { 0, 100, 150, 100 };
static const unsigned int heartbeat[] = { 0, 80, 120, 160, 600, 80, 120, 160 };
/* Morse with a 150 ms unit: a dot is 1 unit and a dash 3, signals are 1
 * unit apart and letters 3. Each row starts with the pause before its
 * letter. */
static const unsigned int sos[] = {
	0,   150, 150, 150, 150, 150, /* S */
	450, 450, 150, 450, 150, 450, /* O */
	450, 150, 150, 150, 150, 150, /* S */
};
/* With repeat, the last pause and the wait form the pause before the first
 * pulse comes again: 1.5 s on and 0.5 s off until stopped. */
static const unsigned int on_off[] = { 0, 1500, 500 };
struct request {
	/* N_() text of the choice. */
	const char *name;
	const unsigned int *pattern_ms;
	size_t count;
	bool repeat;
};

#define REQUEST(name, pattern, repeat) \
	{ name, pattern, APIDEMO_COUNT(pattern), repeat }

static const struct request patterns[] = {
	REQUEST(N_("100 ms pulse"), short_pulse, false),
	REQUEST(N_("1 s pulse"), long_pulse, false),
	REQUEST(N_("Two pulses"), two_pulses, false),
	REQUEST(N_("Heartbeat"), heartbeat, false),
	REQUEST(N_("SOS"), sos, false),
	REQUEST(N_("Repeating"), on_off, true),
};

/* The pattern form owns its request and result until Back or cleanup. */
static struct {
	struct fplinux_ui_screen *form;
	size_t choice;
	size_t result;
	size_t pattern;
	/* A pattern was accepted since the last stop: Back, CLOSE and cleanup
	 * stop the vibrator. */
	bool started;
} vibration_demo;

static void append(char *text, size_t size, const char *format, ...)
	__attribute__((format(printf, 3, 4)));

static void append(char *text, size_t size, const char *format, ...)
{
	size_t used = strlen(text);
	va_list arguments;

	if (used + 1 >= size)
		return;
	va_start(arguments, format);
	vsnprintf(text + used, size - used, format, arguments);
	va_end(arguments);
}

/* The arguments as C: "{0, 1500, 500}, repeat". */
static void show_pattern(const struct request *request)
{
	char text[TEXT_BYTES] = "{";

	for (size_t i = 0; i < request->count; i++) {
		append(text, sizeof(text), "%s%u", i ? ", " : "",
		       request->pattern_ms[i]);
	}
	append(text, sizeof(text), "}%s", request->repeat ? ", repeat" : "");
	fplinux_ui_form_set_string(vibration_demo.form, vibration_demo.pattern,
				   text);
}

static void show_result(const char *text)
{
	if (vibration_demo.form)
		fplinux_ui_form_set_string(vibration_demo.form,
					   vibration_demo.result, text);
}

static void set_commands(struct apidemo *app, bool playing)
{
	const struct fplinux_ui_command play_command = {
		.id = COMMAND_PLAY,
		.type = FPLINUX_UI_COMMAND_SCREEN,
		.label = _("Play"),
		.icon = apidemo_glyph(app, "play"),
	};
	const struct fplinux_ui_command stop_command = {
		.id = COMMAND_STOP,
		.type = FPLINUX_UI_COMMAND_SCREEN,
		.label = _("Stop"),
		.icon = apidemo_glyph(app, "stop"),
	};

	if (!vibration_demo.form)
		return;
	fplinux_ui_screen_remove_command(vibration_demo.form, COMMAND_PLAY);
	fplinux_ui_screen_remove_command(vibration_demo.form, COMMAND_STOP);
	fplinux_ui_screen_add_command(vibration_demo.form,
				      playing ? &stop_command : &play_command);
}

/* "OK", or the errno name and its description. */
static void show_outcome(bool done, int error)
{
	char text[TEXT_BYTES] = "";

	if (done)
		append(text, sizeof(text), "%s", _("Request sent"));
	else
		append(text, sizeof(text), "%s (%s)", apidemo_errno_name(error),
		       strerror(error));
	show_result(text);
}

static bool play(struct apidemo *app, const struct request *request)
{
	bool done = fplinux_ui_vibration_play(app->ui, request->pattern_ms,
					      request->count, request->repeat);

	if (done) {
		vibration_demo.started = true;
		set_commands(app, true);
	}
	return done;
}

static void stop(struct apidemo *app, bool show)
{
	bool done = fplinux_ui_vibration_stop(app->ui);
	int error = errno;

	if (done) {
		vibration_demo.started = false;
		set_commands(app, false);
	}
	if (!show)
		return;
	if (done)
		show_result(_("Stop requested"));
	else
		show_outcome(false, error);
}

/* Choose a pattern before explicitly playing it. */

static void request_changed(struct fplinux_ui *ui,
			    struct fplinux_ui_screen *form, size_t item,
			    void *context)
{
	size_t index = fplinux_ui_form_get_choice(form, item);
	(void)ui;
	(void)context;
	if (item == vibration_demo.choice && index < APIDEMO_COUNT(patterns))
		show_pattern(&patterns[index]);
}

static void request_command(struct fplinux_ui *ui,
			    struct fplinux_ui_screen *form, int command_id,
			    void *context)
{
	if (command_id == COMMAND_PLAY) {
		size_t index =
			fplinux_ui_form_get_choice(form, vibration_demo.choice);
		bool done;

		if (index >= APIDEMO_COUNT(patterns))
			return;
		done = play(context, &patterns[index]);
		show_outcome(done, errno);
	} else if (command_id == COMMAND_STOP) {
		stop(context, true);
	} else if (command_id == APIDEMO_COMMAND_BACK) {
		/* Vibrate only while the user watches this screen. */
		if (vibration_demo.started)
			stop(context, false);
		vibration_demo.form = NULL;
		fplinux_ui_go_back(ui);
	}
}

static void open_requests(struct apidemo *app,
			  const struct apidemo_entry *entry)
{
	const char *choices[APIDEMO_COUNT(patterns)];
	struct fplinux_ui_screen *form =
		fplinux_ui_form_new(app->ui, _(entry->title));

	if (!form) {
		apidemo_error(app, "fplinux_ui_form_new");
		return;
	}
	for (size_t i = 0; i < APIDEMO_COUNT(patterns); i++)
		choices[i] = _(patterns[i].name);
	vibration_demo.form = form;
	/* Changing the popup only selects the pattern; Play sends the request. */
	vibration_demo.choice = fplinux_ui_form_append_choice(
		form, _("Pattern"), FPLINUX_UI_CHOICE_POPUP, choices,
		APIDEMO_COUNT(patterns));
	vibration_demo.result = fplinux_ui_form_append_string(
		form, _("Result"), _("Nothing sent yet."));
	vibration_demo.pattern =
		fplinux_ui_form_append_string(form, "pattern_ms", NULL);
	show_pattern(&patterns[0]);
	fplinux_ui_form_append_string(
		form, _("Vibration support"),
		fplinux_ui_vibration_supported(app->ui) ? _("Yes") : _("No"));
	fplinux_ui_form_append_string(
		form, NULL,
		_("Choose a pattern, then Play. Stop, Back or Home stops vibration."));
	set_commands(app, false);
	apidemo_add_back(form);
	fplinux_ui_screen_set_listener(form, request_command, request_changed,
				       app);
	fplinux_ui_push(app->ui, form);
}

/* Hooks */

static void vibration_visibility(struct apidemo *app, bool visible)
{
	(void)app;
	if (visible || !vibration_demo.form)
		return;
	/* The manager stops vibration; returning never starts it again. */
	vibration_demo.started = false;
	set_commands(app, false);
	show_result(_("Vibration stopped while hidden"));
}

static void vibration_close(struct apidemo *app)
{
	if (vibration_demo.started)
		stop(app, false);
}

static void vibration_cleanup(struct apidemo *app)
{
	vibration_close(app);
	vibration_demo.form = NULL;
}

void apidemo_vibration_error(struct apidemo *app, const char *reason)
{
	char text[TEXT_BYTES];

	vibration_demo.started = false;
	snprintf(text, sizeof(text), "%s: %s", _("Vibration request failed"),
		 reason);
	set_commands(app, false);
	if (vibration_demo.form)
		show_result(text);
	else
		apidemo_show(app, _("Vibration"), text);
}

static const struct apidemo_entry entries[] = {
	{ N_("Patterns"),
	  "fplinux_ui_vibration_supported · "
	  "fplinux_ui_vibration_play · fplinux_ui_vibration_stop",
	  open_requests, 0, N_("Choose, play and stop a vibration pattern") },
};

const struct apidemo_category apidemo_vibration = {
	.entries = entries,
	.count = APIDEMO_COUNT(entries),
	.visibility = vibration_visibility,
	.close = vibration_close,
	.cleanup = vibration_cleanup,
};
