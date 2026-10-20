// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include "showcase-hardware.h"
#include "fplinux-keypad.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define SHOWCASE_KEYPAD_PHYS "fplinux/keypad0"
#define SHOWCASE_VIBRATOR_PHYS "fplinux/vibrator0"
#ifndef FPLINUX_KEYPAD_LED_CLASS_DIR
#define FPLINUX_KEYPAD_LED_CLASS_DIR "/sys/class/leds"
#endif
#define SHOWCASE_MAX_INPUT_DEVICES 64
#define SHOWCASE_EFFECT_LENGTH_MS 5000U

static bool bit_is_set(const unsigned long *bits, unsigned int bit)
{
	return (bits[bit / (8U * sizeof(*bits))] &
		(1UL << (bit % (8U * sizeof(*bits))))) != 0;
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
	if (!bit_is_set(bits, code)) {
		errno = ENOTSUP;
		return false;
	}
	return true;
}

static int open_input(const char *required_name, const char *required_phys,
		      int flags, int required_key)
{
	int saved_errno = ENODEV;
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
		      strcmp(name, required_name) == 0))) {
			if (required_key < 0 ||
			    (input_supports(descriptor, 0, EV_KEY, EV_MAX) &&
			     input_supports(descriptor, EV_KEY,
					    (unsigned int)required_key,
					    KEY_MAX)))
				return descriptor;
			saved_errno = errno;
		}
		close(descriptor);
	}
	errno = saved_errno;
	return -1;
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
	char directory[FPLINUX_KEYPAD_LIGHT_PATH_BYTES];

	memset(state, 0, sizeof(*state));
	state->keypad = -1;
	state->vibrator = -1;
	state->effect_id = -1;
	state->brightness.fd = -1;
	state->lcd_level = -1;
	if (!keypad_led) {
		if (fplinux_keypad_light_find(directory, sizeof(directory),
					      FPLINUX_KEYPAD_LED_CLASS_DIR) <
		    0) {
			snprintf(error, error_size, "%s",
				 errno == ENOTUNIQ ?
					 "ambiguous keypad LEDs" :
					 "required keypad LED is unavailable");
			return false;
		}
		keypad_led = directory;
	}
	state->keypad = open_input(NULL, SHOWCASE_KEYPAD_PHYS,
				   O_RDONLY | O_NONBLOCK,
				   FPLINUX_KEY_SOFT_RIGHT);
	if (state->keypad < 0) {
		if (errno == ENODEV)
			snprintf(error, error_size,
				 "required keypad %s is unavailable",
				 SHOWCASE_KEYPAD_PHYS);
		else
			snprintf(error, error_size,
				 "keypad does not expose the right soft key");
		return false;
	}

	state->vibrator = open_input(NULL, SHOWCASE_VIBRATOR_PHYS, O_RDWR, -1);
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

	if (fplinux_keypad_light_open(&state->keypad_light, keypad_led) < 0) {
		snprintf(error, error_size,
			 "required keypad LED interface is unavailable");
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

	if (fplinux_keypad_light_close(&state->keypad_light) < 0)
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
		if (fplinux_keypad_light_set(&state->keypad_light,
					     outputs->keypad) < 0)
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
