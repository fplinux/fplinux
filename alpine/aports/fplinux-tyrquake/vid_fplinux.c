// SPDX-License-Identifier: GPL-2.0-or-later
/* Native DRM video backend for FPLinux. */
/* fplinux-check: package-embedded */

#include "fplinux-drm-session.h"
#include "fplinux-quake-internal.h"

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#undef K_SCROLLLOCK
#undef K_NUMLOCK
#undef K_CAPSLOCK

#include "common.h"
#include "console.h"
#include "d_local.h"
#include "input.h"
#include "quakedef.h"
#include "screen.h"
#include "sys.h"
#include "vid.h"

#ifdef NQ_HACK
#include "host.h"
#endif
#ifdef QW_HACK
#include "client.h"
#endif

#define FPLINUX_QUAKE_VIDEO_BITS_PER_PIXEL 16
#define FPLINUX_QUAKE_VIDEO_CACHE_GUARD_BYTES 16

viddef_t vid;
unsigned short d_8to16table[256];
unsigned d_8to24table[256];

static struct fplinux_drm_session display;
static uint8_t *framebuffer;
static size_t framebuffer_page_bytes;
static unsigned int framebuffer_width;
static unsigned int framebuffer_height;
static unsigned int framebuffer_stride;
static unsigned int framebuffer_pages;
static qboolean framebuffer_rotated;
static unsigned int output_width;
static unsigned int output_height;
static unsigned int render_scale;
static unsigned int render_width;
static unsigned int render_height;
static unsigned int next_page;
static int video_hunk_mark = -1;
static volatile sig_atomic_t exit_signal;
static struct sigaction previous_sigint;
static struct sigaction previous_sigterm;
static struct sigaction previous_sighup;
static struct sigaction previous_sigquit;
static qboolean signal_handlers_installed;
static qvidmode_t fixed_mode;

static void request_clean_exit(int signal_number)
{
	exit_signal = signal_number;
}

static void install_signal_handlers(void)
{
	struct sigaction action;

	memset(&action, 0, sizeof(action));
	action.sa_handler = request_clean_exit;
	sigemptyset(&action.sa_mask);

	if (sigaction(SIGINT, &action, &previous_sigint) < 0)
		Sys_Error("FPLinux video: sigaction(SIGINT): %s",
			  strerror(errno));
	if (sigaction(SIGTERM, &action, &previous_sigterm) < 0) {
		int saved_errno = errno;

		sigaction(SIGINT, &previous_sigint, NULL);
		Sys_Error("FPLinux video: sigaction(SIGTERM): %s",
			  strerror(saved_errno));
	}
	if (sigaction(SIGHUP, &action, &previous_sighup) < 0) {
		int saved_errno = errno;

		sigaction(SIGTERM, &previous_sigterm, NULL);
		sigaction(SIGINT, &previous_sigint, NULL);
		Sys_Error("FPLinux video: sigaction(SIGHUP): %s",
			  strerror(saved_errno));
	}
	if (sigaction(SIGQUIT, &action, &previous_sigquit) < 0) {
		int saved_errno = errno;

		sigaction(SIGHUP, &previous_sighup, NULL);
		sigaction(SIGTERM, &previous_sigterm, NULL);
		sigaction(SIGINT, &previous_sigint, NULL);
		Sys_Error("FPLinux video: sigaction(SIGQUIT): %s",
			  strerror(saved_errno));
	}
	signal_handlers_installed = true;
}

static void restore_signal_handlers(void)
{
	if (!signal_handlers_installed)
		return;

	sigaction(SIGQUIT, &previous_sigquit, NULL);
	sigaction(SIGHUP, &previous_sighup, NULL);
	sigaction(SIGTERM, &previous_sigterm, NULL);
	sigaction(SIGINT, &previous_sigint, NULL);
	signal_handlers_installed = false;
	exit_signal = 0;
}

struct fplinux_drm_session *fplinux_quake_display_session(void)
{
	return &display;
}

static uint8_t *framebuffer_page(unsigned int page)
{
	return framebuffer + page * framebuffer_page_bytes;
}

static int pan_to_page(unsigned int page)
{
	if (!fplinux_drm_session_present(&display, page))
		return -1;
	next_page = 1U - page;
	return 0;
}

static void choose_render_geometry(void)
{
	framebuffer_rotated = framebuffer_height > framebuffer_width;
	if (framebuffer_rotated) {
		output_width = framebuffer_height;
		output_height = framebuffer_width;
	} else {
		output_width = framebuffer_width;
		output_height = framebuffer_height;
	}

	render_scale = 1;
	while (output_width * render_scale < MINWIDTH ||
	       output_height * render_scale < MINHEIGHT ||
	       (output_width * render_scale) % 8 != 0)
		++render_scale;

	render_width = output_width * render_scale;
	render_height = output_height * render_scale;
}

static void open_display(void)
{
	char error[160];

	if (!fplinux_drm_session_open(&display, NULL, NULL, DRM_FORMAT_RGB565,
				      error, sizeof(error)))
		Sys_Error("FPLinux video: %s", error);
	framebuffer_width = display.width;
	framebuffer_height = display.height;
	framebuffer_stride = display.stride;
	framebuffer_page_bytes = display.page_bytes;
	framebuffer_pages = display.pages;
	framebuffer = display.mapping;
	next_page = 1U - display.shown_page;
	choose_render_geometry();
}

static void allocate_renderer_buffers(void)
{
	size_t pixels = (size_t)render_width * render_height;
	size_t cache_size = D_SurfaceCacheForRes(render_width, render_height);
	size_t zbuffer_size = pixels * sizeof(*d_pzbuffer);
	size_t total = zbuffer_size + cache_size +
		       FPLINUX_QUAKE_VIDEO_CACHE_GUARD_BYTES + pixels + pixels;
	byte *block;
	byte *surface_cache;

	video_hunk_mark = Hunk_HighMark();
	block = Hunk_HighAllocName(total, "fplinux-video");
	if (!block)
		Sys_Error("FPLinux video: not enough renderer memory");

	d_pzbuffer = (short *)block;
	surface_cache = block + zbuffer_size;
	r_warpbuffer = surface_cache + cache_size +
		       FPLINUX_QUAKE_VIDEO_CACHE_GUARD_BYTES;
	vid.buffer = r_warpbuffer + pixels;
	vid.conbuffer = vid.direct = vid.buffer;

	D_InitCaches(surface_cache, cache_size);
	R_AllocSurfEdges(false);
}

static void write_output_pixel(uint8_t *destination, unsigned int x,
			       unsigned int y, uint16_t pixel)
{
	unsigned int framebuffer_x;
	unsigned int framebuffer_y;
	uint16_t *row;

	if (framebuffer_rotated) {
		framebuffer_x = framebuffer_width - 1 - y;
		framebuffer_y = x;
	} else {
		framebuffer_x = x;
		framebuffer_y = y;
	}
	row = (uint16_t *)(destination + framebuffer_y * framebuffer_stride);
	row[framebuffer_x] = pixel;
}

static void render_frame(uint8_t *destination)
{
	unsigned int x;
	unsigned int y;

	for (y = 0; y < output_height; ++y) {
		unsigned int source_y = y * render_scale;

		for (x = 0; x < output_width; ++x) {
			unsigned int source_x = x * render_scale;
			byte index =
				vid.buffer[source_y * vid.rowbytes + source_x];

			write_output_pixel(destination, x, y,
					   d_8to16table[index]);
		}
	}
}

static void render_overlay(uint8_t *destination, int overlay_x, int overlay_y,
			   const byte *source, int width, int height)
{
	unsigned int x;
	unsigned int y;

	if (!source || width <= 0 || height <= 0)
		return;

	for (y = 0; y < output_height; ++y) {
		int logical_y = (int)(y * render_scale);

		if (logical_y < overlay_y || logical_y >= overlay_y + height)
			continue;
		for (x = 0; x < output_width; ++x) {
			int logical_x = (int)(x * render_scale);
			byte index;

			if (logical_x < overlay_x ||
			    logical_x >= overlay_x + width)
				continue;
			index = source[(logical_y - overlay_y) * width +
				       logical_x - overlay_x];
			write_output_pixel(destination, x, y,
					   d_8to16table[index]);
		}
	}
}

static void publish_frame(int x, int y, const byte *overlay, int width,
			  int height)
{
	uint8_t *destination;
	unsigned int page;

	if (!framebuffer || !vid.buffer)
		return;

	if (!fplinux_drm_session_dispatch(&display))
		Sys_Error("FPLinux video: VT dispatch: %s", strerror(errno));
	if (!display.active)
		return;
	page = next_page;
	destination = framebuffer_page(page);
	render_frame(destination);
	render_overlay(destination, x, y, overlay, width, height);
	__sync_synchronize();
	if (pan_to_page(page) < 0)
		Sys_Error("FPLinux video: DRM present: %s", strerror(errno));
}

void VID_GetDesktopRect(vrect_t *rect)
{
	rect->x = 0;
	rect->y = 0;
	rect->width = render_width;
	rect->height = render_height;
}

void VID_SetPalette(const byte *palette)
{
	unsigned int i;

	for (i = 0; i < 256; ++i) {
		unsigned int red = palette[i * 3];
		unsigned int green = palette[i * 3 + 1];
		unsigned int blue = palette[i * 3 + 2];

		d_8to16table[i] =
			(unsigned short)(((red & 0xf8) << 8) |
					 ((green & 0xfc) << 3) | (blue >> 3));
		d_8to24table[i] = red | (green << 8) | (blue << 16);
	}
}

void VID_ShiftPalette(const byte *palette)
{
	VID_SetPalette(palette);
}

void VID_InitColormap(const byte *palette)
{
	(void)palette;
	vid.colormap = host_colormap;
	vid.fullbright = 256 - LittleLong(*((int *)vid.colormap + 2048));
}

void VID_Init(const byte *palette)
{
	install_signal_handlers();
	open_display();

	fixed_mode.width = render_width;
	fixed_mode.height = render_height;
	fixed_mode.bpp = FPLINUX_QUAKE_VIDEO_BITS_PER_PIXEL;
	fixed_mode.refresh = 0;
	fixed_mode.min_scale = 1;
	fixed_mode.resolution.scale = 1;
	fixed_mode.resolution.width = render_width;
	fixed_mode.resolution.height = render_height;

	vid_modelist = &fixed_mode;
	vid_nummodes = 1;
	vid_windowed_mode = fixed_mode;
	vid_currentmode = &fixed_mode;

	VID_Mode_SetupViddef(&fixed_mode, &vid);
	vid.rowbytes = vid.conrowbytes = render_width;
	vid.aspect = 1.0;
	vid.numpages = 1;
	vid.recalc_refdef = 1;

	allocate_renderer_buffers();
	VID_InitColormap(palette);
	VID_SetPalette(palette);

	vid_menudrawfn = VID_MenuDraw;
	vid_menukeyfn = VID_MenuKey;
	vsync_available = false;
	adaptive_vsync_available = false;

	Cvar_SetValue("vid_fullscreen", 1);
	Cvar_SetValue("vid_width", render_width);
	Cvar_SetValue("vid_height", render_height);
	Cvar_SetValue("vid_bpp", FPLINUX_QUAKE_VIDEO_BITS_PER_PIXEL);
	Cvar_SetValue("vid_refreshrate", fixed_mode.refresh);

	Con_Printf("FPLinux video: render %ux%u -> DRM %ux%u RGB565, %s, "
		   "%ux downscale, %u page%s\n",
		   render_width, render_height, framebuffer_width,
		   framebuffer_height,
		   framebuffer_rotated ? "clockwise" : "native orientation",
		   render_scale, framebuffer_pages,
		   framebuffer_pages == 1 ? "" : "s");
}

void VID_Shutdown(void)
{
	restore_signal_handlers();
	if (framebuffer && !fplinux_drm_session_close(&display))
		Con_Printf("FPLinux video: DRM close: %s\n", strerror(errno));
	framebuffer = NULL;

	if (video_hunk_mark >= 0) {
		Hunk_FreeToHighMark(video_hunk_mark);
		video_hunk_mark = -1;
		d_pzbuffer = NULL;
		r_warpbuffer = NULL;
		vid.buffer = vid.conbuffer = vid.direct = NULL;
	}
}

void VID_Update(vrect_t *rects)
{
	(void)rects;
	publish_frame(0, 0, NULL, 0, 0);
}

void D_BeginDirectRect(int x, int y, const byte *pbitmap, int width, int height)
{
	if (x < 0)
		x = (int)render_width + x;
	publish_frame(x, y, pbitmap, width, height);
}

void D_EndDirectRect(int x, int y, int width, int height)
{
	(void)x;
	(void)y;
	(void)width;
	(void)height;
	publish_frame(0, 0, NULL, 0, 0);
}

qboolean VID_CheckAdequateMem(int width, int height)
{
	return width == (int)render_width && height == (int)render_height;
}

qboolean VID_SetMode(const qvidmode_t *mode, const byte *palette)
{
	if (mode->width != (int)render_width ||
	    mode->height != (int)render_height ||
	    mode->bpp != FPLINUX_QUAKE_VIDEO_BITS_PER_PIXEL)
		return false;

	vid_currentmode = mode;
	VID_SetPalette(palette);
	return true;
}

void VID_SetDefaultMode(void)
{
}

void VID_ProcessEvents(void)
{
	if (exit_signal) {
		int signal_number = exit_signal;

		exit_signal = 0;
		Con_Printf("FPLinux video: signal %d requested clean exit\n",
			   signal_number);
		Sys_Quit();
	}
	if (framebuffer && !fplinux_drm_session_dispatch(&display))
		Sys_Error("FPLinux video: VT dispatch: %s", strerror(errno));
	IN_Commands();
}

void Sys_SendKeyEvents(void)
{
	VID_ProcessEvents();
}

void VID_LockBuffer(void)
{
}

void VID_UnlockBuffer(void)
{
}

void VID_AddCommands(void)
{
}

void VID_RegisterVariables(void)
{
}

qboolean window_visible(void)
{
	return true;
}
