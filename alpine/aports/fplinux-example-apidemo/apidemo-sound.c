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
enum { COMMAND_START = 1, COMMAND_STOP };

static const unsigned int rates[] = { 8000, 22050, 48000 };

/* The open demo screen; only one is on the stack at a time. */
static struct {
	struct apidemo *app;
	struct fplinux_ui_screen *form;
	struct fplinux_audio_stream *stream;
	lv_timer_t *timer;
	unsigned int rate;
	unsigned int channels;
	/* Tone phases, 2^32 per cycle, left and right. */
	uint32_t phase[2];
	size_t rate_item;
	size_t channels_item;
	size_t status;
	size_t queued;
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
		.icon = apidemo_glyph(sound.app, "play"),
	};
	const struct fplinux_ui_command stop = {
		.id = COMMAND_STOP,
		.type = FPLINUX_UI_COMMAND_SCREEN,
		.label = _("Stop"),
		.icon = apidemo_glyph(sound.app, "stop"),
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
	if (sound.form)
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

static void close_screen(struct fplinux_ui *ui)
{
	/* Sound plays only while the user watches the screen. */
	stop_stream();
	sound.form = NULL;
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
	return form;
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
	sound.app = app;
	for (size_t i = 0; i < APIDEMO_COUNT(rates); i++) {
		snprintf(rate_names[i], sizeof(rate_names[i]), _("%u Hz"),
			 rates[i]);
		rate_choices[i] = rate_names[i];
	}
	sound.rate = rates[0];
	sound.channels = 1;
	sound.status = fplinux_ui_form_append_string(
		form, _("Status"),
		fplinux_ui_audio_supported(app->ui) ? _("Stopped") :
						      _("Sound unavailable"));
	sound.rate_item = fplinux_ui_form_append_choice(
		form, _("Rate"), FPLINUX_UI_CHOICE_POPUP, rate_choices,
		APIDEMO_COUNT(rate_choices));
	sound.channels_item = fplinux_ui_form_append_choice(
		form, _("Channels"), FPLINUX_UI_CHOICE_POPUP, channel_choices,
		APIDEMO_COUNT(channel_choices));
	sound.queued = fplinux_ui_form_append_string(form, _("Queued"),
						     _("No stream"));
	fplinux_ui_form_append_string(
		form, _("Sound"),
		fplinux_ui_audio_supported(app->ui) ? _("Yes") : _("No"));
	fplinux_ui_form_append_string(
		form, _("Background playback"),
		fplinux_ui_audio_background_supported(app->ui) ? _("Yes") :
								 _("No"));
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

/* Hooks */

static void sound_visibility(struct apidemo *app, bool visible)
{
	(void)app;
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
	if (sound.stream)
		set_line(sound.status,
			 visible ? _("Streaming") : _("Paused while hidden"));
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
}

static const struct apidemo_entry entries[] = {
	{ N_("Tone"),
	  "fplinux_ui_audio_supported · fplinux_ui_audio_open · "
	  "fplinux_ui_audio_write · fplinux_ui_audio_queued · fplinux_ui_audio_close",
	  open_tone, 0, N_("Control a tone and inspect queued audio") },
};

const struct apidemo_category apidemo_sound = {
	.entries = entries,
	.count = APIDEMO_COUNT(entries),
	.visibility = sound_visibility,
	.close = sound_close,
	.cleanup = sound_cleanup,
};
