// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include "armada-scene.h"
#include "fplinux-fb-session.h"

#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <inttypes.h>
#include <linux/input.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#define SHOWCASE_FRAMEBUFFER "/dev/fb0"
#define SHOWCASE_TTY "/dev/tty0"
#define SHOWCASE_KEYPAD_PHYS "fplinux/keypad0"
#define SHOWCASE_VIBRATOR_PHYS "fplinux/vibrator0"
#ifndef FPLINUX_SHOWCASE_KEYPAD_LED_GLOB
#define FPLINUX_SHOWCASE_KEYPAD_LED_GLOB "/sys/class/leds/*/brightness"
#endif
#ifndef FPLINUX_SHOWCASE_LCD_BACKLIGHT_GLOB
#define FPLINUX_SHOWCASE_LCD_BACKLIGHT_GLOB "/sys/class/backlight/*/brightness"
#endif
#define SHOWCASE_MAX_INPUT_DEVICES 64
#define SHOWCASE_CLASS_PATH_BYTES 512U
#define SHOWCASE_EFFECT_LENGTH_MS 5000U
#define SHOWCASE_NANOSECONDS_PER_SECOND 1000000000ULL
#define SHOWCASE_MICROSECONDS_PER_SECOND 1000000ULL
#define SHOWCASE_FRAME_PERIOD_NS \
	(SHOWCASE_NANOSECONDS_PER_SECOND / ARMADA_FRAMES_PER_SECOND)
#define SHOWCASE_CYCLE_DURATION_NS \
	(SHOWCASE_FRAME_PERIOD_NS * ARMADA_DURATION_FRAMES)

struct class_output {
	char brightness[SHOWCASE_CLASS_PATH_BYTES];
	char max_brightness[SHOWCASE_CLASS_PATH_BYTES];
};

struct showcase_options {
	uint64_t runs;
	const char *keypad_led;
	const char *lcd_backlight;
};

struct hardware_state {
	int keypad;
	int vibrator;
	int effect_id;
	int keypad_original;
	int lcd_original;
	bool keypad_grabbed;
	bool keypad_interface;
	bool effect_uploaded;
	bool keypad_on;
	bool rumble_on;
	uint16_t rumble_cue_id;
	int lcd_level;
	struct class_output keypad_led;
	struct class_output lcd_backlight;
};

struct frame_statistics {
	uint64_t started_ns;
	uint64_t render_us_sum;
	uint64_t rendered;
	uint32_t render_us_max;
};

static volatile sig_atomic_t stop_requested;

static void print_usage(void)
{
	fprintf(stderr, "usage: fplinux-showcase [--runs N] [--keypad-led DIR] "
			"[--lcd-backlight DIR]\n");
}

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

static bool parse_options(int argc, char **argv,
			  struct showcase_options *options, const char **error)
{
	bool runs_set = false;
	int index;

	memset(options, 0, sizeof(*options));
	*error = NULL;
	for (index = 1; index < argc; ++index) {
		const char *argument = argv[index];

		if (!strcmp(argument, "--runs")) {
			if (runs_set || ++index == argc ||
			    !parse_positive_runs(argv[index], &options->runs,
						 error))
				return false;
			runs_set = true;
			continue;
		}
		if (!strcmp(argument, "--keypad-led")) {
			if (options->keypad_led || ++index == argc ||
			    argv[index][0] == '\0')
				break;
			options->keypad_led = argv[index];
			continue;
		}
		if (!strcmp(argument, "--lcd-backlight")) {
			if (options->lcd_backlight || ++index == argc ||
			    argv[index][0] == '\0')
				break;
			options->lcd_backlight = argv[index];
			continue;
		}
		break;
	}
	if (index == argc)
		return true;
	*error = "invalid arguments";
	return false;
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

static bool read_number(const char *path, int *value)
{
	char buffer[32];
	char *end;
	long parsed;
	ssize_t count;
	int descriptor = open(path, O_RDONLY | O_CLOEXEC);

	if (descriptor < 0)
		return false;
	count = read(descriptor, buffer, sizeof(buffer) - 1U);
	if (close(descriptor) < 0 && count >= 0)
		count = -1;
	if (count <= 0)
		return false;
	buffer[count] = '\0';
	errno = 0;
	parsed = strtol(buffer, &end, 10);
	if (errno || end == buffer || (*end != '\0' && *end != '\n') ||
	    parsed < 0 || parsed > INT32_MAX) {
		errno = EINVAL;
		return false;
	}
	*value = (int)parsed;
	return true;
}

static bool write_number(const char *path, int value)
{
	char buffer[32];
	int length;
	ssize_t written;
	int descriptor = open(path, O_WRONLY | O_CLOEXEC);

	if (descriptor < 0)
		return false;
	length = snprintf(buffer, sizeof(buffer), "%d\n", value);
	written = length > 0 && length < (int)sizeof(buffer) ?
			  write(descriptor, buffer, (size_t)length) :
			  -1;
	if (close(descriptor) < 0 && written == length)
		written = -1;
	if (written != length) {
		if (written >= 0)
			errno = EIO;
		return false;
	}
	return true;
}

static bool set_class_output(struct class_output *output, const char *directory)
{
	int brightness_length;
	int maximum_length;

	brightness_length = snprintf(output->brightness,
				     sizeof(output->brightness),
				     "%s/brightness", directory);
	maximum_length = snprintf(output->max_brightness,
				  sizeof(output->max_brightness),
				  "%s/max_brightness", directory);
	if (brightness_length < 0 || maximum_length < 0 ||
	    brightness_length >= (int)sizeof(output->brightness) ||
	    maximum_length >= (int)sizeof(output->max_brightness)) {
		errno = ENAMETOOLONG;
		return false;
	}
	return true;
}

static bool keyboard_led_path(const char *brightness_path)
{
	static const char function[] = "kbd_backlight";
	const char *component;
	const char *brightness;
	size_t component_length;

	brightness = strrchr(brightness_path, '/');
	if (!brightness || strcmp(brightness, "/brightness"))
		return false;
	component = brightness;
	while (component > brightness_path && component[-1] != '/')
		--component;
	component_length = (size_t)(brightness - component);
	return (component_length == sizeof(function) - 1U &&
		!memcmp(component, function, sizeof(function) - 1U)) ||
	       (component_length > sizeof(function) - 1U &&
		component[component_length - sizeof(function)] == ':' &&
		!memcmp(component + component_length - sizeof(function) + 1U,
			function, sizeof(function) - 1U));
}

static bool select_class_output(const char *pattern, bool keypad_led,
				struct class_output *output, char *error,
				size_t error_size)
{
	char directory[SHOWCASE_CLASS_PATH_BYTES];
	const char *attribute;
	glob_t matches = { 0 };
	const char *selected = NULL;
	int result;
	size_t index;

	result = glob(pattern, 0, NULL, &matches);
	if (result == GLOB_NOMATCH) {
		snprintf(error, error_size, "required %s is unavailable",
			 keypad_led ? "keypad LED" : "LCD backlight");
		errno = ENODEV;
		return false;
	}
	if (result != 0) {
		snprintf(error, error_size, "cannot enumerate %s",
			 keypad_led ? "keypad LEDs" : "LCD backlights");
		errno = EIO;
		return false;
	}
	for (index = 0; index < matches.gl_pathc; ++index) {
		if (keypad_led && !keyboard_led_path(matches.gl_pathv[index]))
			continue;
		if (selected) {
			snprintf(error, error_size, "ambiguous %s",
				 keypad_led ? "keypad LEDs" : "LCD backlights");
			errno = ENOTUNIQ;
			goto fail;
		}
		selected = matches.gl_pathv[index];
	}
	if (!selected) {
		snprintf(error, error_size, "required %s is unavailable",
			 keypad_led ? "keypad LED" : "LCD backlight");
		errno = ENODEV;
		goto fail;
	}
	attribute = strrchr(selected, '/');
	if (!attribute || (size_t)(attribute - selected) >= sizeof(directory)) {
		snprintf(error, error_size, "%s path is too long",
			 keypad_led ? "keypad LED" : "LCD backlight");
		errno = ENAMETOOLONG;
		goto fail;
	}
	memcpy(directory, selected, (size_t)(attribute - selected));
	directory[attribute - selected] = '\0';
	if (!set_class_output(output, directory)) {
		snprintf(error, error_size, "%s path is too long",
			 keypad_led ? "keypad LED" : "LCD backlight");
		goto fail;
	}
	globfree(&matches);
	return true;

fail:
	globfree(&matches);
	return false;
}

static bool bit_is_set(const unsigned long *bits, unsigned int bit)
{
	return (bits[bit / (8U * sizeof(*bits))] &
		(1UL << (bit % (8U * sizeof(*bits))))) != 0;
}

static int open_input(const char *required_name, const char *required_phys,
		      int flags)
{
	int index;

	for (index = 0; index < SHOWCASE_MAX_INPUT_DEVICES; ++index) {
		char path[64];
		char name[128] = { 0 };
		char phys[128] = { 0 };
		int descriptor;

		if (snprintf(path, sizeof(path), "/dev/input/event%d", index) >=
		    (int)sizeof(path))
			continue;
		descriptor = open(path, flags | O_CLOEXEC);
		if (descriptor < 0)
			continue;
		if (ioctl(descriptor, EVIOCGPHYS(sizeof(phys)), phys) >= 0 &&
		    strcmp(phys, required_phys) == 0 &&
		    (!required_name ||
		     (ioctl(descriptor, EVIOCGNAME(sizeof(name)), name) >= 0 &&
		      strcmp(name, required_name) == 0)))
			return descriptor;
		close(descriptor);
	}
	errno = ENODEV;
	return -1;
}

static bool input_supports(int descriptor, unsigned int type, unsigned int code,
			   unsigned int maximum)
{
	unsigned long bits[(KEY_MAX + 8U * sizeof(unsigned long)) /
			   (8U * sizeof(unsigned long))] = { 0 };
	size_t bytes = (maximum + 8U) / 8U;

	if (bytes > sizeof(bits)) {
		errno = EOVERFLOW;
		return false;
	}
	if (ioctl(descriptor, EVIOCGBIT(type, bytes), bits) < 0)
		return false;
	return bit_is_set(bits, code);
}

static bool write_force_feedback(int descriptor, int effect_id, int value)
{
	struct input_event event = {
		.type = EV_FF,
		.code = (uint16_t)effect_id,
		.value = value,
	};
	ssize_t written;

	written = write(descriptor, &event, sizeof(event));
	if (written == (ssize_t)sizeof(event))
		return true;
	if (written >= 0)
		errno = EIO;
	return false;
}

static bool open_hardware(struct hardware_state *state,
			  const struct showcase_options *options, char *error,
			  size_t error_size)
{
	unsigned long event_bits[(EV_MAX + 8U * sizeof(unsigned long)) /
				 (8U * sizeof(unsigned long))] = { 0 };
	struct ff_effect effect = {
		.type = FF_RUMBLE,
		.id = -1,
		.replay.length = SHOWCASE_EFFECT_LENGTH_MS,
		.u.rumble.strong_magnitude = UINT16_MAX,
		.u.rumble.weak_magnitude = UINT16_MAX,
	};
	int maximum;

	memset(state, 0, sizeof(*state));
	state->keypad = -1;
	state->vibrator = -1;
	state->effect_id = -1;
	state->keypad_original = -1;
	state->lcd_original = -1;
	state->lcd_level = -1;
	if (options->keypad_led) {
		if (!set_class_output(&state->keypad_led,
				      options->keypad_led)) {
			snprintf(error, error_size,
				 "keypad LED path is too long");
			return false;
		}
	} else if (!select_class_output(FPLINUX_SHOWCASE_KEYPAD_LED_GLOB, true,
					&state->keypad_led, error,
					error_size)) {
		return false;
	}
	if (options->lcd_backlight) {
		if (!set_class_output(&state->lcd_backlight,
				      options->lcd_backlight)) {
			snprintf(error, error_size,
				 "LCD backlight path is too long");
			return false;
		}
	} else if (!select_class_output(FPLINUX_SHOWCASE_LCD_BACKLIGHT_GLOB,
					false, &state->lcd_backlight, error,
					error_size)) {
		return false;
	}
	state->keypad =
		open_input(NULL, SHOWCASE_KEYPAD_PHYS, O_RDONLY | O_NONBLOCK);
	if (state->keypad < 0) {
		snprintf(error, error_size, "required keypad %s is unavailable",
			 SHOWCASE_KEYPAD_PHYS);
		return false;
	}
	if (ioctl(state->keypad, EVIOCGBIT(0, sizeof(event_bits)), event_bits) <
		    0 ||
	    !bit_is_set(event_bits, EV_KEY) ||
	    !input_supports(state->keypad, EV_KEY, KEY_BACKSPACE, KEY_MAX)) {
		snprintf(error, error_size,
			 "keypad does not expose KEY_BACKSPACE");
		return false;
	}
	if (ioctl(state->keypad, EVIOCGRAB, 1) < 0) {
		snprintf(error, error_size,
			 "cannot exclusively acquire keypad");
		return false;
	}
	state->keypad_grabbed = true;

	state->vibrator = open_input(NULL, SHOWCASE_VIBRATOR_PHYS, O_RDWR);
	if (state->vibrator < 0) {
		snprintf(error, error_size,
			 "required vibrator %s is unavailable",
			 SHOWCASE_VIBRATOR_PHYS);
		return false;
	}
	memset(event_bits, 0, sizeof(event_bits));
	if (ioctl(state->vibrator, EVIOCGBIT(0, sizeof(event_bits)),
		  event_bits) < 0 ||
	    !bit_is_set(event_bits, EV_FF) ||
	    !input_supports(state->vibrator, EV_FF, FF_RUMBLE, FF_MAX)) {
		snprintf(error, error_size,
			 "vibrator does not expose FF_RUMBLE");
		return false;
	}
	if (ioctl(state->vibrator, EVIOCSFF, &effect) < 0) {
		snprintf(error, error_size, "cannot upload rumble effect");
		return false;
	}
	state->effect_uploaded = true;
	state->effect_id = effect.id;
	if (!write_force_feedback(state->vibrator, state->effect_id, 0)) {
		snprintf(error, error_size, "cannot switch vibrator off");
		return false;
	}

	if (!read_number(state->keypad_led.max_brightness, &maximum) ||
	    maximum < 1 ||
	    !read_number(state->keypad_led.brightness,
			 &state->keypad_original)) {
		snprintf(error, error_size,
			 "required keypad LED interface is unavailable");
		return false;
	}
	state->keypad_interface = true;
	if (!write_number(state->keypad_led.brightness, 0)) {
		snprintf(error, error_size, "cannot switch keypad LED off");
		return false;
	}
	if (!read_number(state->lcd_backlight.max_brightness, &maximum) ||
	    maximum < 10 ||
	    !read_number(state->lcd_backlight.brightness,
			 &state->lcd_original)) {
		snprintf(error, error_size,
			 "required LCD backlight interface is unavailable");
		return false;
	}
	state->lcd_level = state->lcd_original;
	return true;
}

static bool close_hardware(struct hardware_state *state)
{
	bool ok = true;

	if (state->keypad_interface && state->keypad_original >= 0 &&
	    !write_number(state->keypad_led.brightness, state->keypad_original))
		ok = false;
	state->keypad_on = false;
	if (state->vibrator >= 0 && state->effect_uploaded) {
		if (!write_force_feedback(state->vibrator, state->effect_id, 0))
			ok = false;
		state->rumble_on = false;
		state->rumble_cue_id = 0;
		if (ioctl(state->vibrator, EVIOCRMFF, state->effect_id) < 0)
			ok = false;
	}
	if (state->lcd_original >= 0 &&
	    !write_number(state->lcd_backlight.brightness, state->lcd_original))
		ok = false;
	if (state->keypad_grabbed && ioctl(state->keypad, EVIOCGRAB, 0) < 0)
		ok = false;
	if (state->vibrator >= 0 && close(state->vibrator) < 0)
		ok = false;
	if (state->keypad >= 0 && close(state->keypad) < 0)
		ok = false;
	state->vibrator = -1;
	state->keypad = -1;
	return ok;
}

static bool apply_outputs(struct hardware_state *state,
			  const struct armada_outputs *outputs)
{
	int lcd = outputs->lcd_level >= 0 ? outputs->lcd_level :
					    state->lcd_original;

	if (outputs->keypad != state->keypad_on) {
		if (!write_number(state->keypad_led.brightness,
				  outputs->keypad ? 1 : 0))
			return false;
		state->keypad_on = outputs->keypad;
	}
	if (outputs->rumble && state->rumble_on &&
	    outputs->cue_id != state->rumble_cue_id) {
		if (!write_force_feedback(state->vibrator, state->effect_id, 0))
			return false;
		state->rumble_on = false;
	}
	if (outputs->rumble != state->rumble_on) {
		if (!write_force_feedback(state->vibrator, state->effect_id,
					  outputs->rumble ? 1 : 0))
			return false;
		state->rumble_on = outputs->rumble;
		state->rumble_cue_id = outputs->rumble ? outputs->cue_id : 0;
	}
	if (lcd != state->lcd_level) {
		if (!write_number(state->lcd_backlight.brightness, lcd))
			return false;
		state->lcd_level = lcd;
	}
	return true;
}

static int exit_key_pressed(int keypad)
{
	struct input_event events[16];
	ssize_t count;
	size_t index;

	for (;;) {
		count = read(keypad, events, sizeof(events));
		if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return 0;
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0 || count % (ssize_t)sizeof(events[0]) != 0) {
			if (count >= 0)
				errno = EIO;
			return -1;
		}
		for (index = 0; index < (size_t)count / sizeof(events[0]);
		     ++index)
			if (events[index].type == EV_KEY &&
			    events[index].code == KEY_BACKSPACE &&
			    events[index].value != 0)
				return 1;
	}
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

static void copy_frame(struct fplinux_fb_session *display,
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
	struct fplinux_fb_session display;
	struct hardware_state hardware;
	struct showcase_options options;
	struct frame_statistics statistics = { 0 };
	struct armada_scene *scene = NULL;
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
	const char *argument_error;
	char error[160] = { 0 };

	memset(&display, 0, sizeof(display));
	memset(&hardware, 0, sizeof(hardware));
	hardware.keypad = -1;
	hardware.vibrator = -1;
	hardware.lcd_original = -1;
	if (!parse_options(argc, argv, &options, &argument_error)) {
		fprintf(stderr, "fplinux-showcase: %s\n", argument_error);
		print_usage();
		return EXIT_FAILURE;
	}
	frame_limit = options.runs * ARMADA_DURATION_FRAMES;
	if (!install_signal_handlers()) {
		fprintf(stderr,
			"fplinux-showcase: cannot install signal handlers: %s\n",
			strerror(errno));
		return EXIT_FAILURE;
	}
	if (!open_hardware(&hardware, &options, error, sizeof(error))) {
		fprintf(stderr, "fplinux-showcase: %s: %s\n", error,
			strerror(errno));
		hardware_open = true;
		goto cleanup;
	}
	hardware_open = true;
	if (!fplinux_fb_session_open(&display, SHOWCASE_FRAMEBUFFER,
				     SHOWCASE_TTY, error, sizeof(error))) {
		fprintf(stderr, "fplinux-showcase: %s\n", error);
		goto cleanup;
	}
	display_open = true;
	if (display.pages != 2U ||
	    !((display.width == 240U && display.height == 320U) ||
	      (display.width == 128U && display.height == 160U))) {
		fprintf(stderr,
			"fplinux-showcase: expected two-page RGB565 240x320 or 128x160 framebuffer\n");
		goto cleanup;
	}
	if (!fplinux_fb_session_set_graphics(&display, error, sizeof(error))) {
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
	scene = armada_scene_create(display.width, display.height, pixels);
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
		key_result = exit_key_pressed(hardware.keypad);
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
		if (!apply_outputs(&hardware, &outputs)) {
			fprintf(stderr,
				"fplinux-showcase: cannot apply synchronized hardware cue: %s\n",
				strerror(errno));
			goto cleanup;
		}
		__sync_synchronize();
		if (!fplinux_fb_session_present(&display, page)) {
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
	free(pixels);
	if (hardware_open && !close_hardware(&hardware)) {
		fprintf(stderr,
			"fplinux-showcase: hardware restore was incomplete\n");
		success = false;
	}
	if (display_open && !fplinux_fb_session_close(&display)) {
		fprintf(stderr,
			"fplinux-showcase: display restore was incomplete\n");
		success = false;
	}
	if (success && report_result)
		print_result(completed_runs, &statistics, finished_ns,
			     started_cpu_us, finished_cpu_us, peak_rss_kib);
	return success ? EXIT_SUCCESS : EXIT_FAILURE;
}
