// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include "fplinux-cli.h"
#include "fplinux-drm-session.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <poll.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_DRM "/dev/dri/card0"
#define DEFAULT_TTY "/dev/tty0"
#define DEFAULT_FPS 10U
#define DEFAULT_REPEAT 1U
#define MAX_REPEAT 4096U
#define MAX_FPS 30U
#define MAX_HOLD_MS 60000U

enum present_mode {
	PRESENT_MODE_NV16,
	PRESENT_MODE_CPU_RGB565,
};

struct options {
	const char *input_path;
	const char *output_path;
	const char *drm_path;
	const char *tty_path;
	enum present_mode mode;
	unsigned int repeat;
	unsigned int fps;
	unsigned int hold_ms;
};

struct timing_sample {
	uint64_t wall_ns;
	uint64_t user_ns;
	uint64_t system_ns;
	uint64_t max_rss_kib;
	uint64_t minor_faults;
	uint64_t major_faults;
};

struct timing_total {
	uint64_t wall_ns;
	uint64_t user_ns;
	uint64_t system_ns;
	uint64_t max_rss_kib;
	uint64_t minor_faults;
	uint64_t major_faults;
	unsigned int calls;
};

struct run_stats {
	struct timing_total conversion;
	struct timing_total present;
	struct timing_total whole;
	unsigned int completed;
};

enum present_option {
	OPTION_INPUT,
	OPTION_MODE,
	OPTION_REPEAT,
	OPTION_FPS,
	OPTION_HOLD_MS,
	OPTION_DRM,
	OPTION_TTY,
	OPTION_OUTPUT,
};

static volatile sig_atomic_t stop_signal;

static void request_stop(int signal_number)
{
	stop_signal = signal_number;
}

static int install_signal_handlers(void)
{
	struct sigaction action;

	memset(&action, 0, sizeof(action));
	action.sa_handler = request_stop;
	sigemptyset(&action.sa_mask);
	if (sigaction(SIGINT, &action, NULL) < 0 ||
	    sigaction(SIGTERM, &action, NULL) < 0)
		return -1;
	return 0;
}

static const char *mode_name(enum present_mode mode)
{
	return mode == PRESENT_MODE_CPU_RGB565 ? "cpu-rgb565" : "nv16";
}

static const char *parse_option(size_t option, const char *value, void *data)
{
	struct options *options = data;

	switch (option) {
	case OPTION_INPUT:
		options->input_path = value;
		break;
	case OPTION_MODE:
		if (!strcmp(value, "nv16"))
			options->mode = PRESENT_MODE_NV16;
		else if (!strcmp(value, "cpu-rgb565"))
			options->mode = PRESENT_MODE_CPU_RGB565;
		else
			return "--mode must be nv16 or cpu-rgb565";
		break;
	case OPTION_REPEAT:
		if (!fplinux_cli_unsigned(value, 1, MAX_REPEAT,
					  &options->repeat))
			return "--repeat must be an integer from 1 to 4096";
		break;
	case OPTION_FPS:
		if (!fplinux_cli_unsigned(value, 1, MAX_FPS, &options->fps))
			return "--fps must be an integer from 1 to 30";
		break;
	case OPTION_HOLD_MS:
		if (!fplinux_cli_unsigned(value, 0, MAX_HOLD_MS,
					  &options->hold_ms))
			return "--hold-ms must be an integer from 0 to 60000";
		break;
	case OPTION_DRM:
		options->drm_path = value;
		break;
	case OPTION_TTY:
		options->tty_path = value;
		break;
	case OPTION_OUTPUT:
		options->output_path = value;
		break;
	}
	return NULL;
}

static enum fplinux_cli_result parse_options(int argc, char **argv,
					     struct options *options)
{
	struct fplinux_cli_option arguments[] = {
		[OPTION_INPUT] = { .name = "input",
				   .metavar = "NV16",
				   .help = "NV16 input frame",
				   .flags = FPLINUX_CLI_REQUIRED |
					    FPLINUX_CLI_REPEAT },
		[OPTION_MODE] = { .name = "mode",
				  .metavar = "nv16|cpu-rgb565",
				  .help = "presentation path (default: nv16)",
				  .flags = FPLINUX_CLI_REPEAT },
		[OPTION_REPEAT] = { .name = "repeat",
				    .metavar = "N",
				    .help = "completed frames, 1..4096 (default: 1)",
				    .flags = FPLINUX_CLI_REPEAT },
		[OPTION_FPS] = { .name = "fps",
				 .metavar = "N",
				 .help = "absolute-deadline pace, 1..30 (default: 10)",
				 .flags = FPLINUX_CLI_REPEAT },
		[OPTION_HOLD_MS] = { .name = "hold-ms",
				     .metavar = "N",
				     .help = "hold final frame, 0..60000 (default: 0)",
				     .flags = FPLINUX_CLI_REPEAT },
		[OPTION_DRM] = { .name = "drm",
				 .metavar = "PATH",
				 .help = "DRM device (default: /dev/dri/card0)",
				 .flags = FPLINUX_CLI_REPEAT },
		[OPTION_TTY] = { .name = "tty",
				 .metavar = "PATH",
				 .help = "console tty (default: /dev/tty0)",
				 .flags = FPLINUX_CLI_REPEAT },
		[OPTION_OUTPUT] = { .name = "output",
				    .metavar = "RGB565",
				    .help = "optional CPU-converted raw output path",
				    .flags = FPLINUX_CLI_REPEAT },
	};
	struct fplinux_cli cli = {
		.program = argv[0],
		.description =
			"Present NV16 frames through DRM atomic commits.",
		.options = arguments,
		.option_count = sizeof(arguments) / sizeof(arguments[0]),
		.parse_option = parse_option,
		.data = options,
	};
	enum fplinux_cli_result result;

	*options = (struct options){
		.drm_path = DEFAULT_DRM,
		.tty_path = DEFAULT_TTY,
		.mode = PRESENT_MODE_NV16,
		.repeat = DEFAULT_REPEAT,
		.fps = DEFAULT_FPS,
	};
	result = fplinux_cli_parse(&cli, argc, argv);
	if (result != FPLINUX_CLI_READY)
		return result;
	if (options->output_path && options->mode != PRESENT_MODE_CPU_RGB565)
		return fplinux_cli_error(&cli,
					 "--output requires --mode cpu-rgb565");
	return FPLINUX_CLI_READY;
}

static void set_errno_error(char *error, size_t error_size, const char *message)
{
	if (error_size)
		snprintf(error, error_size, "%s: %s", message, strerror(errno));
}

static void set_message(char *error, size_t error_size, const char *message)
{
	if (error_size)
		snprintf(error, error_size, "%s", message);
}

static int read_exact_frame(const char *path, size_t frame_bytes,
			    uint8_t **frame, char *error, size_t error_size)
{
	struct stat status;
	uint8_t extra;
	uint8_t *buffer = NULL;
	size_t offset = 0;
	ssize_t extra_count;
	int descriptor = -1;
	int saved_errno;

	*frame = NULL;
	descriptor = open(path, O_RDONLY | O_CLOEXEC);
	if (descriptor < 0) {
		set_errno_error(error, error_size, "cannot open NV16 input");
		return -1;
	}
	if (fstat(descriptor, &status) < 0) {
		set_errno_error(error, error_size, "cannot inspect NV16 input");
		goto fail;
	}
	if (!S_ISREG(status.st_mode) || status.st_size != (off_t)frame_bytes) {
		errno = EINVAL;
		snprintf(error, error_size,
			 "input must be a regular %zu-byte NV16 file",
			 frame_bytes);
		goto fail;
	}
	buffer = malloc(frame_bytes);
	if (!buffer) {
		set_errno_error(error, error_size,
				"cannot allocate NV16 input");
		goto fail;
	}
	while (offset < frame_bytes) {
		ssize_t count =
			read(descriptor, buffer + offset, frame_bytes - offset);

		if (count < 0) {
			if (errno == EINTR && !stop_signal)
				continue;
			set_errno_error(error, error_size,
					"cannot read NV16 input");
			goto fail;
		}
		if (count == 0) {
			errno = EIO;
			set_message(error, error_size,
				    "NV16 input became shorter while reading");
			goto fail;
		}
		offset += (size_t)count;
	}
	do {
		extra_count = read(descriptor, &extra, 1);
	} while (extra_count < 0 && errno == EINTR && !stop_signal);
	if (extra_count != 0) {
		if (extra_count > 0)
			errno = EIO;
		set_errno_error(error, error_size,
				"NV16 input changed while reading");
		goto fail;
	}
	if (close(descriptor) < 0) {
		descriptor = -1;
		set_errno_error(error, error_size, "cannot close NV16 input");
		goto fail;
	}
	*frame = buffer;
	return 0;

fail:
	saved_errno = errno;
	if (descriptor >= 0)
		(void)close(descriptor);
	free(buffer);
	errno = saved_errno;
	return -1;
}

static int write_exact_output(const char *path, const uint8_t *frame,
			      size_t frame_bytes, char *error,
			      size_t error_size)
{
	size_t offset = 0;
	int descriptor;
	int saved_errno;

	descriptor = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (descriptor < 0) {
		set_errno_error(error, error_size, "cannot open RGB565 output");
		return -1;
	}
	while (offset < frame_bytes) {
		ssize_t count =
			write(descriptor, frame + offset, frame_bytes - offset);

		if (count < 0) {
			if (errno == EINTR && !stop_signal)
				continue;
			set_errno_error(error, error_size,
					"cannot write RGB565 output");
			goto fail;
		}
		if (count == 0) {
			errno = EIO;
			set_message(error, error_size,
				    "short write to RGB565 output");
			goto fail;
		}
		offset += (size_t)count;
	}
	if (close(descriptor) < 0) {
		set_errno_error(error, error_size,
				"cannot close RGB565 output");
		return -1;
	}
	return 0;

fail:
	saved_errno = errno;
	(void)close(descriptor);
	errno = saved_errno;
	return -1;
}

static uint64_t timespec_ns(const struct timespec *value)
{
	return (uint64_t)value->tv_sec * 1000000000ULL +
	       (uint64_t)value->tv_nsec;
}

static uint64_t timeval_ns(const struct timeval *value)
{
	return (uint64_t)value->tv_sec * 1000000000ULL +
	       (uint64_t)value->tv_usec * 1000ULL;
}

static int timing_start(struct timing_sample *sample)
{
	struct rusage usage;
	struct timespec wall;

	if (getrusage(RUSAGE_SELF, &usage) < 0 ||
	    clock_gettime(CLOCK_MONOTONIC, &wall) < 0)
		return -1;
	sample->wall_ns = timespec_ns(&wall);
	sample->user_ns = timeval_ns(&usage.ru_utime);
	sample->system_ns = timeval_ns(&usage.ru_stime);
	sample->max_rss_kib = (uint64_t)usage.ru_maxrss;
	sample->minor_faults = (uint64_t)usage.ru_minflt;
	sample->major_faults = (uint64_t)usage.ru_majflt;
	return 0;
}

static int timing_stop(struct timing_sample *sample)
{
	struct rusage usage;
	struct timespec wall;

	if (clock_gettime(CLOCK_MONOTONIC, &wall) < 0 ||
	    getrusage(RUSAGE_SELF, &usage) < 0)
		return -1;
	sample->wall_ns = timespec_ns(&wall);
	sample->user_ns = timeval_ns(&usage.ru_utime);
	sample->system_ns = timeval_ns(&usage.ru_stime);
	sample->max_rss_kib = (uint64_t)usage.ru_maxrss;
	sample->minor_faults = (uint64_t)usage.ru_minflt;
	sample->major_faults = (uint64_t)usage.ru_majflt;
	return 0;
}

static int add_u64(uint64_t *total, uint64_t value)
{
	if (UINT64_MAX - *total < value) {
		errno = EOVERFLOW;
		return -1;
	}
	*total += value;
	return 0;
}

static int timing_add(struct timing_total *total,
		      const struct timing_sample *start,
		      const struct timing_sample *stop)
{
	if (stop->wall_ns < start->wall_ns || stop->user_ns < start->user_ns ||
	    stop->system_ns < start->system_ns ||
	    stop->minor_faults < start->minor_faults ||
	    stop->major_faults < start->major_faults) {
		errno = EIO;
		return -1;
	}
	if (add_u64(&total->wall_ns, stop->wall_ns - start->wall_ns) < 0 ||
	    add_u64(&total->user_ns, stop->user_ns - start->user_ns) < 0 ||
	    add_u64(&total->system_ns, stop->system_ns - start->system_ns) <
		    0 ||
	    add_u64(&total->minor_faults,
		    stop->minor_faults - start->minor_faults) < 0 ||
	    add_u64(&total->major_faults,
		    stop->major_faults - start->major_faults) < 0)
		return -1;
	if (stop->max_rss_kib > total->max_rss_kib)
		total->max_rss_kib = stop->max_rss_kib;
	++total->calls;
	return 0;
}

static int round_thousandths(int value)
{
	if (value >= 0)
		return (value + 500) / 1000;
	return -((-value + 500) / 1000);
}

static uint8_t clamp_u8(int value)
{
	if (value < 0)
		return 0;
	if (value > 255)
		return 255;
	return (uint8_t)value;
}

static void convert_pixel(uint8_t y, int cb, int cr, uint8_t *output)
{
	uint8_t red = clamp_u8((int)y + round_thousandths(1402 * cr));
	uint8_t green =
		clamp_u8((int)y + round_thousandths(-344 * cb - 714 * cr));
	uint8_t blue = clamp_u8((int)y + round_thousandths(1772 * cb));
	uint16_t pixel = (uint16_t)((red >> 3) << 11) |
			 (uint16_t)((green >> 2) << 5) | (uint16_t)(blue >> 3);

	output[0] = (uint8_t)pixel;
	output[1] = (uint8_t)(pixel >> 8);
}

static void convert_nv16_to_rgb565(const uint8_t *input, uint8_t *output,
				   size_t plane_bytes)
{
	const uint8_t *chroma = input + plane_bytes;
	size_t pixel;

	for (pixel = 0; pixel < plane_bytes; pixel += 2) {
		int cb = (int)chroma[pixel] - 128;
		int cr = (int)chroma[pixel + 1] - 128;

		convert_pixel(input[pixel], cb, cr, output + pixel * 2);
		convert_pixel(input[pixel + 1], cb, cr,
			      output + (pixel + 1) * 2);
	}
}

static int sleep_until(struct fplinux_drm_session *session,
		       uint64_t deadline_ns)
{
	struct timespec deadline = {
		.tv_sec = (time_t)(deadline_ns / 1000000000ULL),
		.tv_nsec = (long)(deadline_ns % 1000000000ULL),
	};
	int result;

	do {
		result = fplinux_drm_session_wait_until(session, &deadline) ?
				 0 :
				 errno;
	} while (result == EINTR && !stop_signal);
	if (result) {
		errno = result;
		return stop_signal && result == EINTR ? 1 : -1;
	}
	return 0;
}

static int hold_last_frame(struct fplinux_drm_session *session,
			   unsigned int hold_ms)
{
	struct timespec now;
	uint64_t deadline;

	if (!hold_ms || stop_signal)
		return stop_signal ? 1 : 0;
	if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
		return -1;
	deadline = timespec_ns(&now) + (uint64_t)hold_ms * 1000000ULL;
	return sleep_until(session, deadline);
}

static int run_series(struct fplinux_drm_session *session,
		      const struct options *options, const uint8_t *nv16,
		      uint8_t *rgb565, struct run_stats *stats)
{
	struct timing_sample loop_start;
	struct timing_sample loop_stop;
	unsigned int iteration;
	int status = 0;

	memset(stats, 0, sizeof(*stats));
	if (timing_start(&loop_start) < 0)
		return -1;
	for (iteration = 0; iteration < options->repeat; ++iteration) {
		unsigned int page = 1U - session->shown_page;
		struct timing_sample start;
		struct timing_sample stop;
		const uint8_t *pixels;
		int sleep_status;

		if (stop_signal) {
			status = 1;
			break;
		}
		if (iteration) {
			uint64_t deadline =
				loop_start.wall_ns +
				((uint64_t)iteration * 1000000000ULL) /
					options->fps;

			sleep_status = sleep_until(session, deadline);
			if (sleep_status) {
				status = sleep_status;
				break;
			}
		}
		if (options->mode == PRESENT_MODE_CPU_RGB565) {
			if (timing_start(&start) < 0)
				return -1;
			convert_nv16_to_rgb565(nv16, rgb565,
					       session->page_bytes / 2);
			if (timing_stop(&stop) < 0 ||
			    timing_add(&stats->conversion, &start, &stop) < 0)
				return -1;
			pixels = rgb565;
		} else {
			pixels = nv16;
		}
		if (stop_signal) {
			status = 1;
			break;
		}
		if (!fplinux_drm_session_dispatch(session))
			return -1;
		while (!session->active && !stop_signal) {
			struct pollfd event = {
				.fd = fplinux_drm_session_fd(session),
				.events = POLLIN,
			};

			if (poll(&event, 1, -1) < 0) {
				if (errno == EINTR)
					continue;
				return -1;
			}
			if (!fplinux_drm_session_dispatch(session))
				return -1;
		}
		if (stop_signal) {
			status = 1;
			break;
		}
		memcpy(session->mapping + page * session->page_bytes, pixels,
		       session->page_bytes);
		if (timing_start(&start) < 0)
			return -1;
		if (!fplinux_drm_session_present(session, page)) {
			if (errno == EINTR && stop_signal) {
				status = 1;
				break;
			}
			return -1;
		}
		if (timing_stop(&stop) < 0 ||
		    timing_add(&stats->present, &start, &stop) < 0)
			return -1;
		++stats->completed;
	}
	if (timing_stop(&loop_stop) < 0 ||
	    timing_add(&stats->whole, &loop_start, &loop_stop) < 0)
		return -1;
	return status;
}

static void print_timing(const char *stage, const struct timing_total *timing)
{
	uint64_t divisor = timing->calls ? timing->calls : 1;

	printf("timing stage=%s calls=%u wall_ns=%" PRIu64
	       " wall_avg_ns=%" PRIu64 " ru_user_ns=%" PRIu64
	       " ru_user_avg_ns=%" PRIu64 " ru_system_ns=%" PRIu64
	       " ru_system_avg_ns=%" PRIu64 " ru_maxrss_kib=%" PRIu64
	       " ru_minflt=%" PRIu64 " ru_majflt=%" PRIu64 "\n",
	       stage, timing->calls, timing->wall_ns, timing->wall_ns / divisor,
	       timing->user_ns, timing->user_ns / divisor, timing->system_ns,
	       timing->system_ns / divisor, timing->max_rss_kib,
	       timing->minor_faults, timing->major_faults);
}

static void print_stats(const struct options *options,
			const struct run_stats *stats, bool interrupted)
{
	printf("result mode=%s completed=%u requested=%u fps=%u hold_ms=%u "
	       "interrupted=%s\n",
	       mode_name(options->mode), stats->completed, options->repeat,
	       options->fps, options->hold_ms, interrupted ? "yes" : "no");
	print_timing("codec_convert", &stats->conversion);
	print_timing("atomic_commit", &stats->present);
	print_timing("whole_loop", &stats->whole);
}

int main(int argc, char **argv)
{
	struct fplinux_drm_session session;
	struct options options;
	struct run_stats stats;
	uint8_t *nv16 = NULL;
	uint8_t *rgb565 = NULL;
	size_t frame_bytes = 0;
	char error[256] = { 0 };
	bool session_open = false;
	bool have_stats = false;
	bool interrupted = false;
	bool failed = false;
	int run_status;
	enum fplinux_cli_result parse_result;

	parse_result = parse_options(argc, argv, &options);
	if (parse_result != FPLINUX_CLI_READY)
		return parse_result;
	if (install_signal_handlers() < 0) {
		perror("fplinux-present: cannot install signal handlers");
		return EXIT_FAILURE;
	}
	if (stop_signal) {
		interrupted = true;
		goto cleanup;
	}
	if (!fplinux_drm_session_open(
		    &session, options.drm_path, options.tty_path,
		    options.mode == PRESENT_MODE_NV16 ? DRM_FORMAT_NV16 :
							DRM_FORMAT_RGB565,
		    error, sizeof(error))) {
		fprintf(stderr, "fplinux-present: %s\n", error);
		failed = true;
		goto cleanup;
	}
	session_open = true;
	frame_bytes = session.page_bytes;
	if (read_exact_frame(options.input_path, frame_bytes, &nv16, error,
			     sizeof(error)) < 0) {
		fprintf(stderr, "fplinux-present: %s\n", error);
		failed = true;
		goto cleanup;
	}
	if (options.mode == PRESENT_MODE_CPU_RGB565) {
		rgb565 = malloc(frame_bytes);
		if (!rgb565) {
			perror("fplinux-present: cannot allocate RGB565 frame");
			failed = true;
			goto cleanup;
		}
	}
	if (stop_signal) {
		interrupted = true;
		goto cleanup;
	}
	run_status = run_series(&session, &options, nv16, rgb565, &stats);
	if (run_status < 0) {
		fprintf(stderr, "fplinux-present: presentation loop: %s\n",
			strerror(errno));
		failed = true;
		goto cleanup;
	}
	have_stats = true;
	interrupted = run_status > 0 || stop_signal;
	if (!interrupted) {
		run_status = hold_last_frame(&session, options.hold_ms);
		if (run_status < 0) {
			fprintf(stderr, "fplinux-present: final hold: %s\n",
				strerror(errno));
			failed = true;
		} else if (run_status > 0) {
			interrupted = true;
		}
	}

cleanup:
	if (session_open && !fplinux_drm_session_close(&session)) {
		fprintf(stderr,
			"fplinux-present: DRM/console restore failed\n");
		failed = true;
	}
	if (!failed && !interrupted && options.output_path &&
	    write_exact_output(options.output_path, rgb565, frame_bytes, error,
			       sizeof(error)) < 0) {
		fprintf(stderr, "fplinux-present: %s\n", error);
		failed = true;
	}
	if (have_stats)
		print_stats(&options, &stats, interrupted);
	free(rgb565);
	free(nv16);
	if (stop_signal)
		return 128 + stop_signal;
	return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
