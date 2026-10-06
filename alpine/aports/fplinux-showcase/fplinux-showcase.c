// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include "armada-scene.h"
#include "showcase-hardware.h"
#include "fplinux-cli.h"
#include "fplinux-drm-session.h"

#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#define SHOWCASE_DRM "/dev/dri/card0"
#define SHOWCASE_TTY "/dev/tty0"
#define SHOWCASE_NANOSECONDS_PER_SECOND 1000000000ULL
#define SHOWCASE_MICROSECONDS_PER_SECOND 1000000ULL
#define SHOWCASE_FRAME_PERIOD_NS \
	(SHOWCASE_NANOSECONDS_PER_SECOND / ARMADA_FRAMES_PER_SECOND)
#define SHOWCASE_CYCLE_DURATION_NS \
	(SHOWCASE_FRAME_PERIOD_NS * ARMADA_DURATION_FRAMES)

struct showcase_options {
	uint64_t runs;
	const char *keypad_led;
};

struct frame_statistics {
	uint64_t started_ns;
	uint64_t render_us_sum;
	uint64_t rendered;
	uint32_t render_us_max;
};

enum showcase_option {
	SHOWCASE_OPTION_RUNS,
	SHOWCASE_OPTION_KEYPAD_LED,
};

static volatile sig_atomic_t stop_requested;

static bool parse_positive_runs(const char *value, uint64_t *runs,
				const char **error)
{
	char *end;
	const char *cursor;
	uint64_t parsed;

	if (value[0] == '\0') {
		*error = "--runs requires a positive decimal count";
		return false;
	}
	for (cursor = value; *cursor; ++cursor)
		if (*cursor < '0' || *cursor > '9') {
			*error = "--runs requires a positive decimal count";
			return false;
		}
	errno = 0;
	parsed = strtoull(value, &end, 10);
	if (errno == ERANGE || end == value || *end != '\0' || parsed == 0 ||
	    parsed > UINT64_MAX / SHOWCASE_CYCLE_DURATION_NS) {
		*error = "--runs count is out of range";
		return false;
	}
	*runs = parsed;
	return true;
}

static const char *parse_showcase_option(size_t option, const char *value,
					 void *data)
{
	struct showcase_options *options = data;
	const char *argument_error;

	switch (option) {
	case SHOWCASE_OPTION_RUNS:
		if (!parse_positive_runs(value, &options->runs,
					 &argument_error))
			return argument_error;
		break;
	case SHOWCASE_OPTION_KEYPAD_LED:
		options->keypad_led = value;
		break;
	}
	return NULL;
}

static enum fplinux_cli_result parse_options(int argc, char **argv,
					     struct showcase_options *parsed)
{
	struct fplinux_cli_option options[] = {
		{
			.name = "runs",
			.metavar = "N",
			.help = "run the showcase N times",
		},
		{
			.name = "keypad-led",
			.metavar = "DIR",
			.help = "use the keypad LED brightness directory",
		},
	};
	struct fplinux_cli cli = {
		.program = argv[0],
		.description = "Display the Armada showcase.",
		.options = options,
		.option_count = ARRAY_SIZE(options),
		.parse_option = parse_showcase_option,
		.data = parsed,
	};

	memset(parsed, 0, sizeof(*parsed));
	return fplinux_cli_parse(&cli, argc, argv);
}

static void catch_signal(int signal_number)
{
	stop_requested = signal_number;
}

static bool install_signal_handlers(void)
{
	static const int handled[] = { SIGHUP, SIGINT, SIGQUIT, SIGTERM };
	struct sigaction action = {
		.sa_handler = catch_signal,
	};
	size_t index;

	sigemptyset(&action.sa_mask);
	for (index = 0; index < ARRAY_SIZE(handled); ++index)
		if (sigaction(handled[index], &action, NULL) < 0)
			return false;
	return true;
}

static uint64_t monotonic_ns(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
		return 0;
	return (uint64_t)now.tv_sec * SHOWCASE_NANOSECONDS_PER_SECOND +
	       (uint64_t)now.tv_nsec;
}

static bool process_usage(uint64_t *cpu_us, long *peak_rss_kib)
{
	struct rusage usage;

	if (getrusage(RUSAGE_SELF, &usage) != 0)
		return false;
	*cpu_us = (uint64_t)usage.ru_utime.tv_sec *
			  SHOWCASE_MICROSECONDS_PER_SECOND +
		  (uint64_t)usage.ru_utime.tv_usec +
		  (uint64_t)usage.ru_stime.tv_sec *
			  SHOWCASE_MICROSECONDS_PER_SECOND +
		  (uint64_t)usage.ru_stime.tv_usec;
	*peak_rss_kib = usage.ru_maxrss;
	return true;
}

static struct timespec nanoseconds_to_timespec(uint64_t nanoseconds)
{
	struct timespec result = {
		.tv_sec =
			(time_t)(nanoseconds / SHOWCASE_NANOSECONDS_PER_SECOND),
		.tv_nsec =
			(long)(nanoseconds % SHOWCASE_NANOSECONDS_PER_SECOND),
	};

	return result;
}

static void update_metrics(const struct frame_statistics *statistics,
			   uint64_t now_ns, struct armada_metrics *metrics)
{
	uint64_t elapsed_us;

	memset(metrics, 0, sizeof(*metrics));
	if (statistics->rendered < 2 || now_ns <= statistics->started_ns)
		return;
	elapsed_us = (now_ns - statistics->started_ns) / 1000U;
	if (elapsed_us == 0)
		return;
	metrics->average_fps_tenths =
		(uint32_t)(statistics->rendered * 10000000ULL / elapsed_us);
	metrics->minimum_fps_tenths =
		statistics->render_us_max ?
			(uint32_t)(10000000ULL / statistics->render_us_max) :
			0;
	metrics->maximum_frame_us = statistics->render_us_max;
	metrics->valid = true;
}

static void print_result(uint64_t runs,
			 const struct frame_statistics *statistics,
			 uint64_t finished_ns, uint64_t started_cpu_us,
			 uint64_t finished_cpu_us, long peak_rss_kib)
{
	uint64_t elapsed_us = 0;
	uint64_t average_fps_tenths = 0;
	uint64_t minimum_fps_tenths = 0;
	uint64_t average_frame_us = 0;
	uint64_t maximum_frame_us = statistics->render_us_max;
	uint64_t cpu_percent_tenths = 0;

	if (finished_ns > statistics->started_ns)
		elapsed_us = (finished_ns - statistics->started_ns) / 1000U;
	if (statistics->rendered && elapsed_us)
		average_fps_tenths = statistics->rendered *
				     (10U * SHOWCASE_MICROSECONDS_PER_SECOND) /
				     elapsed_us;
	if (statistics->render_us_max)
		minimum_fps_tenths = (10U * SHOWCASE_MICROSECONDS_PER_SECOND) /
				     statistics->render_us_max;
	if (statistics->rendered)
		average_frame_us =
			statistics->render_us_sum / statistics->rendered;
	if (elapsed_us && finished_cpu_us >= started_cpu_us)
		cpu_percent_tenths =
			(finished_cpu_us - started_cpu_us) * 1000U / elapsed_us;
	printf("runs=%" PRIu64 " rendered=%" PRIu64 " average_fps=%" PRIu64
	       ".%" PRIu64 " minimum_fps=%" PRIu64 ".%" PRIu64
	       " average_frame_ms=%" PRIu64 ".%" PRIu64
	       " maximum_frame_ms=%" PRIu64 ".%" PRIu64 " cpu_percent=%" PRIu64
	       ".%" PRIu64 " peak_rss_kib=%ld\n",
	       runs, statistics->rendered, average_fps_tenths / 10U,
	       average_fps_tenths % 10U, minimum_fps_tenths / 10U,
	       minimum_fps_tenths % 10U, average_frame_us / 1000U,
	       (average_frame_us / 100U) % 10U, maximum_frame_us / 1000U,
	       (maximum_frame_us / 100U) % 10U, cpu_percent_tenths / 10U,
	       cpu_percent_tenths % 10U, peak_rss_kib);
}

static void copy_frame(struct fplinux_drm_session *display,
		       const uint16_t *pixels, unsigned int page)
{
	uint8_t *destination = display->mapping + page * display->page_bytes;
	unsigned int row;

	for (row = 0; row < display->height; ++row)
		memcpy(destination + (size_t)row * display->stride,
		       pixels + (size_t)row * display->width,
		       (size_t)display->width * sizeof(*pixels));
}

int main(int argc, char **argv)
{
	struct fplinux_drm_session display;
	struct showcase_hardware hardware;
	struct showcase_options options;
	struct frame_statistics statistics = { 0 };
	struct armada_scene *scene = NULL;
	struct fplinux_font font = { 0 };
	uint16_t *pixels = NULL;
	uint64_t frame_limit;
	uint64_t completed_runs = 0;
	uint64_t finished_ns = 0;
	uint64_t started_cpu_us = 0;
	uint64_t finished_cpu_us = 0;
	long peak_rss_kib = 0;
	uint64_t frame_period = SHOWCASE_FRAME_PERIOD_NS;
	uint64_t next_frame = 0;
	bool display_open = false;
	bool hardware_open = false;
	bool success = false;
	bool report_result = false;
	enum fplinux_cli_result parse_result;
	char error[160] = { 0 };

	memset(&display, 0, sizeof(display));
	parse_result = parse_options(argc, argv, &options);
	if (parse_result != FPLINUX_CLI_READY)
		return parse_result;
	frame_limit = options.runs * ARMADA_DURATION_FRAMES;
	if (!install_signal_handlers()) {
		fprintf(stderr,
			"fplinux-showcase: cannot install signal handlers: %s\n",
			strerror(errno));
		return EXIT_FAILURE;
	}
	if (!showcase_hardware_open(&hardware, options.keypad_led, error,
				    sizeof(error))) {
		fprintf(stderr, "fplinux-showcase: %s: %s\n", error,
			strerror(errno));
		hardware_open = true;
		goto cleanup;
	}
	hardware_open = true;
	if (!fplinux_drm_session_open(&display, SHOWCASE_DRM, SHOWCASE_TTY,
				      DRM_FORMAT_RGB565, error,
				      sizeof(error))) {
		fprintf(stderr, "fplinux-showcase: %s\n", error);
		goto cleanup;
	}
	display_open = true;
	if (!fplinux_drm_session_set_active_handler(
		    &display, showcase_hardware_set_display_active,
		    &hardware)) {
		perror("fplinux-showcase: display activation");
		goto cleanup;
	}
	if (display.pages != 2U ||
	    !((display.width == 240U && display.height == 320U) ||
	      (display.width == 128U && display.height == 160U))) {
		fprintf(stderr,
			"fplinux-showcase: expected two-page RGB565 240x320 or 128x160 framebuffer\n");
		goto cleanup;
	}
	if (!fplinux_font_open_default(&font, display.width, error,
				       sizeof(error))) {
		fprintf(stderr, "fplinux-showcase: %s\n", error);
		goto cleanup;
	}
	pixels =
		calloc((size_t)display.width * display.height, sizeof(*pixels));
	if (!pixels) {
		fprintf(stderr,
			"fplinux-showcase: cannot allocate render surface\n");
		goto cleanup;
	}
	scene = armada_scene_create(display.width, display.height, pixels,
				    &font);
	if (!scene) {
		fprintf(stderr, "fplinux-showcase: cannot create scene: %s\n",
			strerror(errno));
		goto cleanup;
	}
	if (!process_usage(&started_cpu_us, &peak_rss_kib)) {
		fprintf(stderr,
			"fplinux-showcase: cannot read process resource usage: %s\n",
			strerror(errno));
		goto cleanup;
	}
	statistics.started_ns = monotonic_ns();
	if (!statistics.started_ns) {
		fprintf(stderr,
			"fplinux-showcase: CLOCK_MONOTONIC is unavailable\n");
		goto cleanup;
	}

	while (!stop_requested) {
		struct armada_metrics metrics;
		struct armada_outputs outputs;
		uint64_t now_ns = monotonic_ns();
		uint64_t desired_frame;
		uint64_t render_started;
		uint64_t render_finished;
		uint64_t render_us;
		unsigned int page;
		struct timespec deadline;
		int sleep_result;
		int key_result;

		if (!now_ns) {
			fprintf(stderr,
				"fplinux-showcase: cannot read CLOCK_MONOTONIC\n");
			goto cleanup;
		}
		if (!fplinux_drm_session_dispatch(&display))
			goto cleanup;
		if (!display.active) {
			usleep(20000);
			continue;
		}
		key_result = showcase_hardware_exit_key_pressed(&hardware);
		if (key_result < 0) {
			fprintf(stderr,
				"fplinux-showcase: cannot read keypad: %s\n",
				strerror(errno));
			goto cleanup;
		}
		if (key_result > 0) {
			success = true;
			break;
		}
		desired_frame = (now_ns - statistics.started_ns) / frame_period;
		if (options.runs && desired_frame >= frame_limit) {
			completed_runs = options.runs;
			success = true;
			break;
		}
		if (desired_frame > next_frame)
			next_frame = desired_frame;
		if (options.runs && next_frame >= frame_limit) {
			completed_runs = options.runs;
			success = true;
			break;
		}
		update_metrics(&statistics, now_ns, &metrics);
		render_started = monotonic_ns();
		armada_scene_render(
			scene, (uint32_t)(next_frame % ARMADA_DURATION_FRAMES),
			&metrics, &outputs);
		page = 1U - display.shown_page;
		copy_frame(&display, pixels, page);
		if (!showcase_hardware_apply_outputs(&hardware, &outputs)) {
			fprintf(stderr,
				"fplinux-showcase: cannot apply synchronized hardware cue: %s\n",
				strerror(errno));
			goto cleanup;
		}
		__sync_synchronize();
		if (!fplinux_drm_session_present(&display, page)) {
			fprintf(stderr,
				"fplinux-showcase: cannot present frame: %s\n",
				strerror(errno));
			goto cleanup;
		}
		render_finished = monotonic_ns();
		render_us = render_finished > render_started ?
				    (render_finished - render_started) / 1000U :
				    0;
		statistics.render_us_sum += render_us;
		++statistics.rendered;
		if (render_us > statistics.render_us_max)
			statistics.render_us_max = render_us > UINT32_MAX ?
							   UINT32_MAX :
							   (uint32_t)render_us;
		++next_frame;
		deadline = nanoseconds_to_timespec(statistics.started_ns +
						   next_frame * frame_period);
		do {
			sleep_result = clock_nanosleep(CLOCK_MONOTONIC,
						       TIMER_ABSTIME, &deadline,
						       NULL);
		} while (sleep_result == EINTR && !stop_requested);
		if (sleep_result != 0 && sleep_result != EINTR) {
			errno = sleep_result;
			fprintf(stderr,
				"fplinux-showcase: frame clock failed: %s\n",
				strerror(errno));
			goto cleanup;
		}
	}
	if (stop_requested)
		success = true;
	if (success) {
		if (!process_usage(&finished_cpu_us, &peak_rss_kib)) {
			fprintf(stderr,
				"fplinux-showcase: cannot read process resource usage: %s\n",
				strerror(errno));
			success = false;
		} else {
			finished_ns = monotonic_ns();
		}
		if (success && !finished_ns) {
			fprintf(stderr,
				"fplinux-showcase: cannot read CLOCK_MONOTONIC\n");
			success = false;
		} else if (success && !options.runs) {
			completed_runs = (finished_ns - statistics.started_ns) /
					 SHOWCASE_CYCLE_DURATION_NS;
		} else if (success && !completed_runs) {
			completed_runs = (finished_ns - statistics.started_ns) /
					 SHOWCASE_CYCLE_DURATION_NS;
			if (completed_runs > options.runs)
				completed_runs = options.runs;
		}
		report_result = success;
	}

cleanup:
	armada_scene_destroy(scene);
	fplinux_font_close(&font);
	free(pixels);
	if (display_open)
		fplinux_drm_session_set_active_handler(&display, NULL, NULL);
	if (hardware_open && !showcase_hardware_close(&hardware)) {
		fprintf(stderr,
			"fplinux-showcase: hardware restore was incomplete\n");
		success = false;
	}
	if (display_open && !fplinux_drm_session_close(&display)) {
		fprintf(stderr,
			"fplinux-showcase: display restore was incomplete\n");
		success = false;
	}
	if (success && report_result)
		print_result(completed_runs, &statistics, finished_ns,
			     started_cpu_us, finished_cpu_us, peak_rss_kib);
	return success ? EXIT_SUCCESS : EXIT_FAILURE;
}
