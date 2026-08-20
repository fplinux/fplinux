// SPDX-License-Identifier: GPL-2.0-or-later
/* Native fbdev video backend for FPLinux. */
/* fplinux-check: package-embedded */

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/kd.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
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

#define FPLINUX_FB_DEVICE "/dev/fb0"
#define FPLINUX_TTY_DEVICE "/dev/tty0"
#define FPLINUX_BPP 16
#define FPLINUX_MAX_PAGES 2
#define CACHE_GUARD_BYTES 16

viddef_t vid;
unsigned short d_8to16table[256];
unsigned d_8to24table[256];

static int fb_fd = -1;
static int tty_fd = -1;
static int tty_saved_mode;
static qboolean tty_mode_changed;
static uint8_t *framebuffer;
static uint8_t *framebuffer_backup;
static size_t framebuffer_size;
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
static unsigned int original_page;
static unsigned int shown_page;
static unsigned int next_page;
static struct fb_var_screeninfo framebuffer_var;
static struct fb_var_screeninfo saved_framebuffer_var;
static qboolean framebuffer_mode_changed;
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

static void validate_rgb565(const struct fb_var_screeninfo *var)
{
	if (var->bits_per_pixel != FPLINUX_BPP || var->red.offset != 11 ||
	    var->red.length != 5 || var->red.msb_right != 0 ||
	    var->green.offset != 5 || var->green.length != 6 ||
	    var->green.msb_right != 0 || var->blue.offset != 0 ||
	    var->blue.length != 5 || var->blue.msb_right != 0 ||
	    var->transp.length != 0)
		Sys_Error("FPLinux video: expected RGB565, got bpp=%u "
			  "rgb=%u:%u:%u/%u:%u:%u/%u:%u:%u alpha=%u",
			  var->bits_per_pixel, var->red.offset, var->red.length,
			  var->red.msb_right, var->green.offset,
			  var->green.length, var->green.msb_right,
			  var->blue.offset, var->blue.length,
			  var->blue.msb_right, var->transp.length);
}

static void validate_framebuffer(const struct fb_fix_screeninfo *fix,
				 const struct fb_var_screeninfo *var)
{
	size_t page_bytes;

	if (fix->type != FB_TYPE_PACKED_PIXELS ||
	    fix->visual != FB_VISUAL_TRUECOLOR)
		Sys_Error("FPLinux video: unsupported fb type=%u visual=%u",
			  fix->type, fix->visual);
	if (var->xres == 0 || var->yres == 0 ||
	    var->xres_virtual != var->xres || var->xoffset != 0)
		Sys_Error(
			"FPLinux video: unsupported geometry %ux%u virtual=%ux%u "
			"offset=%ux%u",
			var->xres, var->yres, var->xres_virtual,
			var->yres_virtual, var->xoffset, var->yoffset);
	if (var->yres_virtual != var->yres &&
	    var->yres_virtual != var->yres * FPLINUX_MAX_PAGES)
		Sys_Error(
			"FPLinux video: unsupported virtual height %u for %u rows",
			var->yres_virtual, var->yres);
	if (var->yoffset != 0 &&
	    (var->yres_virtual != var->yres * FPLINUX_MAX_PAGES ||
	     var->yoffset != var->yres))
		Sys_Error("FPLinux video: unsupported yoffset %u",
			  var->yoffset);
	validate_rgb565(var);
	if (fix->line_length < var->xres * sizeof(uint16_t))
		Sys_Error("FPLinux video: stride %u is shorter than %u pixels",
			  fix->line_length, var->xres);

	page_bytes = (size_t)fix->line_length * var->yres;
	if (fix->smem_len < page_bytes)
		Sys_Error(
			"FPLinux video: framebuffer memory %u is shorter than "
			"%zu-byte page",
			fix->smem_len, page_bytes);
	if (var->yres_virtual == var->yres * FPLINUX_MAX_PAGES &&
	    (fix->ypanstep == 0 || fix->smem_len < page_bytes * 2))
		Sys_Error(
			"FPLinux video: virtual double buffer is not pannable");
}

static uint8_t *framebuffer_page(unsigned int page)
{
	return framebuffer + page * framebuffer_page_bytes;
}

static int pan_to_page(unsigned int page)
{
	struct fb_var_screeninfo pan;

	if (framebuffer_pages == 1) {
		shown_page = 0;
		next_page = 0;
		return 0;
	}
	if (page >= framebuffer_pages)
		return -1;

	pan = framebuffer_var;
	pan.xoffset = 0;
	pan.yoffset = page * framebuffer_height;
	pan.activate = FB_ACTIVATE_NOW;
	if (ioctl(fb_fd, FBIOPAN_DISPLAY, &pan) < 0)
		return -1;
	shown_page = page;
	next_page = page == 0 ? 1 : 0;
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

static void open_framebuffer(void)
{
	struct fb_fix_screeninfo fix;

	fb_fd = open(FPLINUX_FB_DEVICE, O_RDWR | O_CLOEXEC);
	if (fb_fd < 0)
		Sys_Error("FPLinux video: open(%s): %s", FPLINUX_FB_DEVICE,
			  strerror(errno));
	if (ioctl(fb_fd, FBIOGET_FSCREENINFO, &fix) < 0 ||
	    ioctl(fb_fd, FBIOGET_VSCREENINFO, &framebuffer_var) < 0)
		Sys_Error("FPLinux video: framebuffer ioctl: %s",
			  strerror(errno));

	validate_framebuffer(&fix, &framebuffer_var);
	saved_framebuffer_var = framebuffer_var;
	framebuffer_width = framebuffer_var.xres;
	framebuffer_height = framebuffer_var.yres;
	framebuffer_stride = fix.line_length;
	framebuffer_page_bytes =
		(size_t)framebuffer_stride * framebuffer_height;

	if (framebuffer_var.yres_virtual == framebuffer_height &&
	    fix.ypanstep != 0 &&
	    fix.smem_len >= framebuffer_page_bytes * FPLINUX_MAX_PAGES) {
		struct fb_var_screeninfo requested = framebuffer_var;

		requested.yres_virtual = framebuffer_height * FPLINUX_MAX_PAGES;
		requested.yoffset = 0;
		requested.activate = FB_ACTIVATE_NOW;
		if (ioctl(fb_fd, FBIOPUT_VSCREENINFO, &requested) == 0) {
			framebuffer_mode_changed = true;
			if (ioctl(fb_fd, FBIOGET_FSCREENINFO, &fix) < 0 ||
			    ioctl(fb_fd, FBIOGET_VSCREENINFO,
				  &framebuffer_var) < 0)
				Sys_Error(
					"FPLinux video: read double-buffered mode: %s",
					strerror(errno));
			validate_framebuffer(&fix, &framebuffer_var);
			framebuffer_stride = fix.line_length;
			framebuffer_page_bytes =
				(size_t)framebuffer_stride * framebuffer_height;
		} else {
			Con_Printf(
				"FPLinux video: double buffering unavailable: %s\n",
				strerror(errno));
		}
	}

	framebuffer_pages =
		framebuffer_var.yres_virtual >= framebuffer_height * 2 ? 2 : 1;
	framebuffer_size = framebuffer_page_bytes * framebuffer_pages;
	framebuffer = mmap(NULL, framebuffer_size, PROT_READ | PROT_WRITE,
			   MAP_SHARED, fb_fd, 0);
	if (framebuffer == MAP_FAILED) {
		framebuffer = NULL;
		Sys_Error("FPLinux video: mmap: %s", strerror(errno));
	}

	framebuffer_backup = malloc(framebuffer_size);
	if (!framebuffer_backup)
		Sys_Error("FPLinux video: cannot allocate framebuffer backup");
	memcpy(framebuffer_backup, framebuffer, framebuffer_size);

	original_page = framebuffer_var.yoffset / framebuffer_height;
	shown_page = original_page;
	next_page = framebuffer_pages == 2 ? 1U - shown_page : shown_page;
	choose_render_geometry();

	tty_fd = open(FPLINUX_TTY_DEVICE, O_RDWR | O_CLOEXEC);
	if (tty_fd < 0)
		Sys_Error("FPLinux video: open(%s): %s", FPLINUX_TTY_DEVICE,
			  strerror(errno));
	if (ioctl(tty_fd, KDGETMODE, &tty_saved_mode) < 0)
		Sys_Error("FPLinux video: KDGETMODE: %s", strerror(errno));
	if (ioctl(tty_fd, KDSETMODE, KD_GRAPHICS) < 0)
		Sys_Error("FPLinux video: KDSETMODE(KD_GRAPHICS): %s",
			  strerror(errno));
	tty_mode_changed = true;
}

static void allocate_renderer_buffers(void)
{
	size_t pixels = (size_t)render_width * render_height;
	size_t cache_size = D_SurfaceCacheForRes(render_width, render_height);
	size_t zbuffer_size = pixels * sizeof(*d_pzbuffer);
	size_t total =
		zbuffer_size + cache_size + CACHE_GUARD_BYTES + pixels + pixels;
	byte *block;
	byte *surface_cache;

	video_hunk_mark = Hunk_HighMark();
	block = Hunk_HighAllocName(total, "fplinux-video");
	if (!block)
		Sys_Error("FPLinux video: not enough renderer memory");

	d_pzbuffer = (short *)block;
	surface_cache = block + zbuffer_size;
	r_warpbuffer = surface_cache + cache_size + CACHE_GUARD_BYTES;
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

	page = next_page;
	destination = framebuffer_page(page);
	render_frame(destination);
	render_overlay(destination, x, y, overlay, width, height);
	__sync_synchronize();
	if (pan_to_page(page) < 0)
		Sys_Error("FPLinux video: FBIOPAN_DISPLAY: %s",
			  strerror(errno));
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
	open_framebuffer();

	fixed_mode.width = render_width;
	fixed_mode.height = render_height;
	fixed_mode.bpp = FPLINUX_BPP;
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
	Cvar_SetValue("vid_bpp", FPLINUX_BPP);
	Cvar_SetValue("vid_refreshrate", fixed_mode.refresh);

	Con_Printf("FPLinux video: render %ux%u -> fb %ux%u RGB565, %s, "
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
	if (framebuffer && framebuffer_backup) {
		if (framebuffer_pages == 1) {
			memcpy(framebuffer, framebuffer_backup,
			       framebuffer_page_bytes);
			__sync_synchronize();
		} else {
			unsigned int old_shown = shown_page;
			unsigned int hidden = 1U - old_shown;

			memcpy(framebuffer_page(hidden),
			       framebuffer_backup +
				       hidden * framebuffer_page_bytes,
			       framebuffer_page_bytes);
			__sync_synchronize();
			if (pan_to_page(hidden) == 0) {
				memcpy(framebuffer_page(old_shown),
				       framebuffer_backup +
					       old_shown *
						       framebuffer_page_bytes,
				       framebuffer_page_bytes);
				__sync_synchronize();
				if (shown_page != original_page)
					pan_to_page(original_page);
			}
		}
	}
	if (tty_mode_changed)
		ioctl(tty_fd, KDSETMODE, tty_saved_mode);
	tty_mode_changed = false;
	if (framebuffer_mode_changed && fb_fd >= 0 &&
	    ioctl(fb_fd, FBIOPUT_VSCREENINFO, &saved_framebuffer_var) < 0)
		Con_Printf("FPLinux video: restore framebuffer mode: %s\n",
			   strerror(errno));
	framebuffer_mode_changed = false;
	if (tty_fd >= 0)
		close(tty_fd);
	tty_fd = -1;

	free(framebuffer_backup);
	framebuffer_backup = NULL;
	if (framebuffer)
		munmap(framebuffer, framebuffer_size);
	framebuffer = NULL;
	framebuffer_size = 0;
	if (fb_fd >= 0)
		close(fb_fd);
	fb_fd = -1;

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
	    mode->height != (int)render_height || mode->bpp != FPLINUX_BPP)
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
