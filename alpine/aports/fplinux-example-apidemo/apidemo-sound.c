/* SPDX-License-Identifier: 0BSD */
/*
 * Sound: streams of 16-bit frames that the system mixes and plays while the
 * application is visible. Hiding the application pauses its streams with
 * their frames kept and showing it resumes them; leaving ends them. A timer
 * on the runtime thread keeps a tone queued and pauses while hidden.
 */
#include "apidemo.h"

#include <fplinux/fplinux-ui-audio.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TEXT_BYTES 160
/* The timer that feeds the tone and refreshes Queued. */
#define TICK_MS 40
/* Kept queued: a tick and a margin, less than the 250 ms a stream holds. */
#define AHEAD_MS 160
#define CHUNK_FRAMES 512
#define LEFT_HZ 440
#define RIGHT_HZ 660
#define AMPLITUDE 6000
/* The Stream screen's stream holds 2048 frames; writes offer 1000. */
#define STREAM_RATE 8000
#define FILL_FRAMES 1000
#define FILL_WRITES 4

enum { COMMAND_START = 1, COMMAND_STOP };

static const unsigned int rates[] = { 8000, 22050, 48000 };

enum screen { SCREEN_TONE = 1, SCREEN_STREAM, SCREEN_ERRORS };

enum request {
	REQUEST_FILL,
	REQUEST_NULL_FRAMES,
	REQUEST_CLOSE,
	REQUEST_CLOSE_NULL,
	REQUEST_LOW_RATE,
	REQUEST_HIGH_RATE,
	REQUEST_CHANNELS,
	REQUEST_FIVE,
	REQUEST_VALID,
	REQUEST_HIDDEN,
};

static const struct {
	enum request request;
	/* N_() text of the choice, and the call as C. */
	const char *name;
	const char *call;
} stream_requests[] = {
	{ REQUEST_FILL, N_("Fill"), "write(audio, frames, 1000)" },
	{ REQUEST_NULL_FRAMES, N_("NULL frames"), "write(audio, NULL, 1)" },
	{ REQUEST_CLOSE, N_("Close"), "close(audio)" },
	{ REQUEST_CLOSE_NULL, N_("Close NULL"), "close(NULL)" },
}, error_requests[] = {
	{ REQUEST_LOW_RATE, N_("7999 Hz"), "open(ui, 7999, 1)" },
	{ REQUEST_HIGH_RATE, N_("48001 Hz"), "open(ui, 48001, 2)" },
	{ REQUEST_CHANNELS, N_("3 channels"), "open(ui, 22050, 3)" },
	{ REQUEST_FIVE, N_("5 streams"), "open(ui, 8000, 1) × 5" },
	{ REQUEST_VALID, N_("Valid stream"), "open(ui, 22050, 2)" },
	{ REQUEST_HIDDEN, N_("While hidden"), "open(ui, 22050, 2)" },
};

/* The open demo screen; only one is on the stack at a time. */
static struct {
	struct fplinux_ui_screen *form;
	enum screen screen;
	struct fplinux_audio_stream *stream;
	lv_timer_t *timer;
	unsigned int rate;
	unsigned int channels;
	/* Tone phases, 2^32 per cycle, left and right. */
	uint32_t phase[2];
	size_t rate_item;
	size_t channels_item;
	size_t choice;
	size_t call;
	size_t status;
	size_t queued;
	/* Opens a stream when the application is next hidden. */
	bool armed;
} sound;

/* sin(2πp / 2^32) × AMPLITUDE by Bhaskara's approximation, without libm. */
static int16_t tone_sample(uint32_t phase)
{
	int64_t half = (phase >> 16) & 0x7fff;
	int64_t product = half * (32768 - half);
	int64_t value =
		16 * product * AMPLITUDE / (5LL * 32768 * 32768 - 4 * product);

	return (int16_t)(phase & 0x80000000U ? -value : value);
}

static uint32_t phase_step(unsigned int hz)
{
	return (uint32_t)(((uint64_t)hz << 32) / sound.rate);
}

/* count frames of the tone from the current phases, which stay. */
static void generate(int16_t *frames, size_t count)
{
	uint32_t left = sound.phase[0];
	uint32_t right = sound.phase[1];

	for (size_t i = 0; i < count; i++) {
		frames[i * sound.channels] = tone_sample(left);
		if (sound.channels == 2)
			frames[i * 2 + 1] = tone_sample(right);
		left += phase_step(LEFT_HZ);
		right += phase_step(RIGHT_HZ);
	}
}

/* The stream took count frames: the tone goes on after them. */
static void advance(long count)
{
	sound.phase[0] += phase_step(LEFT_HZ) * (uint32_t)count;
	sound.phase[1] += phase_step(RIGHT_HZ) * (uint32_t)count;
}

static void set_line(size_t item, const char *text)
{
	if (sound.form)
		fplinux_ui_form_set_string(sound.form, item, text);
}

/* "OK", or the errno name and its description. */
static void show_outcome(size_t item, bool done, int error)
{
	char text[TEXT_BYTES];

	if (done)
		snprintf(text, sizeof(text), "%s", _("OK"));
	else
		snprintf(text, sizeof(text), "%s (%s)",
			 apidemo_errno_name(error), strerror(error));
	set_line(item, text);
}

static void show_queued(void)
{
	char text[TEXT_BYTES];
	long queued;

	if (!sound.stream) {
		set_line(sound.queued, _("No stream"));
		return;
	}
	queued = fplinux_ui_audio_queued(sound.stream);
	if (queued < 0) {
		show_outcome(sound.queued, false, errno);
		return;
	}
	snprintf(text, sizeof(text), _("%ld ms"),
		 (long)((long long)queued * 1000 / sound.rate));
	set_line(sound.queued, text);
}

static void set_commands(bool playing)
{
	const struct fplinux_ui_command start = {
		.id = COMMAND_START,
		.type = FPLINUX_UI_COMMAND_SCREEN,
		.label = _("Start"),
	};
	const struct fplinux_ui_command stop = {
		.id = COMMAND_STOP,
		.type = FPLINUX_UI_COMMAND_SCREEN,
		.label = _("Stop"),
	};

	fplinux_ui_screen_remove_command(sound.form, COMMAND_START);
	fplinux_ui_screen_remove_command(sound.form, COMMAND_STOP);
	fplinux_ui_screen_add_command(sound.form, playing ? &stop : &start);
}

/* Ends the stream and its timer; the system drops frames not yet heard. */
static void stop_stream(void)
{
	if (sound.timer)
		lv_timer_delete(sound.timer);
	sound.timer = NULL;
	fplinux_ui_audio_close(sound.stream);
	sound.stream = NULL;
}

/* write or queued failed: EPIPE once the system has ended the stream. */
static void stream_failed(int error)
{
	stop_stream();
	show_outcome(sound.status, false, error);
	if (sound.screen == SCREEN_TONE && sound.form)
		set_commands(false);
}

/* Keeps AHEAD_MS of the tone queued. */
static void feed(void)
{
	static int16_t frames[CHUNK_FRAMES * 2];
	long queued = fplinux_ui_audio_queued(sound.stream);
	long wanted = (long)(sound.rate * AHEAD_MS / 1000) - queued;

	if (queued < 0) {
		stream_failed(errno);
		return;
	}
	while (wanted > 0) {
		size_t count = wanted > CHUNK_FRAMES ? CHUNK_FRAMES :
						       (size_t)wanted;
		long taken;

		generate(frames, count);
		taken = fplinux_ui_audio_write(sound.stream, frames, count);
		if (taken < 0) {
			stream_failed(errno);
			return;
		}
		advance(taken);
		/* A full stream takes fewer; the next tick goes on. */
		if ((size_t)taken < count)
			break;
		wanted -= taken;
	}
}

static void tick(lv_timer_t *timer)
{
	(void)timer;
	if (sound.screen == SCREEN_TONE)
		feed();
	show_queued();
}

static bool start_timer(struct apidemo *app)
{
	sound.timer = lv_timer_create(tick, TICK_MS, app);
	if (!sound.timer) {
		apidemo_error(app, "lv_timer_create");
		return false;
	}
	if (!app->visible)
		lv_timer_pause(sound.timer);
	return true;
}

/* Opens a stream at the chosen rate and channels and starts the tone. */
static void start_tone(struct apidemo *app)
{
	sound.stream =
		fplinux_ui_audio_open(app->ui, sound.rate, sound.channels);
	if (!sound.stream) {
		show_outcome(sound.status, false, errno);
		return;
	}
	if (!start_timer(app)) {
		stop_stream();
		return;
	}
	sound.phase[0] = sound.phase[1] = 0;
	set_line(sound.status, _("Streaming"));
	set_commands(true);
	tick(sound.timer);
}

/* A stream for the Stream screen; false after showing why it failed. */
static bool stream_ready(struct apidemo *app)
{
	if (sound.stream)
		return true;
	sound.rate = STREAM_RATE;
	sound.channels = 1;
	sound.stream =
		fplinux_ui_audio_open(app->ui, sound.rate, sound.channels);
	if (!sound.stream) {
		show_outcome(sound.status, false, errno);
		return false;
	}
	if (!start_timer(app)) {
		stop_stream();
		return false;
	}
	return true;
}

/* Writes FILL_FRAMES at a time until the stream takes fewer. */
static void fill(struct apidemo *app)
{
	static int16_t frames[FILL_FRAMES];
	char counts[TEXT_BYTES / 2] = "";
	char text[TEXT_BYTES];
	size_t used = 0;
	long taken = FILL_FRAMES;

	if (!stream_ready(app))
		return;
	generate(frames, FILL_FRAMES);
	for (int i = 0; i < FILL_WRITES && taken == FILL_FRAMES; i++) {
		taken = fplinux_ui_audio_write(sound.stream, frames,
					       FILL_FRAMES);
		if (taken < 0) {
			stream_failed(errno);
			return;
		}
		advance(taken);
		generate(frames, FILL_FRAMES);
		used += (size_t)snprintf(counts + used, sizeof(counts) - used,
					 "%s%ld", i ? " + " : "", taken);
		if (used >= sizeof(counts))
			used = sizeof(counts) - 1;
	}
	snprintf(text, sizeof(text), _("Took %s frames"), counts);
	set_line(sound.status, text);
	show_queued();
}

/* Opens five streams: four fit, then EMFILE; all are closed again. */
static void open_five(struct apidemo *app)
{
	struct fplinux_audio_stream *streams[5] = { NULL };
	char text[TEXT_BYTES];
	size_t opened = 0;
	int error = 0;

	for (; opened < APIDEMO_COUNT(streams); opened++) {
		streams[opened] = fplinux_ui_audio_open(app->ui, 8000, 1);
		if (!streams[opened]) {
			error = errno;
			break;
		}
	}
	for (size_t i = 0; i < opened; i++)
		fplinux_ui_audio_close(streams[i]);
	if (!error) {
		show_outcome(sound.status, true, 0);
		return;
	}
	snprintf(text, sizeof(text), _("%zu opened, then %s (%s)"), opened,
		 apidemo_errno_name(error), strerror(error));
	set_line(sound.status, text);
}

/* Opens and closes one stream with these arguments. */
static void try_open(struct apidemo *app, unsigned int rate,
		     unsigned int channels)
{
	struct fplinux_audio_stream *audio =
		fplinux_ui_audio_open(app->ui, rate, channels);
	int error = errno;

	fplinux_ui_audio_close(audio);
	show_outcome(sound.status, audio != NULL, error);
}

static void run_request(struct apidemo *app, enum request request)
{
	switch (request) {
	case REQUEST_FILL:
		fill(app);
		break;
	case REQUEST_NULL_FRAMES:
		if (stream_ready(app)) {
			long taken =
				fplinux_ui_audio_write(sound.stream, NULL, 1);

			show_outcome(sound.status, taken >= 0, errno);
		}
		break;
	case REQUEST_CLOSE:
		set_line(sound.status,
			 sound.stream ?
				 _("Closed; frames not yet heard are dropped") :
				 _("No stream"));
		stop_stream();
		show_queued();
		break;
	case REQUEST_CLOSE_NULL:
		fplinux_ui_audio_close(NULL);
		set_line(sound.status, _("Ignored"));
		break;
	case REQUEST_LOW_RATE:
		try_open(app, 7999, 1);
		break;
	case REQUEST_HIGH_RATE:
		try_open(app, 48001, 2);
		break;
	case REQUEST_CHANNELS:
		try_open(app, 22050, 3);
		break;
	case REQUEST_FIVE:
		open_five(app);
		break;
	case REQUEST_VALID:
		try_open(app, 22050, 2);
		break;
	case REQUEST_HIDDEN:
		sound.armed = true;
		set_line(sound.status, _("Now lock the keypad."));
		break;
	}
}

static void close_screen(struct fplinux_ui *ui)
{
	/* Sound plays only while the user watches the screen. */
	stop_stream();
	sound.form = NULL;
	sound.armed = false;
	fplinux_ui_go_back(ui);
}

static struct fplinux_ui_screen *new_form(struct apidemo *app,
					  const struct apidemo_entry *entry)
{
	struct fplinux_ui_screen *form =
		fplinux_ui_form_new(app->ui, _(entry->title));

	if (!form) {
		apidemo_error(app, "fplinux_ui_form_new");
		return NULL;
	}
	stop_stream();
	sound.form = form;
	sound.screen = entry->variant;
	sound.armed = false;
	return form;
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
		form, _("Sound"),
		fplinux_ui_audio_supported(app->ui) ? _("Yes") : _("No"));
	fplinux_ui_form_append_string(
		form, _("Background playback"),
		fplinux_ui_audio_background_supported(app->ui) ? _("Yes") :
								 _("No"));
	fplinux_ui_form_append_string(
		form, NULL,
		_("Fixed for the process. Without sound, every open fails "
		  "with ENOTSUP."));
	apidemo_add_back(form);
	fplinux_ui_screen_set_listener(form, apidemo_back_listener, NULL, app);
	fplinux_ui_push(app->ui, form);
}

/* Tone */

static void tone_changed(struct fplinux_ui *ui, struct fplinux_ui_screen *form,
			 size_t item, void *context)
{
	(void)ui;
	if (item != sound.rate_item && item != sound.channels_item)
		return;
	sound.rate = rates[fplinux_ui_form_get_choice(form, sound.rate_item)];
	sound.channels =
		fplinux_ui_form_get_choice(form, sound.channels_item) + 1;
	/* A playing tone starts again in the new format. */
	if (sound.stream) {
		stop_stream();
		start_tone(context);
	}
}

static void tone_command(struct fplinux_ui *ui, struct fplinux_ui_screen *form,
			 int command_id, void *context)
{
	(void)form;
	if (command_id == COMMAND_START) {
		start_tone(context);
	} else if (command_id == COMMAND_STOP) {
		stop_stream();
		set_line(sound.status, _("Stopped"));
		show_queued();
		set_commands(false);
	} else if (command_id == APIDEMO_COMMAND_BACK) {
		close_screen(ui);
	}
}

static void open_tone(struct apidemo *app, const struct apidemo_entry *entry)
{
	static char rate_names[APIDEMO_COUNT(rates)][24];
	const char *rate_choices[APIDEMO_COUNT(rates)];
	const char *channel_choices[] = { _("Mono"), _("Stereo") };
	struct fplinux_ui_screen *form = new_form(app, entry);

	if (!form)
		return;
	for (size_t i = 0; i < APIDEMO_COUNT(rates); i++) {
		snprintf(rate_names[i], sizeof(rate_names[i]), _("%u Hz"),
			 rates[i]);
		rate_choices[i] = rate_names[i];
	}
	sound.rate = rates[0];
	sound.channels = 1;
	sound.rate_item = fplinux_ui_form_append_choice(
		form, _("Rate"), FPLINUX_UI_CHOICE_POPUP, rate_choices,
		APIDEMO_COUNT(rate_choices));
	sound.channels_item = fplinux_ui_form_append_choice(
		form, _("Channels"), FPLINUX_UI_CHOICE_POPUP, channel_choices,
		APIDEMO_COUNT(channel_choices));
	sound.status =
		fplinux_ui_form_append_string(form, _("Status"), _("Stopped"));
	sound.queued = fplinux_ui_form_append_string(form, _("Queued"),
						     _("No stream"));
	fplinux_ui_form_append_string(
		form, NULL,
		_("Start opens a stream; a timer keeps 160 ms of a 440 Hz "
		  "tone queued, 660 Hz on the right in stereo. Hiding the "
		  "application pauses the stream with its frames kept."));
	set_commands(false);
	apidemo_add_back(form);
	fplinux_ui_screen_set_listener(form, tone_command, tone_changed, app);
	fplinux_ui_push(app->ui, form);
}

/* Stream and errors */

static void request_changed(struct fplinux_ui *ui,
			    struct fplinux_ui_screen *form, size_t item,
			    void *context)
{
	size_t index = fplinux_ui_form_get_choice(form, item);

	(void)ui;
	if (item != sound.choice)
		return;
	if (sound.screen == SCREEN_STREAM &&
	    index < APIDEMO_COUNT(stream_requests)) {
		set_line(sound.call, stream_requests[index].call);
		run_request(context, stream_requests[index].request);
	} else if (sound.screen == SCREEN_ERRORS &&
		   index < APIDEMO_COUNT(error_requests)) {
		sound.armed = false;
		set_line(sound.call, error_requests[index].call);
		run_request(context, error_requests[index].request);
	}
}

static void request_command(struct fplinux_ui *ui,
			    struct fplinux_ui_screen *form, int command_id,
			    void *context)
{
	(void)form;
	(void)context;
	if (command_id == APIDEMO_COMMAND_BACK)
		close_screen(ui);
}

static void open_requests(struct apidemo *app,
			  const struct apidemo_entry *entry)
{
	const char *choices[APIDEMO_COUNT(error_requests)];
	bool stream = entry->variant == SCREEN_STREAM;
	size_t count = stream ? APIDEMO_COUNT(stream_requests) :
				APIDEMO_COUNT(error_requests);
	struct fplinux_ui_screen *form = new_form(app, entry);

	if (!form)
		return;
	for (size_t i = 0; i < count; i++)
		choices[i] = _(stream ? stream_requests[i].name :
					error_requests[i].name);
	/* A popup keeps the result below it in view on the smallest screen;
	 * choosing runs the request at once, also the one already chosen. */
	sound.choice = fplinux_ui_form_append_choice(
		form, _("Request"), FPLINUX_UI_CHOICE_POPUP, choices, count);
	sound.status = fplinux_ui_form_append_string(form, _("Result"),
						     _("Nothing sent yet."));
	sound.call = fplinux_ui_form_append_string(
		form, "fplinux_ui_audio_",
		stream ? stream_requests[0].call : error_requests[0].call);
	if (stream)
		sound.queued = fplinux_ui_form_append_string(form, _("Queued"),
							     _("No stream"));
	fplinux_ui_form_append_string(
		form, NULL,
		stream ? _("An 8000 Hz mono stream holds 2048 frames, 256 ms. "
			   "A full stream takes fewer frames; Queued falls as "
			   "the system plays them.") :
			 _("Checked in this order: EINVAL for a rate or "
			   "channel count out of range, ENOTSUP without sound, "
			   "EAGAIN while hidden, EMFILE with four streams "
			   "open, then errors of the audio service such as "
			   "EBUSY or ECONNREFUSED."));
	apidemo_add_back(form);
	fplinux_ui_screen_set_listener(form, request_command, request_changed,
				       app);
	fplinux_ui_push(app->ui, form);
}

/* Hooks */

static void sound_visibility(struct apidemo *app, bool visible)
{
	if (!sound.form)
		return;
	/* The system pauses the stream; the timer waits with it. */
	if (sound.timer) {
		if (visible) {
			lv_timer_resume(sound.timer);
			tick(sound.timer);
		} else {
			lv_timer_pause(sound.timer);
		}
	}
	if (sound.screen == SCREEN_TONE && sound.stream)
		set_line(sound.status,
			 visible ? _("Streaming") : _("Paused while hidden"));
	if (!visible && sound.armed) {
		/* The runtime counts the application as hidden before this
		 * hook. */
		sound.armed = false;
		try_open(app, 22050, 2);
	}
}

static void sound_close(struct apidemo *app)
{
	(void)app;
	stop_stream();
}

static void sound_cleanup(struct apidemo *app)
{
	sound_close(app);
	sound.form = NULL;
	sound.armed = false;
}

static const struct apidemo_entry entries[] = {
	{ N_("Supported"), "fplinux_ui_audio_supported", open_supported, 0 },
	{ N_("Tone"), "fplinux_ui_audio_open · write · queued · close",
	  open_tone, SCREEN_TONE },
	{ N_("Stream"), "fplinux_ui_audio_write · queued · close · NULL",
	  open_requests, SCREEN_STREAM },
	{ N_("Errors"), "fplinux_ui_audio_open · EINVAL · EMFILE · EAGAIN",
	  open_requests, SCREEN_ERRORS },
};

const struct apidemo_category apidemo_sound = {
	.title = N_("Sound"),
	.icon = APIDEMO_IMAGE_DIR "sound-symbolic.png",
	.entries = entries,
	.count = APIDEMO_COUNT(entries),
	.visibility = sound_visibility,
	.close = sound_close,
	.cleanup = sound_cleanup,
};
