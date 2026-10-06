// SPDX-License-Identifier: GPL-2.0-only
#ifndef FPLINUX_SHOWCASE_HARDWARE_H
#define FPLINUX_SHOWCASE_HARDWARE_H

#include "armada-scene.h"
#include "fplinux-brightness-client.h"

#define SHOWCASE_CLASS_PATH_BYTES 512U

struct showcase_class_output {
	char brightness[SHOWCASE_CLASS_PATH_BYTES];
	char max_brightness[SHOWCASE_CLASS_PATH_BYTES];
	char trigger[SHOWCASE_CLASS_PATH_BYTES];
};

struct showcase_hardware {
	int keypad;
	int vibrator;
	int effect_id;
	int keypad_original;
	char *keypad_trigger_original;
	struct fplinux_brightness_client brightness;
	bool keypad_grabbed;
	bool keypad_interface;
	bool effect_uploaded;
	bool keypad_on;
	bool rumble_on;
	bool brightness_claimed;
	uint16_t rumble_cue_id;
	int lcd_level;
	struct showcase_class_output keypad_led;
};

struct fplinux_drm_session;

/* Close also restores resources acquired by an unsuccessful open. */
bool showcase_hardware_open(struct showcase_hardware *state,
			    const char *keypad_led, char *error,
			    size_t error_size);
bool showcase_hardware_close(struct showcase_hardware *state);
bool showcase_hardware_apply_outputs(struct showcase_hardware *state,
				     const struct armada_outputs *outputs);
int showcase_hardware_exit_key_pressed(const struct showcase_hardware *state);
bool showcase_hardware_set_display_active(struct fplinux_drm_session *display,
					  bool active, void *data);

#endif
