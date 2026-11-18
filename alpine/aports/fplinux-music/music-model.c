/* SPDX-License-Identifier: GPL-2.0-only */
#include "music-model.h"

struct music_transition music_model_transition(size_t count, size_t selected,
					       uint64_t position_ms,
					       enum music_repeat repeat,
					       enum music_trigger trigger)
{
	struct music_transition result = { .action = MUSIC_TRANSITION_RESTART,
					   .selected = selected };
	if (trigger == MUSIC_PREVIOUS && position_ms >= 3000U)
		return result;
	if (trigger == MUSIC_END && repeat == MUSIC_REPEAT_ONE)
		return result;
	if (!count) {
		if (trigger == MUSIC_END && repeat == MUSIC_REPEAT_OFF)
			result.action = MUSIC_TRANSITION_FINISH;
		return result;
	}
	if (trigger == MUSIC_PREVIOUS)
		result.selected = selected ? selected - 1U : count - 1U;
	else if (selected + 1U < count)
		result.selected = selected + 1U;
	else if (trigger == MUSIC_END && repeat == MUSIC_REPEAT_OFF)
		result.action = MUSIC_TRANSITION_FINISH;
	else
		result.selected = 0;
	if (result.action != MUSIC_TRANSITION_FINISH &&
	    result.selected != selected)
		result.action = MUSIC_TRANSITION_SELECT;
	return result;
}
