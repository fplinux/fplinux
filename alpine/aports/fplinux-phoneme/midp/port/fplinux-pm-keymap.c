// SPDX-License-Identifier: GPL-2.0-only
/*
 * fplinux-check: package-embedded
 *
 * fplinux-phoneme key map with the Nokia-style game actions:
 * 2/4/6/8/5 are UP/LEFT/RIGHT/DOWN/FIRE besides the D-pad and OK, and
 * 1/3/7/9 are GAME_A..D. getKeyCode(GAME_A..D) returns those digits because
 * the phones have no dedicated game keys.
 */
#include <kni.h>
#include <keymap_input.h>

/* Canvas game actions (javax.microedition.lcdui.Canvas). */
enum {
	GA_UP = 1,
	GA_LEFT = 2,
	GA_RIGHT = 5,
	GA_DOWN = 6,
	GA_FIRE = 8,
	GA_GAME_A = 9,
	GA_GAME_B = 10,
	GA_GAME_C = 11,
	GA_GAME_D = 12,
};

static const KeymapKey keys[] = {
	{ KEYMAP_KEY_POWER, "POWER" },
	{ KEYMAP_KEY_SOFT1, "SOFT1" },
	{ KEYMAP_KEY_SOFT2, "SOFT2" },
	{ KEYMAP_KEY_UP, "Up" },
	{ KEYMAP_KEY_DOWN, "Down" },
	{ KEYMAP_KEY_LEFT, "Left" },
	{ KEYMAP_KEY_RIGHT, "Right" },
	{ KEYMAP_KEY_SELECT, "Select" },
	{ KEYMAP_KEY_SEND, "Send" },
	{ KEYMAP_KEY_END, "End" },
	{ KEYMAP_KEY_CLEAR, "Clear" },
	{ KEYMAP_KEY_1, "1" },
	{ KEYMAP_KEY_2, "2" },
	{ KEYMAP_KEY_3, "3" },
	{ KEYMAP_KEY_4, "4" },
	{ KEYMAP_KEY_5, "5" },
	{ KEYMAP_KEY_6, "6" },
	{ KEYMAP_KEY_7, "7" },
	{ KEYMAP_KEY_8, "8" },
	{ KEYMAP_KEY_9, "9" },
	{ KEYMAP_KEY_ASTERISK, "*" },
	{ KEYMAP_KEY_0, "0" },
	{ KEYMAP_KEY_POUND, "#" },
	{ KEYMAP_KEY_SPACE, "Space" },
	{ KEYMAP_KEY_BACKSPACE, "BackSpace" },
	{ KEYMAP_KEY_GAME_UP, "SHIFT_UP" },
	{ KEYMAP_KEY_GAME_DOWN, "SHIFT_DOWN" },
	{ KEYMAP_KEY_GAME_LEFT, "SHIFT_LEFT" },
	{ KEYMAP_KEY_GAME_RIGHT, "SHIFT_RIGHT" },
	{ KEYMAP_KEY_INVALID, "" },
};

int keymap_get_key_code(int gameAction)
{
	switch (gameAction) {
	case GA_UP:
		return KEYMAP_KEY_UP;
	case GA_DOWN:
		return KEYMAP_KEY_DOWN;
	case GA_LEFT:
		return KEYMAP_KEY_LEFT;
	case GA_RIGHT:
		return KEYMAP_KEY_RIGHT;
	case GA_FIRE:
		return KEYMAP_KEY_SELECT;
	case GA_GAME_A:
		return KEYMAP_KEY_1;
	case GA_GAME_B:
		return KEYMAP_KEY_3;
	case GA_GAME_C:
		return KEYMAP_KEY_7;
	case GA_GAME_D:
		return KEYMAP_KEY_9;
	default:
		return 0;
	}
}

int keymap_get_game_action(int keyCode)
{
	switch (keyCode) {
	case KEYMAP_KEY_UP:
	case KEYMAP_KEY_GAME_UP:
	case KEYMAP_KEY_2:
		return GA_UP;
	case KEYMAP_KEY_DOWN:
	case KEYMAP_KEY_GAME_DOWN:
	case KEYMAP_KEY_8:
		return GA_DOWN;
	case KEYMAP_KEY_LEFT:
	case KEYMAP_KEY_GAME_LEFT:
	case KEYMAP_KEY_4:
		return GA_LEFT;
	case KEYMAP_KEY_RIGHT:
	case KEYMAP_KEY_GAME_RIGHT:
	case KEYMAP_KEY_6:
		return GA_RIGHT;
	case KEYMAP_KEY_SELECT:
	case KEYMAP_KEY_5:
		return GA_FIRE;
	case KEYMAP_KEY_GAMEA:
	case KEYMAP_KEY_1:
		return GA_GAME_A;
	case KEYMAP_KEY_GAMEB:
	case KEYMAP_KEY_3:
		return GA_GAME_B;
	case KEYMAP_KEY_GAMEC:
	case KEYMAP_KEY_7:
		return GA_GAME_C;
	case KEYMAP_KEY_GAMED:
	case KEYMAP_KEY_9:
		return GA_GAME_D;
	default:
		return keymap_is_invalid_key_code(keyCode) ? -1 : 0;
	}
}

int keymap_get_system_key(int keyCode)
{
	switch (keyCode) {
	case KEYMAP_KEY_POWER:
		return 1;
	case KEYMAP_KEY_SEND:
		return 2;
	case KEYMAP_KEY_END:
		return 3;
	case KEYMAP_KEY_BACKSPACE:
	case KEYMAP_KEY_CLEAR:
		return 4;
	case KEYMAP_KEY_SELECT:
		return 5;
	default:
		return 0;
	}
}

char *keymap_get_key_name(int keyCode)
{
	const KeymapKey *k;

	for (k = keys; k->keyCode != KEYMAP_KEY_INVALID; k++) {
		if (k->keyCode == keyCode)
			return k->name;
	}
	return 0;
}

jboolean keymap_is_invalid_key_code(int keyCode)
{
	/* Unicode except 0 and 0xffff, plus the negative MIDP key codes. */
	if (keyCode == 0 || keyCode >= 0xFFFF || keyCode < MIN_KEY_VALUE)
		return KNI_TRUE;
	return KNI_FALSE;
}
