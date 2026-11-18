/* SPDX-License-Identifier: GPL-2.0-only */
#include "music-model.h"

#include <assert.h>
#include <stdio.h>

int main(void)
{
	struct music_transition result = music_model_transition(
		3, 1, 2999, MUSIC_REPEAT_OFF, MUSIC_PREVIOUS);
	assert(result.action == MUSIC_TRANSITION_SELECT &&
	       result.selected == 0);
	result = music_model_transition(3, 1, 3000, MUSIC_REPEAT_OFF,
					MUSIC_PREVIOUS);
	assert(result.action == MUSIC_TRANSITION_RESTART &&
	       result.selected == 1);
	result = music_model_transition(3, 0, 0, MUSIC_REPEAT_OFF,
					MUSIC_PREVIOUS);
	assert(result.action == MUSIC_TRANSITION_SELECT &&
	       result.selected == 2);
	result = music_model_transition(3, 2, 12000, MUSIC_REPEAT_OFF,
					MUSIC_NEXT);
	assert(result.action == MUSIC_TRANSITION_SELECT &&
	       result.selected == 0);
	result = music_model_transition(3, 2, 12000, MUSIC_REPEAT_OFF,
					MUSIC_END);
	assert(result.action == MUSIC_TRANSITION_FINISH);
	result = music_model_transition(3, 2, 12000, MUSIC_REPEAT_ALL,
					MUSIC_END);
	assert(result.action == MUSIC_TRANSITION_SELECT &&
	       result.selected == 0);
	result = music_model_transition(3, 1, 12000, MUSIC_REPEAT_ONE,
					MUSIC_END);
	assert(result.action == MUSIC_TRANSITION_RESTART);
	result = music_model_transition(3, 1, 12000, MUSIC_REPEAT_ONE,
					MUSIC_NEXT);
	assert(result.action == MUSIC_TRANSITION_SELECT &&
	       result.selected == 2);
	result = music_model_transition(0, 0, 12000, MUSIC_REPEAT_OFF,
					MUSIC_END);
	assert(result.action == MUSIC_TRANSITION_FINISH);
	result = music_model_transition(0, 0, 12000, MUSIC_REPEAT_ALL,
					MUSIC_END);
	assert(result.action == MUSIC_TRANSITION_RESTART);
	puts("music model: ok");
	return 0;
}
