// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include "fplinux-fb-session.h"
#include "ums9117-present.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/fb.h>
#include <signal.h>
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

#define DEFAULT_FRAMEBUFFER "/dev/fb0"
#define DEFAULT_TTY "/dev/tty0"
#define DEFAULT_FPS 10U
#define DEFAULT_REPEAT 1U
#define MAX_REPEAT 4096U
#define MAX_FPS 30U
#define MAX_HOLD_MS 60000U

_Static_assert(sizeof(struct ums9117_present) == 32,
	       "unexpected UMS9117 present ABI size");
_Static_assert(offsetof(struct ums9117_present, pixels) == 0,
	       "unexpected UMS9117 present pixels offset");
_Static_assert(offsetof(struct ums9117_present, format) == 8,
	       "unexpected UMS9117 present format offset");
_Static_assert(offsetof(struct ums9117_present, bytes) == 12,
	       "unexpected UMS9117 present bytes offset");
_Static_assert(offsetof(struct ums9117_present, sequence) == 16,
	       "unexpected UMS9117 present sequence offset");
_Static_assert(offsetof(struct ums9117_present, transfer_ns) == 24,
	       "unexpected UMS9117 present transfer offset");

enum present_mode {
	PRESENT_MODE_NV16,
	PRESENT_MODE_CPU_RGB565,
};

struct options {
	const char *input_path;
	const char *output_path;
	const char *framebuffer_path;
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
	uint64_t transfer_total_ns;
	uint64_t transfer_min_ns;
	uint64_t transfer_max_ns;
	uint64_t first_sequence;
	uint64_t last_sequence;
	unsigned int completed;
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

static void usage(FILE *stream)
{
	fprintf(stream,
		"usage: fplinux-present --input NV16 [options]\n"
		"options:\n"
		"  --mode nv16|cpu-rgb565  presentation path (default: nv16)\n"
		"  --repeat N              completed frames, 1..4096 (default: 1)\n"
		"  --fps N                 absolute-deadline pace, 1..30 (default: 10)\n"
		"  --hold-ms N             hold final frame, 0..60000 (default: 0)\n"
		"  --framebuffer PATH      framebuffer (default: /dev/fb0)\n"
		"  --tty PATH              console tty (default: /dev/tty0)\n"
		"  --output RGB565         optional CPU-converted raw output path\n"
		"  --help                  show this text\n");
}

static bool parse_bounded(const char *text, unsigned int minimum,
			  unsigned int maximum, unsigned int *value)
{
	char *end;
	unsigned long parsed;

	errno = 0;
	parsed = strtoul(text, &end, 10);
	if (errno || text[0] == '\0' || text[0] == '-' || end[0] != '\0' ||
	    parsed < minimum || parsed > maximum)
		return false;
	*value = (unsigned int)parsed;
	return true;
}

static bool parse_options(int argc, char **argv, struct options *options)
{
	int index;

	memset(options, 0, sizeof(*options));
	options->framebuffer_path = DEFAULT_FRAMEBUFFER;
	options->tty_path = DEFAULT_TTY;
	options->mode = PRESENT_MODE_NV16;
	options->repeat = DEFAULT_REPEAT;
	options->fps = DEFAULT_FPS;
	for (index = 1; index < argc; index += 2) {
		const char *argument = argv[index];
		const char *value = index + 1 < argc ? argv[index + 1] : NULL;

		if (!strcmp(argument, "--help")) {
			usage(stdout);
			exit(EXIT_SUCCESS);
		}
		if (!value)
			return false;
		if (!strcmp(argument, "--input")) {
			options->input_path = value;
		} else if (!strcmp(argument, "--mode")) {
			if (!strcmp(value, "nv16"))
				options->mode = PRESENT_MODE_NV16;
			else if (!strcmp(value, "cpu-rgb565"))
				options->mode = PRESENT_MODE_CPU_RGB565;
			else
				return false;
		} else if (!strcmp(argument, "--repeat")) {
			if (!parse_bounded(value, 1, MAX_REPEAT,
					   &options->repeat))
				return false;
		} else if (!strcmp(argument, "--fps")) {
			if (!parse_bounded(value, 1, MAX_FPS, &options->fps))
				return false;
		} else if (!strcmp(argument, "--hold-ms")) {
			if (!parse_bounded(value, 0, MAX_HOLD_MS,
					   &options->hold_ms))
				return false;
		} else if (!strcmp(argument, "--framebuffer")) {
			options->framebuffer_path = value;
		} else if (!strcmp(argument, "--tty")) {
			options->tty_path = value;
		} else if (!strcmp(argument, "--output")) {
			options->output_path = value;
		} else {
			return false;
		}
	}
	return options->input_path &&
	       (!options->output_path ||
		options->mode == PRESENT_MODE_CPU_RGB565);
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

static int validate_native_framebuffer(struct fplinux_fb_session *session,
				       char *error, size_t error_size)
{
	struct fb_fix_screeninfo fixed;
	bool native_geometry;

	memset(&fixed, 0, sizeof(fixed));
	if (ioctl(session->framebuffer, FBIOGET_FSCREENINFO, &fixed) < 0) {
		set_errno_error(error, error_size,
				"cannot read native framebuffer identity");
		return -1;
	}
	native_geometry =
		((!strncmp(fixed.id, "ta1618-rgb565", sizeof(fixed.id)) ||
		  !strncmp(fixed.id, "inoi244-rgb565", sizeof(fixed.id))) &&
		 session->width == 240 && session->height == 320) ||
		(!strncmp(fixed.id, "inoi240-rgb565", sizeof(fixed.id)) &&
		 session->width == 128 && session->height == 160);
	if (!native_geometry ||
	    session->stride != session->width * sizeof(uint16_t) ||
	    session->page_bytes != session->stride * session->height) {
		errno = EINVAL;
		set_message(
			error, error_size,
			"framebuffer must be a native-size Nokia or INOI RGB565 display without row padding");
		return -1;
	}
	return 0;
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

static int sleep_until(uint64_t deadline_ns)
{
	struct timespec deadline = {
		.tv_sec = (time_t)(deadline_ns / 1000000000ULL),
		.tv_nsec = (long)(deadline_ns % 1000000000ULL),
	};
	int result;

	do {
		result = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME,
					 &deadline, NULL);
	} while (result == EINTR && !stop_signal);
	if (result) {
		errno = result;
		return stop_signal && result == EINTR ? 1 : -1;
	}
	return 0;
}

static int hold_last_frame(unsigned int hold_ms)
{
	struct timespec now;
	uint64_t deadline;

	if (!hold_ms || stop_signal)
		return stop_signal ? 1 : 0;
	if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
		return -1;
	deadline = timespec_ns(&now) + (uint64_t)hold_ms * 1000000ULL;
	return sleep_until(deadline);
}

static int record_completion(struct run_stats *stats,
			     const struct ums9117_present *request)
{
	if (!request->sequence || request->sequence <= stats->last_sequence ||
	    !request->transfer_ns) {
		errno = EIO;
		return -1;
	}
	if (add_u64(&stats->transfer_total_ns, request->transfer_ns) < 0)
		return -1;
	if (!stats->completed) {
		stats->first_sequence = request->sequence;
		stats->transfer_min_ns = request->transfer_ns;
	} else if (request->transfer_ns < stats->transfer_min_ns) {
		stats->transfer_min_ns = request->transfer_ns;
	}
	if (request->transfer_ns > stats->transfer_max_ns)
		stats->transfer_max_ns = request->transfer_ns;
	stats->last_sequence = request->sequence;
	++stats->completed;
	return 0;
}

static int run_series(struct fplinux_fb_session *session,
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
		struct ums9117_present request;
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

			sleep_status = sleep_until(deadline);
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
		memset(&request, 0, sizeof(request));
		request.pixels = (uint64_t)(uintptr_t)pixels;
		request.format = options->mode == PRESENT_MODE_CPU_RGB565 ?
					 UMS9117_PRESENT_RGB565 :
					 UMS9117_PRESENT_NV16;
		request.bytes = (uint32_t)session->page_bytes;
		if (timing_start(&start) < 0)
			return -1;
		if (ioctl(session->framebuffer, UMS9117_FBIO_PRESENT,
			  &request) < 0) {
			if (errno == EINTR && stop_signal) {
				status = 1;
				break;
			}
			return -1;
		}
		if (timing_stop(&stop) < 0 ||
		    timing_add(&stats->present, &start, &stop) < 0 ||
		    record_completion(stats, &request) < 0)
			return -1;
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
	print_timing("present_ioctl", &stats->present);
	print_timing("whole_loop", &stats->whole);
	printf("lcdc_transfer completions=%u total_ns=%" PRIu64
	       " min_ns=%" PRIu64 " max_ns=%" PRIu64 "\n",
	       stats->completed, stats->transfer_total_ns,
	       stats->completed ? stats->transfer_min_ns : 0,
	       stats->transfer_max_ns);
	printf("lcdc_sequence first=%" PRIu64 " last=%" PRIu64 " delta=%" PRIu64
	       " strict_monotonic=%s\n",
	       stats->first_sequence, stats->last_sequence,
	       stats->completed ? stats->last_sequence - stats->first_sequence :
				  0,
	       stats->completed ? "yes" : "n/a");
}

int main(int argc, char **argv)
{
	struct fplinux_fb_session session;
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

	if (!parse_options(argc, argv, &options)) {
		usage(stderr);
		return EXIT_FAILURE;
	}
	if (install_signal_handlers() < 0) {
		perror("fplinux-present: cannot install signal handlers");
		return EXIT_FAILURE;
	}
	if (stop_signal) {
		interrupted = true;
		goto cleanup;
	}
	if (!fplinux_fb_session_open(&session, options.framebuffer_path,
				     options.tty_path, error, sizeof(error))) {
		fprintf(stderr, "fplinux-present: %s\n", error);
		failed = true;
		goto cleanup;
	}
	session_open = true;
	if (validate_native_framebuffer(&session, error, sizeof(error)) < 0) {
		fprintf(stderr, "fplinux-present: %s\n", error);
		failed = true;
		goto cleanup;
	}
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
	if (!fplinux_fb_session_set_graphics(&session, error, sizeof(error))) {
		fprintf(stderr, "fplinux-present: %s\n", error);
		failed = true;
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
		run_status = hold_last_frame(options.hold_ms);
		if (run_status < 0) {
			fprintf(stderr, "fplinux-present: final hold: %s\n",
				strerror(errno));
			failed = true;
		} else if (run_status > 0) {
			interrupted = true;
		}
	}

cleanup:
	if (session_open && !fplinux_fb_session_close(&session)) {
		fprintf(stderr,
			"fplinux-present: framebuffer/console restore failed\n");
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
