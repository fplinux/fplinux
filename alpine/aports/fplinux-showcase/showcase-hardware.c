// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include "showcase-hardware.h"
#include "fplinux-keypad.h"

#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define SHOWCASE_KEYPAD_PHYS "fplinux/keypad0"
#define SHOWCASE_VIBRATOR_PHYS "fplinux/vibrator0"
#ifndef FPLINUX_SHOWCASE_KEYPAD_LED_GLOB
#define FPLINUX_SHOWCASE_KEYPAD_LED_GLOB "/sys/class/leds/*/brightness"
#endif
#define SHOWCASE_MAX_INPUT_DEVICES 64
#define SHOWCASE_EFFECT_LENGTH_MS 5000U

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

static bool read_selected_trigger(const char *path, char **value)
{
	char *buffer = NULL;
	char *selected;
	char *end;
	size_t capacity = 0;
	ssize_t count;
	int saved_errno;
	FILE *file = fopen(path, "re");

	if (!file)
		return false;
	errno = 0;
	count = getline(&buffer, &capacity, file);
	saved_errno = errno;
	if (fclose(file) < 0 && count >= 0) {
		free(buffer);
		return false;
	}
	if (count < 0) {
		free(buffer);
		errno = saved_errno ? saved_errno : EIO;
		return false;
	}
	selected = strchr(buffer, '[');
	end = selected ? strchr(selected + 1, ']') : NULL;
	if (!end || end == selected + 1) {
		free(buffer);
		errno = EINVAL;
		return false;
	}
	*end = '\0';
	memmove(buffer, selected + 1, (size_t)(end - selected));
	*value = buffer;
	return true;
}

static bool write_attribute(const char *path, const char *value, size_t length)
{
	ssize_t written;
	int descriptor = open(path, O_WRONLY | O_CLOEXEC);

	if (descriptor < 0)
		return false;
	written = write(descriptor, value, length);
	if (close(descriptor) < 0 && written == (ssize_t)length)
		written = -1;
	if (written != (ssize_t)length) {
		if (written >= 0)
			errno = EIO;
		return false;
	}
	return true;
}

static bool write_number(const char *path, int value)
{
	char buffer[32];
	int length = snprintf(buffer, sizeof(buffer), "%d\n", value);

	if (length <= 0 || length >= (int)sizeof(buffer)) {
		errno = EOVERFLOW;
		return false;
	}
	return write_attribute(path, buffer, (size_t)length);
}

static bool set_class_output(struct showcase_class_output *output,
			     const char *directory)
{
	int brightness_length;
	int maximum_length;
	int trigger_length;

	brightness_length = snprintf(output->brightness,
				     sizeof(output->brightness),
				     "%s/brightness", directory);
	maximum_length = snprintf(output->max_brightness,
				  sizeof(output->max_brightness),
				  "%s/max_brightness", directory);
	trigger_length = snprintf(output->trigger, sizeof(output->trigger),
				  "%s/trigger", directory);
	if (brightness_length < 0 || maximum_length < 0 || trigger_length < 0 ||
	    brightness_length >= (int)sizeof(output->brightness) ||
	    maximum_length >= (int)sizeof(output->max_brightness) ||
	    trigger_length >= (int)sizeof(output->trigger)) {
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

static bool select_keypad_led(struct showcase_class_output *output, char *error,
			      size_t error_size)
{
	char directory[SHOWCASE_CLASS_PATH_BYTES];
	const char *attribute;
	glob_t matches = { 0 };
	const char *selected = NULL;
	int result;
	size_t index;

	result = glob(FPLINUX_SHOWCASE_KEYPAD_LED_GLOB, 0, NULL, &matches);
	if (result == GLOB_NOMATCH) {
		snprintf(error, error_size,
			 "required keypad LED is unavailable");
		errno = ENODEV;
		return false;
	}
	if (result != 0) {
		snprintf(error, error_size, "cannot enumerate keypad LEDs");
		errno = EIO;
		return false;
	}
	for (index = 0; index < matches.gl_pathc; ++index) {
		if (!keyboard_led_path(matches.gl_pathv[index]))
			continue;
		if (selected) {
			snprintf(error, error_size, "ambiguous keypad LEDs");
			errno = ENOTUNIQ;
			goto fail;
		}
		selected = matches.gl_pathv[index];
	}
	if (!selected) {
		snprintf(error, error_size,
			 "required keypad LED is unavailable");
		errno = ENODEV;
		goto fail;
	}
	attribute = strrchr(selected, '/');
	if (!attribute || (size_t)(attribute - selected) >= sizeof(directory)) {
		snprintf(error, error_size, "keypad LED path is too long");
		errno = ENAMETOOLONG;
		goto fail;
	}
	memcpy(directory, selected, (size_t)(attribute - selected));
	directory[attribute - selected] = '\0';
	if (!set_class_output(output, directory)) {
		snprintf(error, error_size, "keypad LED path is too long");
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

bool showcase_hardware_open(struct showcase_hardware *state,
			    const char *keypad_led, char *error,
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
	state->brightness.fd = -1;
	state->lcd_level = -1;
	if (keypad_led) {
		if (!set_class_output(&state->keypad_led, keypad_led)) {
			snprintf(error, error_size,
				 "keypad LED path is too long");
			return false;
		}
	} else if (!select_keypad_led(&state->keypad_led, error, error_size)) {
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
	    !input_supports(state->keypad, EV_KEY, FPLINUX_KEY_SOFT_RIGHT,
			    KEY_MAX)) {
		snprintf(error, error_size,
			 "keypad does not expose the right soft key");
		return false;
	}

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
			 &state->keypad_original) ||
	    !read_selected_trigger(state->keypad_led.trigger,
				   &state->keypad_trigger_original)) {
		snprintf(error, error_size,
			 "required keypad LED interface is unavailable");
		return false;
	}
	state->keypad_interface = true;
	if (!write_attribute(state->keypad_led.trigger, "none", 4U)) {
		snprintf(error, error_size,
			 "cannot release keypad LED trigger");
		return false;
	}
	if (!write_number(state->keypad_led.brightness, 0)) {
		snprintf(error, error_size, "cannot switch keypad LED off");
		return false;
	}
	return true;
}

static bool claim_brightness(struct showcase_hardware *state)
{
	if (state->brightness_claimed)
		return true;
	if (state->brightness.fd < 0 &&
	    fplinux_brightness_connect(&state->brightness, NULL) < 0)
		return false;
	if (fplinux_brightness_claim(&state->brightness) < 0)
		return false;
	state->brightness_claimed = true;
	state->lcd_level = -1;
	return true;
}

static bool release_brightness(struct showcase_hardware *state)
{
	if (!state->brightness_claimed)
		return true;
	if (fplinux_brightness_release(&state->brightness) < 0)
		return false;
	state->brightness_claimed = false;
	state->lcd_level = -1;
	return true;
}

static bool apply_lcd_level(struct showcase_hardware *state, int level)
{
	if (level < 0)
		return release_brightness(state);
	if (!claim_brightness(state))
		return false;
	if (level == state->lcd_level)
		return true;
	if (fplinux_brightness_show(&state->brightness, (unsigned int)level) <
	    0)
		return false;
	state->lcd_level = level;
	return true;
}

bool showcase_hardware_close(struct showcase_hardware *state)
{
	bool ok = true;
	int saved_errno = 0;

	if (!release_brightness(state)) {
		saved_errno = errno;
		ok = false;
	}
	fplinux_brightness_close(&state->brightness);

	if (state->keypad_interface && state->keypad_original >= 0 &&
	    !write_number(state->keypad_led.brightness, state->keypad_original))
		ok = false;
	/* A zero brightness write removes the trigger, so restore it last. */
	if (state->keypad_interface &&
	    !write_attribute(state->keypad_led.trigger,
			     state->keypad_trigger_original,
			     strlen(state->keypad_trigger_original)))
		ok = false;
	free(state->keypad_trigger_original);
	state->keypad_trigger_original = NULL;
	state->keypad_on = false;
	if (state->vibrator >= 0 && state->effect_uploaded) {
		if (!write_force_feedback(state->vibrator, state->effect_id, 0))
			ok = false;
		state->rumble_on = false;
		state->rumble_cue_id = 0;
		if (ioctl(state->vibrator, EVIOCRMFF, state->effect_id) < 0)
			ok = false;
	}
	if (state->keypad_grabbed && ioctl(state->keypad, EVIOCGRAB, 0) < 0)
		ok = false;
	if (state->vibrator >= 0 && close(state->vibrator) < 0)
		ok = false;
	if (state->keypad >= 0 && close(state->keypad) < 0)
		ok = false;
	state->vibrator = -1;
	state->keypad = -1;
	if (saved_errno)
		errno = saved_errno;
	return ok;
}

bool showcase_hardware_apply_outputs(struct showcase_hardware *state,
				     const struct armada_outputs *outputs)
{
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
	return apply_lcd_level(state, outputs->lcd_level);
}

int showcase_hardware_exit_key_pressed(const struct showcase_hardware *state)
{
	struct input_event events[16];
	ssize_t count;
	size_t index;

	for (;;) {
		count = read(state->keypad, events, sizeof(events));
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
			    events[index].code == FPLINUX_KEY_SOFT_RIGHT &&
			    events[index].value != 0)
				return 1;
	}
}

bool showcase_hardware_set_display_active(struct fplinux_drm_session *display,
					  bool active, void *data)
{
	struct showcase_hardware *hardware = data;
	struct input_event events[32];
	struct armada_outputs idle = { .lcd_level = -1 };
	ssize_t count;

	(void)display;
	if (!active && !showcase_hardware_apply_outputs(hardware, &idle))
		return false;
	if (hardware->keypad_grabbed) {
		if (ioctl(hardware->keypad, EVIOCGRAB, 0) < 0)
			return false;
		hardware->keypad_grabbed = false;
	}
	do {
		count = read(hardware->keypad, events, sizeof(events));
	} while (count > 0 || (count < 0 && errno == EINTR));
	if (active) {
		if (ioctl(hardware->keypad, EVIOCGRAB, 1) < 0)
			return false;
		hardware->keypad_grabbed = true;
		if (!claim_brightness(hardware))
			return false;
	}
	return true;
}
