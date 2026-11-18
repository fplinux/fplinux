/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_MUSIC_MODEL_H
#define FPLINUX_MUSIC_MODEL_H

#include <stddef.h>
#include <stdint.h>

enum music_repeat {
	MUSIC_REPEAT_OFF,
	MUSIC_REPEAT_ALL,
	MUSIC_REPEAT_ONE,
};

enum music_trigger {
	MUSIC_PREVIOUS,
	MUSIC_NEXT,
	MUSIC_END,
};

enum music_transition_action {
	MUSIC_TRANSITION_RESTART,
	MUSIC_TRANSITION_SELECT,
	MUSIC_TRANSITION_FINISH,
};

struct music_transition {
	enum music_transition_action action;
	size_t selected;
};

/* Previous restarts after three seconds; manual track changes wrap the queue.
 * A file opened alone has count zero. Repeat-one applies to natural EOF only. */
struct music_transition music_model_transition(size_t count, size_t selected,
					       uint64_t position_ms,
					       enum music_repeat repeat,
					       enum music_trigger trigger);

#endif
