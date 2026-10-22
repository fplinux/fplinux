// SPDX-License-Identifier: GPL-2.0-only
#define _POSIX_C_SOURCE 200809L

#include "fplinux-audio-client-internal.h"
#include "fplinux-cli.h"
#include "session-vibrator.h"

#include <errno.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define SIGNAL_RATE_HZ 48000U
#define SIGNAL_CHANNELS 2U
#define SIGNAL_CHUNK_FRAMES 480U
#define SIGNAL_CYCLE_MS 2000U
#define SIGNAL_TONE_MS 200U
#define SIGNAL_TONE_FRAMES (SIGNAL_TONE_MS * SIGNAL_RATE_HZ / 1000U)
#define SIGNAL_FADE_MS 5U
#define SIGNAL_SECOND_TONE_MS 350U
#define SIGNAL_RUMBLE_START_MS 1000U
#define SIGNAL_RUMBLE_MS 250U
#define SIGNAL_DURATION_MS 60000U
#define SIGNAL_WAIT_MS 20
#define SIGNAL_AUDIO_RETRY_MS 1000U

struct signal_output {
	struct fplinux_audio_client *audio;
	struct fplinux_audio_stream *stream;
	bool audio_unavailable;
	struct fplinux_session_vibrator vibrator;
	int16_t tones[2][SIGNAL_TONE_FRAMES];
};

static volatile sig_atomic_t stopped;

static void handle_signal(int number)
{
	stopped = number;
}

static bool install_signals(void)
{
	struct sigaction action = { .sa_handler = handle_signal };

	sigemptyset(&action.sa_mask);
	return sigaction(SIGTERM, &action, NULL) == 0 &&
	       sigaction(SIGINT, &action, NULL) == 0;
}

static bool monotonic_ms(uint64_t *value)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
		return false;
	*value =
		(uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
	return true;
}

static void close_audio(struct signal_output *output)
{
	fplinux_audio_stream_close(output->stream);
	output->stream = NULL;
	fplinux_audio_client_close(output->audio);
	output->audio = NULL;
}

static void audio_failed(struct signal_output *output, const char *message,
			 int error)
{
	if (!output->audio_unavailable)
		fprintf(stderr, "clock-signal: %s: %s\n", message,
			strerror(error));
	output->audio_unavailable = true;
	close_audio(output);
}

static bool open_audio(struct signal_output *output)
{
	output->audio = fplinux_audio_client_create();
	if (output->audio)
		output->stream = fplinux_audio_stream_open(
			output->audio, SIGNAL_RATE_HZ, SIGNAL_CHANNELS);
	if (!output->stream) {
		audio_failed(output, "audio unavailable", errno);
		return false;
	}
	output->audio_unavailable = false;
	return true;
}

static void generate_tone(int16_t *samples, unsigned int frequency_hz)
{
	const unsigned int fade_frames =
		SIGNAL_FADE_MS * SIGNAL_RATE_HZ / 1000U;
	unsigned int position;

	for (position = 0; position < SIGNAL_TONE_FRAMES; ++position) {
		double envelope = 1.0;
		double phase;

		if (position < fade_frames)
			envelope = (double)position / fade_frames;
		else if (SIGNAL_TONE_FRAMES - position < fade_frames)
			envelope = (double)(SIGNAL_TONE_FRAMES - position) /
				   fade_frames;
		phase = 6.283185307179586 * frequency_hz * position /
			SIGNAL_RATE_HZ;
		samples[position] = (int16_t)(3000.0 * envelope * sin(phase));
	}
}

static int16_t tone_sample(const struct signal_output *output, uint64_t frame)
{
	const unsigned int frames_per_ms = SIGNAL_RATE_HZ / 1000U;
	unsigned int position = frame % (SIGNAL_CYCLE_MS * frames_per_ms);
	unsigned int tone = 0;

	if (position >= SIGNAL_SECOND_TONE_MS * frames_per_ms) {
		position -= SIGNAL_SECOND_TONE_MS * frames_per_ms;
		tone = 1;
	}
	if (position >= SIGNAL_TONE_FRAMES)
		return 0;
	return output->tones[tone][position];
}

static bool write_audio(struct signal_output *output, uint64_t *frame)
{
	int16_t samples[SIGNAL_CHUNK_FRAMES * SIGNAL_CHANNELS];
	long written;
	unsigned int index;

	for (index = 0; index < SIGNAL_CHUNK_FRAMES; ++index) {
		int16_t value = tone_sample(output, *frame + index);

		samples[index * SIGNAL_CHANNELS] = value;
		samples[index * SIGNAL_CHANNELS + 1U] = value;
	}
	written = fplinux_audio_stream_write(output->stream, samples,
					     SIGNAL_CHUNK_FRAMES);
	if (written > 0) {
		*frame += (uint64_t)written;
		return true;
	}
	if (written < 0)
		audio_failed(output, "audio playback failed", errno);
	return false;
}

static int run_signal(void)
{
	struct signal_output output = { 0 };
	uint64_t frame = 0;
	uint64_t started_ms;
	uint64_t now_ms;
	uint64_t retry_ms = 0;
	bool had_output;
	bool ok = true;

	fplinux_session_vibrator_init(&output.vibrator);
	if (!install_signals() || !monotonic_ms(&started_ms)) {
		fprintf(stderr, "clock-signal: cannot initialize alert: %s\n",
			strerror(errno));
		return 1;
	}
	generate_tone(output.tones[0], 660U);
	generate_tone(output.tones[1], 880U);
	if (!stopped &&
	    !fplinux_session_vibrator_open(&output.vibrator, "/dev/input",
					   SIGNAL_RUMBLE_MS, UINT16_MAX / 2U))
		fprintf(stderr, "clock-signal: vibration unavailable: %s\n",
			strerror(errno));
	had_output = fplinux_session_vibrator_available(&output.vibrator);
	while (!stopped) {
		struct pollfd descriptor;
		unsigned int phase_ms;
		bool vibrating;

		if (!monotonic_ms(&now_ms)) {
			fprintf(stderr,
				"clock-signal: cannot read alert time: %s\n",
				strerror(errno));
			ok = false;
			break;
		}
		if (now_ms - started_ms >= SIGNAL_DURATION_MS)
			break;
		phase_ms = (now_ms - started_ms) % SIGNAL_CYCLE_MS;
		vibrating = phase_ms >= SIGNAL_RUMBLE_START_MS &&
			    phase_ms <
				    SIGNAL_RUMBLE_START_MS + SIGNAL_RUMBLE_MS;
		if (fplinux_session_vibrator_available(&output.vibrator) &&
		    !fplinux_session_vibrator_set(&output.vibrator,
						  vibrating)) {
			fprintf(stderr, "clock-signal: vibration failed: %s\n",
				strerror(errno));
			ok = false;
		}
		if (!output.stream && now_ms >= retry_ms) {
			if (open_audio(&output))
				had_output = true;
			retry_ms = now_ms + SIGNAL_AUDIO_RETRY_MS;
		}
		if (stopped)
			break;
		if (output.stream) {
			if (write_audio(&output, &frame))
				continue;
			if (!output.stream)
				retry_ms = now_ms + SIGNAL_AUDIO_RETRY_MS;
		}
		descriptor = (struct pollfd){
			.fd = fplinux_audio_client_fd(output.audio),
			.events = POLLIN,
		};
		if (poll(&descriptor, 1, SIGNAL_WAIT_MS) < 0 &&
		    errno != EINTR) {
			fprintf(stderr,
				"clock-signal: cannot wait for alert: %s\n",
				strerror(errno));
			ok = false;
			break;
		}
		fplinux_audio_client_dispatch(output.audio, descriptor.revents);
	}
	if (!had_output ||
	    (!stopped && !output.stream &&
	     !fplinux_session_vibrator_available(&output.vibrator)))
		ok = false;
	if (!fplinux_session_vibrator_close(&output.vibrator)) {
		fprintf(stderr, "clock-signal: cannot close vibration: %s\n",
			strerror(errno));
		ok = false;
	}
	close_audio(&output);
	return ok ? 0 : 1;
}

int main(int argc, char **argv)
{
	struct fplinux_cli cli = {
		.program = "clock-signal",
		.description =
			"Sound a bounded clock alert with optional vibration; SIGTERM stops it.",
	};
	enum fplinux_cli_result parsed = fplinux_cli_parse(&cli, argc, argv);

	if (parsed != FPLINUX_CLI_READY)
		return parsed;
	return run_signal();
}
