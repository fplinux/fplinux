/* SPDX-License-Identifier: GPL-2.0-only */
#define _POSIX_C_SOURCE 200809L

#include "display-test-pattern.h"
#include "display-test-gpu.h"
#include "fplinux-cli.h"
#include "fplinux-drm-session.h"

#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_FRAMES 65536U
#define MAX_DURATION_MS 60000U
#define MAX_FPS 120U

struct options {
	enum fplinux_display_test_pattern pattern;
	const char *drm_path;
	const char *tty_path;
	unsigned int frames;
	unsigned int duration_ms;
	unsigned int fps;
	bool gpu;
	bool verify;
};

enum option {
	OPTION_PATTERN,
	OPTION_FRAMES,
	OPTION_DURATION_MS,
	OPTION_FPS,
	OPTION_DRM,
	OPTION_TTY,
	OPTION_RENDERER,
	OPTION_VERIFY,
};

struct run_stats {
	uint64_t start_ns;
	uint64_t end_ns;
	unsigned int completed;
};

struct frame_sample {
	unsigned int id;
	uint64_t draw_start_ns;
	uint64_t submit_ns;
	bool pending;
};

static volatile sig_atomic_t stop_signal;

static void request_stop(int signal_number)
{
	stop_signal = signal_number;
}

static bool install_signal_handlers(void)
{
	struct sigaction action = { .sa_handler = request_stop };

	sigemptyset(&action.sa_mask);
	if (sigaction(SIGINT, &action, NULL) < 0 ||
	    sigaction(SIGTERM, &action, NULL) < 0)
		return false;
	/* A closed output pipe must still restore the display session. */
	action.sa_handler = SIG_IGN;
	return sigaction(SIGPIPE, &action, NULL) == 0;
}

static const char *parse_option(size_t option, const char *value, void *data)
{
	struct options *options = data;

	switch (option) {
	case OPTION_PATTERN:
		if (!strcmp(value, "geometry"))
			options->pattern = FPLINUX_DISPLAY_TEST_GEOMETRY;
		else if (!strcmp(value, "gray"))
			options->pattern = FPLINUX_DISPLAY_TEST_GRAY;
		else if (!strcmp(value, "ids"))
			options->pattern = FPLINUX_DISPLAY_TEST_IDS;
		else if (!strcmp(value, "motion"))
			options->pattern = FPLINUX_DISPLAY_TEST_MOTION;
		else
			return "--pattern must be geometry, gray, ids or motion";
		break;
	case OPTION_FRAMES:
		if (!fplinux_cli_unsigned(value, 1, MAX_FRAMES,
					  &options->frames))
			return "--frames must be an integer from 1 to 65536";
		break;
	case OPTION_DURATION_MS:
		if (!fplinux_cli_unsigned(value, 1, MAX_DURATION_MS,
					  &options->duration_ms))
			return "--duration-ms must be an integer from 1 to 60000";
		break;
	case OPTION_FPS:
		if (!fplinux_cli_unsigned(value, 0, MAX_FPS, &options->fps))
			return "--fps must be an integer from 0 to 120";
		break;
	case OPTION_DRM:
		options->drm_path = value;
		break;
	case OPTION_TTY:
		options->tty_path = value;
		break;
	case OPTION_RENDERER:
		if (!strcmp(value, "gpu"))
			options->gpu = true;
		else if (!strcmp(value, "software"))
			options->gpu = false;
		else
			return "--renderer must be software or gpu";
		break;
	case OPTION_VERIFY:
		options->verify = true;
		break;
	}
	return NULL;
}

static enum fplinux_cli_result parse_options(int argc, char **argv,
					     struct options *options)
{
	struct fplinux_cli_option arguments[] = {
		[OPTION_PATTERN] = {
			.name = "pattern",
			.metavar = "geometry|gray|ids|motion",
			.help = "native RGB565 pattern (default: geometry)",
		},
		[OPTION_FRAMES] = {
			.name = "frames",
			.metavar = "N",
			.help = "completed frame limit, 1..65536 (default: 120)",
		},
		[OPTION_DURATION_MS] = {
			.name = "duration-ms",
			.metavar = "N",
			.help = "wall-clock limit, 1..60000 ms (default: 3000)",
		},
		[OPTION_FPS] = {
			.name = "fps",
			.metavar = "N",
			.help = "absolute pace, 0..120; 0 is unpaced (default: 40)",
		},
		[OPTION_DRM] = {
			.name = "drm",
			.metavar = "PATH",
			.help = "DRM device (default: unique connected display)",
		},
		[OPTION_TTY] = {
			.name = "tty",
			.metavar = "PATH",
			.help = "console tty (default: /dev/tty0)",
		},
		[OPTION_RENDERER] = {
			.name = "renderer",
			.metavar = "software|gpu",
			.help = "pattern renderer (default: software)",
		},
		[OPTION_VERIFY] = {
			.name = "verify",
			.help = "compare GPU pixels with the CPU reference before presentation; includes CPU readback cost",
		},
	};
	struct fplinux_cli cli = {
		.program = argv[0],
		.description =
			"Display bounded test patterns with completed-frame timings. "
			"Geometry and gray present once and hold until the duration limit. "
			"IDs use 16 least-significant-first bits repeated at top, middle and bottom; "
			"each bit has complementary light/dark halves. "
			"Gray stays constant. Motion advances eight pixels per frame "
			"and can alias with camera rolling shutter. "
			"An in-flight presentation may finish after the wall-clock limit.",
		.options = arguments,
		.option_count = sizeof(arguments) / sizeof(arguments[0]),
		.parse_option = parse_option,
		.data = options,
	};
	enum fplinux_cli_result result;

	*options = (struct options){
		.pattern = FPLINUX_DISPLAY_TEST_GEOMETRY,
		.frames = 120,
		.duration_ms = 3000,
		.fps = 40,
	};
	result = fplinux_cli_parse(&cli, argc, argv);
	if (result == FPLINUX_CLI_READY && options->verify && !options->gpu)
		return fplinux_cli_error(&cli,
					 "--verify requires --renderer gpu");
	return result;
}

static bool monotonic_ns(uint64_t *value)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
		return false;
	*value = (uint64_t)now.tv_sec * 1000000000ULL + now.tv_nsec;
	return true;
}

static bool valid_layout(const struct fplinux_drm_session *session)
{
	if (!session->mapping || session->format != DRM_FORMAT_RGB565 ||
	    !session->width || !session->height || session->pages < 2 ||
	    session->shown_page >= session->pages ||
	    SIZE_MAX / session->width < 2 ||
	    session->stride < (size_t)session->width * 2 ||
	    session->stride > session->page_bytes / session->height ||
	    session->page_bytes > session->size / session->pages) {
		errno = EINVAL;
		return false;
	}
	return true;
}

static bool pace_frame(struct fplinux_drm_session *session,
		       uint64_t deadline_ns)
{
	struct timespec deadline = {
		.tv_sec = (time_t)(deadline_ns / 1000000000ULL),
		.tv_nsec = (long)(deadline_ns % 1000000000ULL),
	};

	while (!fplinux_drm_session_wait_until(session, &deadline)) {
		if (errno != EINTR || stop_signal)
			return false;
	}
	return true;
}

/* VT release must not turn a short diagnostic run into an indefinite wait. */
static bool wait_for_display(struct fplinux_drm_session *session,
			     uint64_t end_ns, uint64_t *now_ns)
{
	for (;;) {
		struct pollfd event = {
			.fd = fplinux_drm_session_fd(session),
			.events = POLLIN,
		};
		int timeout_ms;

		if (!fplinux_drm_session_dispatch(session) ||
		    !monotonic_ns(now_ns))
			return false;
		if (stop_signal || *now_ns >= end_ns || session->active)
			return true;
		timeout_ms = (int)((end_ns - *now_ns + 999999ULL) / 1000000ULL);
		if (poll(&event, 1, timeout_ms) < 0 && errno != EINTR)
			return false;
	}
}

static bool complete_frame(struct frame_sample *sample, struct run_stats *stats)
{
	uint64_t complete_ns;

	if (!sample->pending)
		return true;
	if (!monotonic_ns(&complete_ns))
		return false;
	++stats->completed;
	sample->pending = false;
	return printf("%u,%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n", sample->id,
		      sample->draw_start_ns, sample->submit_ns,
		      complete_ns) >= 0;
}

static bool run_frames(struct fplinux_drm_session *session,
		       const struct options *options,
		       struct fplinux_display_test_gpu *gpu,
		       struct run_stats *stats)
{
	uint64_t end_ns;
	uint64_t now_ns;
	struct frame_sample sample = { 0 };
	int saved_errno;
	bool static_pattern = options->pattern ==
				      FPLINUX_DISPLAY_TEST_GEOMETRY ||
			      options->pattern == FPLINUX_DISPLAY_TEST_GRAY;

	if (!monotonic_ns(&stats->start_ns))
		return false;
	end_ns = stats->start_ns + (uint64_t)options->duration_ms * 1000000ULL;
	printf("frame_id,draw_start_ns,submit_ns,complete_ns\n");
	for (unsigned int frame_id = 0; frame_id < options->frames;
	     ++frame_id) {
		uint64_t submit_ns;
		unsigned int page;

		if (stop_signal)
			break;
		if (frame_id && options->fps) {
			uint64_t deadline_ns =
				stats->start_ns + (uint64_t)frame_id *
							  1000000000ULL /
							  options->fps;

			if (deadline_ns > end_ns)
				deadline_ns = end_ns;
			if (!pace_frame(session, deadline_ns)) {
				if (stop_signal)
					break;
				goto failed;
			}
		}
		if (!wait_for_display(session, end_ns, &now_ns))
			goto failed;
		if (stop_signal || now_ns >= end_ns)
			break;
		page = (session->shown_page + 1U) % session->pages;
		if (gpu) {
			/* Submit scene work while the previous SPI transfer runs.
			 * Shared output waits before reusing its display page.
			 */
			if (!fplinux_display_test_gpu_draw(
				    gpu, options->pattern, frame_id) ||
			    (options->verify &&
			     !fplinux_display_test_gpu_verify(
				     gpu, options->pattern, frame_id)))
				goto failed;
			if (!complete_frame(&sample, stats))
				goto failed;
		} else if (!fplinux_display_test_draw(
				   session->mapping +
					   page * session->page_bytes,
				   session->page_bytes, session->stride,
				   session->width, session->height,
				   options->pattern, frame_id)) {
			goto failed;
		}
		if (!monotonic_ns(&submit_ns))
			goto failed;
		if (stop_signal)
			break;
		if (!(gpu ? fplinux_display_test_gpu_present(gpu) :
			    fplinux_drm_session_present(session, page))) {
			if (stop_signal)
				break;
			goto failed;
		}
		sample = (struct frame_sample){
			.id = frame_id,
			.draw_start_ns = now_ns,
			.submit_ns = submit_ns,
			.pending = true,
		};
		if (!gpu && !complete_frame(&sample, stats))
			goto failed;
		if (static_pattern) {
			if (!fplinux_drm_session_wait_present(session) ||
			    !complete_frame(&sample, stats))
				goto failed;
			if (!stop_signal && !pace_frame(session, end_ns)) {
				if (!stop_signal)
					goto failed;
			}
			break;
		}
	}
	if (!fplinux_drm_session_wait_present(session) ||
	    !complete_frame(&sample, stats))
		goto failed;
	return monotonic_ns(&stats->end_ns);
failed:
	saved_errno = errno;
	/* A later rendering error must not discard an already queued frame. */
	if (fplinux_drm_session_wait_present(session))
		complete_frame(&sample, stats);
	monotonic_ns(&stats->end_ns);
	errno = saved_errno;
	return false;
}

int main(int argc, char **argv)
{
	struct fplinux_drm_session session;
	struct fplinux_display_test_gpu *gpu = NULL;
	struct options options;
	struct run_stats stats = { 0 };
	char error[256] = { 0 };
	bool failed = false;
	enum fplinux_cli_result result;

	result = parse_options(argc, argv, &options);
	if (result != FPLINUX_CLI_READY)
		return result;
	if (!install_signal_handlers()) {
		perror("fplinux-display-test: cannot install signal handlers");
		return EXIT_FAILURE;
	}
	if (stop_signal)
		return 128 + stop_signal;
	if (!fplinux_drm_session_open(&session, options.drm_path,
				      options.tty_path, DRM_FORMAT_RGB565,
				      error, sizeof(error))) {
		fprintf(stderr, "fplinux-display-test: %s\n", error);
		return stop_signal ? 128 + stop_signal : EXIT_FAILURE;
	}
	if (!valid_layout(&session)) {
		perror("fplinux-display-test: presentation loop");
		failed = true;
		goto close;
	}
	if (options.gpu) {
		gpu = fplinux_display_test_gpu_open(&session, error,
						    sizeof(error));
		if (!gpu) {
			if (!stop_signal)
				fprintf(stderr, "fplinux-display-test: %s\n",
					error);
			failed = true;
			goto close;
		}
	}
	fprintf(stderr, "fplinux-display-test: renderer=%s%s\n",
		options.gpu ? "gpu" : "software",
		options.verify ? " verify=yes" : "");
	if (!run_frames(&session, &options, gpu, &stats)) {
		if (!stop_signal) {
			const char *detail =
				gpu ? fplinux_display_test_gpu_error(gpu) : "";

			if (*detail)
				fprintf(stderr, "fplinux-display-test: %s\n",
					detail);
			else
				perror("fplinux-display-test: presentation loop");
			failed = true;
		}
	}
close:
	if (!fplinux_display_test_gpu_close(gpu)) {
		perror("fplinux-display-test: GPU cleanup");
		failed = true;
	}
	if (!fplinux_drm_session_close(&session)) {
		fprintf(stderr,
			"fplinux-display-test: DRM/console restore failed\n");
		failed = true;
	}
	if (stats.end_ns)
		printf("summary completed=%u elapsed_ns=%" PRIu64
		       " interrupted=%s\n",
		       stats.completed, stats.end_ns - stats.start_ns,
		       stop_signal ? "yes" : "no");
	if (fflush(stdout) == EOF) {
		perror("fplinux-display-test: frame output");
		failed = true;
	}
	return stop_signal ? 128 + stop_signal :
	       failed	   ? EXIT_FAILURE :
			     EXIT_SUCCESS;
}
