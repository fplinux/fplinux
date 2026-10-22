/* SPDX-License-Identifier: 0BSD */
/*
 * Vibration: patterns for the vibrator. The session manager plays them only
 * while the application is visible and stops the vibrator when it is hidden
 * or leaves, so the application asks again after it is shown.
 */
#include "apidemo.h"

#include <fplinux/fplinux-ui-vibration.h>

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define TEXT_BYTES 256
/* The pattern readout shows this many durations, then an ellipsis. */
#define SHOWN_DURATIONS 18
/* The most requests one screen offers. */
#define REQUEST_LIMIT 8

enum { COMMAND_STOP = 1 };

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
/* A pulse lasts at most 3000 ms. */
static const unsigned int too_long_pulse[] = { 0, 4000 };
/* A pause between two pulses lasts at least 100 ms. */
static const unsigned int too_short_pause[] = { 0, 100, 50, 100 };
/* One duration more than a pattern takes; filled when the screen opens. */
static unsigned int too_many[FPLINUX_UI_VIBRATION_PATTERN_MAX + 1];

struct request {
	/* N_() text of the choice. */
	const char *name;
	const unsigned int *pattern_ms;
	size_t count;
	bool repeat;
	/* Played from the visibility hook once the application is hidden. */
	bool when_hidden;
};

#define REQUEST(name, pattern, repeat) \
	{ name, pattern, APIDEMO_COUNT(pattern), repeat, false }

static const struct request patterns[] = {
	REQUEST(N_("100 ms pulse"), short_pulse, false),
	REQUEST(N_("1 s pulse"), long_pulse, false),
	REQUEST(N_("Two pulses"), two_pulses, false),
	REQUEST(N_("Heartbeat"), heartbeat, false),
	REQUEST(N_("SOS"), sos, false),
	REQUEST(N_("Repeating"), on_off, true),
};

static const struct request errors[] = {
	REQUEST(N_("4 s pulse"), too_long_pulse, false),
	REQUEST(N_("50 ms pause"), too_short_pause, false),
	REQUEST(N_("65 durations"), too_many, false),
	REQUEST(N_("Valid pattern"), short_pulse, false),
	{ N_("While hidden"), short_pulse, APIDEMO_COUNT(short_pulse), false,
	  true },
};

enum { SCREEN_PATTERNS, SCREEN_ERRORS };

static const struct {
	/* N_() texts. */
	const char *label;
	const char *help;
	const struct request *requests;
	size_t count;
	/* Offers the Stop command. */
	bool stop;
} screens[] = {
	[SCREEN_PATTERNS] = { N_("Pattern"),
			      N_("OK chooses a pattern and plays it. Stop, "
				 "Back or hiding the application stops it. "
				 "The system may pause a long repeat to "
				 "spare the motor."),
			      patterns, APIDEMO_COUNT(patterns), true },
	[SCREEN_ERRORS] = { N_("Request"),
			    N_("Checked in this order: EINVAL for a wrong "
			       "pattern, ENOTSUP without a vibrator, EAGAIN "
			       "while the application is hidden."),
			    errors, APIDEMO_COUNT(errors), false },
};

/* The open patterns or errors screen; only one is on the stack at a time. */
static struct {
	struct fplinux_ui_screen *form;
	const struct request *requests;
	size_t count;
	size_t choice;
	size_t result;
	size_t pattern;
	/* The request to play when the application is next hidden, or NULL. */
	const struct request *armed;
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
		if (i == SHOWN_DURATIONS) {
			append(text, sizeof(text), ", …");
			break;
		}
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

	if (done)
		vibration_demo.started = true;
	return done;
}

static void stop(struct apidemo *app, bool show)
{
	bool done = fplinux_ui_vibration_stop(app->ui);
	int error = errno;

	if (done)
		vibration_demo.started = false;
	if (!show)
		return;
	if (done)
		show_result(_("Stop requested"));
	else
		show_outcome(false, error);
}

/* Supported */

static void open_supported(struct apidemo *app,
			   const struct apidemo_entry *entry)
{
	struct fplinux_ui_screen *form =
		fplinux_ui_form_new(app->ui, _(entry->title));

	if (!form) {
		apidemo_error(app, "fplinux_ui_form_new");
		return;
	}
	fplinux_ui_form_append_string(
		form, _("Vibrator"),
		fplinux_ui_vibration_supported(app->ui) ? _("Yes") : _("No"));
	fplinux_ui_form_append_string(
		form, NULL,
		_("Fixed for the process. Without a vibrator, every pattern "
		  "fails with ENOTSUP."));
	apidemo_add_back(form);
	fplinux_ui_screen_set_listener(form, apidemo_back_listener, NULL, app);
	fplinux_ui_push(app->ui, form);
}

/* Patterns and errors */

static void request_changed(struct fplinux_ui *ui,
			    struct fplinux_ui_screen *form, size_t item,
			    void *context)
{
	size_t index = fplinux_ui_form_get_choice(form, item);
	const struct request *request;
	bool done;

	(void)ui;
	if (item != vibration_demo.choice || index >= vibration_demo.count)
		return;
	request = &vibration_demo.requests[index];
	show_pattern(request);
	if (request->when_hidden) {
		vibration_demo.armed = request;
		show_result(_("Now lock the keypad."));
		return;
	}
	vibration_demo.armed = NULL;
	done = play(context, request);
	show_outcome(done, errno);
}

static void request_command(struct fplinux_ui *ui,
			    struct fplinux_ui_screen *form, int command_id,
			    void *context)
{
	(void)form;
	if (command_id == COMMAND_STOP) {
		stop(context, true);
	} else if (command_id == APIDEMO_COMMAND_BACK) {
		/* Vibrate only while the user watches this screen. */
		if (vibration_demo.started)
			stop(context, false);
		vibration_demo.form = NULL;
		vibration_demo.armed = NULL;
		fplinux_ui_go_back(ui);
	}
}

static void open_requests(struct apidemo *app,
			  const struct apidemo_entry *entry)
{
	const struct fplinux_ui_command stop_command = {
		.id = COMMAND_STOP,
		.type = FPLINUX_UI_COMMAND_SCREEN,
		.label = _("Stop"),
		.priority = 0,
	};
	const char *choices[REQUEST_LIMIT];
	size_t count = screens[entry->variant].count;
	struct fplinux_ui_screen *form =
		fplinux_ui_form_new(app->ui, _(entry->title));

	if (!form) {
		apidemo_error(app, "fplinux_ui_form_new");
		return;
	}
	for (size_t i = 0; i < APIDEMO_COUNT(too_many); i++)
		too_many[i] = i ? 100 : 0;
	if (count > REQUEST_LIMIT)
		count = REQUEST_LIMIT;
	for (size_t i = 0; i < count; i++)
		choices[i] = _(screens[entry->variant].requests[i].name);
	vibration_demo.form = form;
	vibration_demo.requests = screens[entry->variant].requests;
	vibration_demo.count = count;
	vibration_demo.armed = NULL;
	/* A popup keeps the result below it in view on the smallest screen;
	 * choosing plays at once, also the pattern already chosen. */
	vibration_demo.choice = fplinux_ui_form_append_choice(
		form, _(screens[entry->variant].label), FPLINUX_UI_CHOICE_POPUP,
		choices, count);
	vibration_demo.result = fplinux_ui_form_append_string(
		form, _("Result"), _("Nothing sent yet."));
	vibration_demo.pattern =
		fplinux_ui_form_append_string(form, "pattern_ms", NULL);
	show_pattern(&vibration_demo.requests[0]);
	fplinux_ui_form_append_string(form, NULL,
				      _(screens[entry->variant].help));
	if (screens[entry->variant].stop)
		fplinux_ui_screen_add_command(form, &stop_command);
	apidemo_add_back(form);
	fplinux_ui_screen_set_listener(form, request_command, request_changed,
				       app);
	fplinux_ui_push(app->ui, form);
}

/* Hooks */

static void vibration_visibility(struct apidemo *app, bool visible)
{
	const struct request *armed = vibration_demo.armed;
	bool done;

	if (visible || !vibration_demo.form)
		return;
	/* The manager has stopped the vibrator: nothing resumes on show. */
	vibration_demo.started = false;
	if (!armed) {
		show_result(_("Vibration stopped while hidden"));
		return;
	}
	/* The runtime counts the application as hidden before this hook; the
	 * popup still names the request. */
	vibration_demo.armed = NULL;
	done = play(app, armed);
	show_outcome(done, errno);
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
	vibration_demo.armed = NULL;
}

void apidemo_vibration_error(struct apidemo *app, const char *reason)
{
	char text[TEXT_BYTES];

	vibration_demo.started = false;
	snprintf(text, sizeof(text), "%s: %s", _("Vibration request failed"),
		 reason);
	if (vibration_demo.form)
		show_result(text);
	else
		apidemo_show(app, _("Vibration"), text);
}

static const struct apidemo_entry entries[] = {
	{ N_("Supported"), "fplinux_ui_vibration_supported", open_supported,
	  0 },
	{ N_("Patterns"), "fplinux_ui_vibration_play · stop · repeat",
	  open_requests, SCREEN_PATTERNS },
	{ N_("Errors"), "fplinux_ui_vibration_play · EINVAL · ENOTSUP · EAGAIN",
	  open_requests, SCREEN_ERRORS },
};

const struct apidemo_category apidemo_vibration = {
	.title = N_("Vibration"),
	.icon = APIDEMO_IMAGE_DIR "vibration-symbolic.png",
	.entries = entries,
	.count = APIDEMO_COUNT(entries),
	.visibility = vibration_visibility,
	.close = vibration_close,
	.cleanup = vibration_cleanup,
};
