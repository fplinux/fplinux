/* SPDX-License-Identifier: GPL-2.0-only */
/* Minimal engine declarations for the video event-loop component test. */
#ifndef FPLINUX_TEST_QUAKE_VIDEO_H
#define FPLINUX_TEST_QUAKE_VIDEO_H

#include <stdbool.h>

typedef unsigned char byte;
typedef bool qboolean;
typedef int knum_t;

#define MINWIDTH 320
#define MINHEIGHT 200

typedef struct {
	int x, y, width, height;
} vrect_t;

typedef struct {
	int width, height, bpp, refresh, min_scale;
	struct {
		int width, height, scale;
	} resolution;
} qvidmode_t;

typedef struct {
	byte *buffer, *conbuffer, *direct, *colormap;
	int rowbytes, conrowbytes, numpages, recalc_refdef, fullbright;
	float aspect;
} viddef_t;

extern short *d_pzbuffer;
extern byte *r_warpbuffer;
extern byte *host_colormap;
extern qvidmode_t vid_windowed_mode;
extern qvidmode_t *vid_modelist;
extern int vid_nummodes;
extern const qvidmode_t *vid_currentmode;
extern qboolean vsync_available;
extern qboolean adaptive_vsync_available;
extern void (*vid_menudrawfn)(void);
extern void (*vid_menukeyfn)(knum_t key);

int Hunk_HighMark(void);
void *Hunk_HighAllocName(int size, const char *name);
void Hunk_FreeToHighMark(int mark);
int D_SurfaceCacheForRes(int width, int height);
void D_InitCaches(void *buffer, int size);
void R_AllocSurfEdges(qboolean nostack);
void Cvar_SetValue(const char *name, float value);
void VID_Mode_SetupViddef(const qvidmode_t *mode, viddef_t *definition);
void VID_MenuDraw(void);
void VID_MenuKey(knum_t key);
void IN_Commands(void);
void Con_Printf(const char *format, ...);
_Noreturn void Sys_Quit(void);
_Noreturn void Sys_Error(const char *format, ...);
int LittleLong(int value);
void VID_Init(const byte *palette);
void VID_Shutdown(void);
void Sys_SendKeyEvents(void);

#endif
