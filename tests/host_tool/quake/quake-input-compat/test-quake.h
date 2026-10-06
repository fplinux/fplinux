/* SPDX-License-Identifier: GPL-2.0-only */
/* The input adapter's engine boundary; no game or renderer runs here. */
#ifndef FPLINUX_TEST_QUAKE_H
#define FPLINUX_TEST_QUAKE_H

#include <stdbool.h>

typedef bool qboolean;
typedef struct {
	const char *name;
	const char *string;
	unsigned int flags;
	float value;
} cvar_t;

typedef struct {
	float sidemove;
	float forwardmove;
} usercmd_t;

typedef enum {
	K_UNKNOWN,
	K_1,
	K_2,
	K_3,
	K_4,
	K_5,
	K_6,
	K_7,
	K_8,
	K_9,
	K_0,
	K_q,
	K_w,
	K_e,
	K_r,
	K_t,
	K_y,
	K_u,
	K_i,
	K_o,
	K_p,
	K_a,
	K_s,
	K_d,
	K_f,
	K_g,
	K_h,
	K_j,
	K_k,
	K_l,
	K_z,
	K_x,
	K_c,
	K_v,
	K_b,
	K_n,
	K_m,
	K_F1,
	K_F2,
	K_F3,
	K_F4,
	K_F5,
	K_F6,
	K_F7,
	K_F8,
	K_F9,
	K_F10,
	K_ESCAPE,
	K_MINUS,
	K_EQUALS,
	K_BACKSPACE,
	K_TAB,
	K_LEFTBRACKET,
	K_RIGHTBRACKET,
	K_ENTER,
	K_SEMICOLON,
	K_QUOTE,
	K_BACKQUOTE,
	K_BACKSLASH,
	K_COMMA,
	K_PERIOD,
	K_SLASH,
	K_SPACE,
	K_DEL,
	K_INS,
	K_HOME,
	K_END,
	K_PGUP,
	K_PGDN,
	K_UPARROW,
	K_DOWNARROW,
	K_LEFTARROW,
	K_RIGHTARROW,
	K_PAUSE,
	K_F11,
	K_F12,
	K_LSHIFT,
	K_RSHIFT,
	K_LCTRL,
	K_RCTRL,
	K_LALT,
	K_RALT,
	K_LSUPER,
	K_RSUPER,
	K_CAPSLOCK,
	K_NUMLOCK,
	K_SCROLLOCK,
	K_SYSREQ,
	K_MENU,
	K_KP0,
	K_KP1,
	K_KP2,
	K_KP3,
	K_KP4,
	K_KP5,
	K_KP6,
	K_KP7,
	K_KP8,
	K_KP9,
	K_KP_PERIOD,
	K_KP_DIVIDE,
	K_KP_MULTIPLY,
	K_KP_MINUS,
	K_KP_PLUS,
	K_KP_ENTER,
	K_KP_EQUALS,
	K_MOUSE1,
	K_MOUSE2,
	K_MOUSE3,
	K_MWHEELUP,
	K_MWHEELDOWN,
	K_LAST,
} knum_t;

struct test_client {
	float viewangles[3];
};

struct test_button {
	int state;
};

#define CVAR_CONFIG 1U
#define YAW 1
#define PITCH 0

extern struct test_client cl;
extern struct test_button in_strafe;
extern cvar_t sensitivity, m_side, m_forward, m_yaw, m_pitch;
extern cvar_t cl_maxpitch, cl_minpitch;

void Cvar_RegisterVariable(cvar_t *variable);
void Key_Event(knum_t key, qboolean pressed);
void Key_ClearAllStates(void);
void Con_Printf(const char *format, ...);
_Noreturn void Sys_Error(const char *format, ...);
void V_StopPitchDrift(void);
void IN_Init(void);
void IN_Commands(void);
void IN_ClearStates(void);
void IN_SetFocus(qboolean focus);
qboolean IN_HaveFocus(void);
void IN_Shutdown(void);

#endif
