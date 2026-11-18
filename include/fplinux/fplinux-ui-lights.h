/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef FPLINUX_UI_LIGHTS_H
#define FPLINUX_UI_LIGHTS_H

#include <stdbool.h>
#include <stdint.h>

struct fplinux_ui;

enum fplinux_ui_light_resource {
	FPLINUX_UI_LIGHT_SCREEN = 1U,
	FPLINUX_UI_LIGHT_KEYPAD = 2U,
};

struct fplinux_ui_light_effect {
	unsigned int resources;
	/* SCREEN uses 1...10; otherwise zero. */
	unsigned int display_level;
	/* False unless KEYPAD is selected. */
	bool keypad_on;
};

/* Configured resources, not confirmation of a physical light change. */
unsigned int fplinux_ui_light_effects_supported(const struct fplinux_ui *ui);
/*
 * Transient effects belong to the visible foreground application. Losing
 * eligibility clears them; showing the application does not restore them.
 * Effects never change the user's selected brightness or saved settings.
 * A zero resource mask clears the caller's effect, including while hidden.
 *
 * False reports EINVAL for invalid fields, ENOTSUP for unavailable resources,
 * EAGAIN while hidden or leaving, or a connection error. True means the
 * request was sent. Optional serial receives that request's nonzero ID, or
 * zero on failure. SESSION replies echo this serial: LIGHT_EFFECT
 * confirms application, ERROR named "light_effect" reports execution failure.
 * Error reasons are "not-visible", "unsupported" or "unavailable".
 */
bool fplinux_ui_light_effects_apply(struct fplinux_ui *ui,
				    const struct fplinux_ui_light_effect *effect,
				    uint32_t *serial);
bool fplinux_ui_light_effects_clear(struct fplinux_ui *ui, uint32_t *serial);

#endif
