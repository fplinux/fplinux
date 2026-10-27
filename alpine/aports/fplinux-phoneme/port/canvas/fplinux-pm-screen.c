// SPDX-License-Identifier: GPL-2.0-only
/*
 * fplinux-check: package-embedded
 *
 * fplinux-phoneme display port: the lcdlf_* interface of
 * highlevelui/lcdlf/include/lcdlf_export.h over a putpixel RGB565 buffer and
 * one STAGED software frame shared by every Canvas peer. LVGL only reads the
 * published frame, including during a system menu redraw inside paint().
 * The buffers hold the whole display; their drawing width and height are
 * those of the shown Canvas. Refreshed rectangles collect as damage and
 * are copied only when paint() is complete, at most once per VM slice and
 * once more by a ready timer after the slice. Partial paints and GameCanvas
 * flushes retain pixels outside their damage, so swapping alone would lose
 * those pixels; copying damage needs no second full-frame synchronization.
 * Nothing is rendered while the app is hidden. Headless needs only putpixel.
 *
 * Canvas.setFullScreenMode() reaches lcdlf_set_fullscreen_mode() through
 * Display, which every Displayable passes when it is shown: the app asks
 * the runtime for the whole display, or for the integrated layout again.
 * The size is predicted from the display metrics at once (a game reads
 * getHeight() right after setFullScreenMode()), and when the runtime's
 * resize gives another size, a ROTATION_EVENT makes Display read it again
 * and call sizeChanged().
 */
#include <fplinux/fplinux-ui-frame.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <kni.h>
#include <lcdlf_export.h>
#include <gxj_putpixel.h>
#include <gxj_screen_buffer.h>
#include <midpEvents.h>

#include "../app/fplinux-pm-app.h"
#include "fplinux-pm-canvas-core.h"
#include "fplinux-pm-canvas.h"
#include "fplinux-pm-vm.h"

/* The putpixel library draws the screen Graphics into this buffer. */
gxj_screen_buffer gxj_system_screen_buffer;

/*
 * The one Display of the suite always has the foreground: Display only draws
 * and refreshes while its id equals gForegroundDisplayId. The id is the
 * first one DefaultDisplayIdPolicy gives isolate 0, (0 << 24) | 1; the
 * launcher creates its LCDUIEnvironment for isolate 0.
 */
int gForegroundIsolateId = 0;
int gForegroundDisplayId = 1;

/* Seconds between FPLINUX_PM_CANVAS_STATS lines on stderr. */
#define STATS_PERIOD_MS 5000u

/* A Canvas peer: a runtime canvas screen and a container for the shared frame. */
struct canvas_peer {
	struct fplinux_ui_screen *screen;
	lv_obj_t *container;
	struct canvas_peer *next;
};

struct frame_stats {
	bool enabled;
	uint64_t since_ms;
	unsigned int frames;
	unsigned int refreshes;
	uint64_t render_us;
	uint64_t render_max_us;
};

static struct {
	bool initialized;
	struct fplinux_ui *ui;
	struct fplinux_ui
		*owner_ui; /* Retained until the last old peer is deleted. */
	uint16_t *pixels;
	struct fplinux_ui_frame *frame;
	size_t capacity; /* pixels */
	/* The mode Display asked for and the shown Canvas's title band. */
	bool full_screen;
	bool titled;
	struct canvas_peer *peers;
	struct canvas_peer *active;
	lv_timer_t *flush;
	bool pending; /* A completed refresh awaits the next publication. */
	/* Binding or resizing alone does not prove that Java painted a usable frame. */
	bool completed_refresh;
	/* A frame was rendered since the flush timer last ran. */
	bool rendered;
	/* Canvas.paint() runs: the buffer holds a half-drawn frame. */
	bool painting;
	/* Between prepare0 and activate0; a resize then needs a new read. */
	bool showing;
	bool resized_while_showing;
	struct frame_stats stats;
	/* FPLINUX_PM_CANVAS_TRACE: every refresh on stderr, for headless runs. */
	bool trace;
	uint64_t started_ms;
} port;

static jint display_ids[] = { 0 };

struct fplinux_ui *fplinux_pm_canvas_ui(void)
{
	return port.ui;
}

bool fplinux_pm_canvas_is_active(const struct fplinux_ui_screen *screen)
{
	return port.active && screen && port.active->screen == screen;
}

static uint64_t monotonic_us(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000000u + (uint64_t)now.tv_nsec / 1000u;
}

/* ---- size ------------------------------------------------------------ */

static void canvas_size(bool full_screen, bool titled, int *width, int *height)
{
	struct fplinux_pm_canvas_geometry geometry = {
		.runtime = port.ui != NULL,
		.full_screen = full_screen,
		.titled = titled && !full_screen,
	};

	if (port.ui) {
		const struct fplinux_ui_metrics *metrics =
			fplinux_ui_metrics(port.ui);

		geometry.surface_matches = fplinux_ui_is_fullscreen(port.ui) ==
					   full_screen;
		geometry.surface_width = fplinux_ui_width(port.ui);
		geometry.surface_height = fplinux_ui_height(port.ui);
		geometry.screen_width = metrics->screen_width;
		geometry.screen_height = metrics->screen_height;
		geometry.status_height = metrics->status_height;
		geometry.softkey_height = metrics->softkey_height;
		geometry.header_height = metrics->header_height;
	}
	fplinux_pm_canvas_size(&geometry, width, height);
	/* Never larger than the buffer, whatever the runtime reports. */
	if ((size_t)*width > port.capacity)
		*width = (int)port.capacity;
	if (*width > 0 && (size_t)*width * (size_t)*height > port.capacity)
		*height = (int)(port.capacity / (size_t)*width);
}

/* The flush timer runs after the current VM slice. */
static void schedule_flush(void)
{
	if (port.flush) {
		fplinux_ui_frame_set_timer(port.frame, port.flush, true);
		lv_timer_ready(port.flush);
	}
}

/* The frame belongs to the port; a peer borrows its one canvas. */
static void bind(struct canvas_peer *peer)
{
	lv_obj_t *canvas = fplinux_ui_frame_object(port.frame);

	lv_obj_set_size(peer->container, gxj_system_screen_buffer.width,
			gxj_system_screen_buffer.height);
	lv_obj_set_parent(canvas, peer->container);
	lv_obj_set_hidden(canvas, false);
	fplinux_ui_frame_damage(port.frame, NULL);
	port.pending = true;
	port.completed_refresh = false;
	schedule_flush();
}

/* Display re-reads the size and calls sizeChanged() on the current LF. */
static void post_size_changed(void)
{
	MidpEvent event;

	if (fplinux_pm_vm_get_state() != FPLINUX_PM_VM_RUNNING)
		return;
	MIDP_EVENT_INITIALIZE(event);
	event.type = ROTATION_EVENT;
	event.DISPLAY = gForegroundDisplayId;
	StoreMIDPEventInVmThread(event, 0);
}

/*
 * Applies the size of the current mode and title band to the screen buffer
 * and the shown Canvas. With notify, a changed size while a Canvas is shown
 * reaches Java as a ROTATION_EVENT; the callers without it are inside a
 * Display call that reads the size itself.
 */
static bool apply_size(bool notify)
{
	int width;
	int height;
	bool changed;

	if (!port.pixels)
		return false;
	canvas_size(port.full_screen, port.titled, &width, &height);
	changed = width != gxj_system_screen_buffer.width ||
		  height != gxj_system_screen_buffer.height;
	if (changed && port.frame &&
	    !fplinux_ui_frame_resize(port.frame, width, height)) {
		fprintf(stderr, "fplinux-phoneme: resize frame: %s\n",
			strerror(errno));
		return false;
	}
	if (port.frame) {
		port.pixels = fplinux_ui_frame_pixels(port.frame, NULL, NULL);
		gxj_system_screen_buffer.pixelData = port.pixels;
	}
	gxj_system_screen_buffer.width = width;
	gxj_system_screen_buffer.height = height;
	if (changed)
		port.pending = false;
	if (port.active && changed)
		bind(port.active);
	if (port.active && changed && notify)
		post_size_changed();
	return changed;
}

/* fplinux_pm_app resize handler: the runtime's FPLINUX_UI_EVENT_RESIZE. */
static void surface_resized(void *context)
{
	(void)context;
	if (apply_size(true)) {
		/* Display read the size of a Canvas being shown before this. */
		if (!port.active && port.showing)
			port.resized_while_showing = true;
	} else if (port.active) {
		/* Same drawing area in a new place: show it there. */
		fplinux_ui_frame_damage(port.frame, NULL);
		port.pending = true;
		schedule_flush();
	}
}

/* ---- rendering ------------------------------------------------------- */

static void report_stats(uint64_t now_ms)
{
	struct frame_stats *stats = &port.stats;
	uint64_t span = now_ms - stats->since_ms;

	if (span < STATS_PERIOD_MS)
		return;
	if (stats->frames)
		fprintf(stderr,
			"fplinux-phoneme: canvas %u frames in %llu ms "
			"(%.1f fps), %u refreshes, render avg %.2f ms max "
			"%.2f ms\n",
			stats->frames, (unsigned long long)span,
			stats->frames * 1000.0 / (double)span, stats->refreshes,
			stats->render_us / 1000.0 / stats->frames,
			stats->render_max_us / 1000.0);
	stats->since_ms = now_ms;
	stats->frames = 0;
	stats->refreshes = 0;
	stats->render_us = 0;
	stats->render_max_us = 0;
}

/*
 * Shows the damage now. Only paint() and GameCanvas flushes (one native
 * copy) draw into the buffer, so outside paint() it holds a whole frame.
 * Hidden: the damage waits; showNotify repaints anyway.
 */
static void render(void)
{
	uint64_t start;
	uint64_t spent;

	if (!port.active || !port.pending || port.painting || !port.ui)
		return;
	start = monotonic_us();
	if (!fplinux_ui_frame_present(port.frame))
		return;
	if (port.completed_refresh) {
		port.completed_refresh = false;
		fplinux_pm_app_loaded();
	}
	port.pending = false;
	port.rendered = true;
	if (!port.stats.enabled)
		return;
	spent = monotonic_us() - start;
	port.stats.frames++;
	port.stats.render_us += spent;
	if (spent > port.stats.render_max_us)
		port.stats.render_max_us = spent;
	report_stats(start / 1000u);
}

/* After a VM slice: what was refreshed after its first frame. */
static void flush(lv_timer_t *timer)
{
	fplinux_ui_frame_set_timer(port.frame, timer, false);
	port.rendered = false;
	render();
}

/* ---- Canvas peers ---------------------------------------------------- */

static void release_buffers(void)
{
	if (port.frame)
		fplinux_ui_frame_delete(port.frame);
	else
		free(port.pixels); /* Headless putpixel storage. */
	port.frame = NULL;
	port.owner_ui = NULL;
	port.pixels = NULL;
	port.capacity = 0;
}

static void peer_deleted(lv_event_t *event)
{
	struct canvas_peer *peer = lv_event_get_user_data(event);
	struct canvas_peer **link = &port.peers;

	while (*link && *link != peer)
		link = &(*link)->next;
	if (*link)
		*link = peer->next;
	if (port.active == peer || !port.initialized) {
		/* DELETE precedes children: keep the port's frame alive. */
		lv_obj_t *canvas = fplinux_ui_frame_object(port.frame);

		if (canvas) {
			lv_obj_set_parent(canvas,
					  fplinux_ui_screen(port.owner_ui));
			lv_obj_set_hidden(canvas, true);
		}
	}
	if (port.active == peer) {
		port.active = NULL;
		port.pending = false;
		if (port.flush)
			fplinux_ui_frame_set_timer(port.frame, port.flush,
						   false);
		fplinux_pm_keys_stop();
		fplinux_pm_device_canvas_shown(false);
	}
	free(peer);
	if (!port.initialized && !port.peers)
		release_buffers();
}

/* A new unstacked canvas screen without title showing the buffer. */
static struct fplinux_ui_screen *create_peer(void)
{
	struct canvas_peer *peer;

	if (!port.ui || !port.pixels) {
		errno = EINVAL;
		return NULL;
	}
	peer = calloc(1, sizeof(*peer));
	if (!peer)
		return NULL;
	/* Allocate all peer resources before creating an unstacked screen. */
	peer->container = lv_obj_create(fplinux_ui_screen(port.ui));
	if (!peer->container) {
		errno = ENOMEM;
		free(peer);
		return NULL;
	}
	lv_obj_remove_style_all(peer->container);
	lv_obj_set_pos(peer->container, 0, 0);
	lv_obj_set_scrollable(peer->container, false);
	lv_obj_set_clickable(peer->container, false);
	if (!lv_obj_add_event_cb(peer->container, peer_deleted, LV_EVENT_DELETE,
				 peer)) {
		lv_obj_delete(peer->container);
		free(peer);
		errno = ENOMEM;
		return NULL;
	}
	peer->screen = fplinux_ui_canvas_new(port.ui, NULL);
	if (!peer->screen) {
		/* Its delete callback owns peer from the registration onwards. */
		lv_obj_delete(peer->container);
		return NULL;
	}
	lv_obj_set_parent(peer->container,
			  fplinux_ui_canvas_object(peer->screen));
	fplinux_ui_canvas_set_key_handler(peer->screen, fplinux_pm_canvas_key,
					  peer);
	peer->next = port.peers;
	port.peers = peer;
	return peer->screen;
}

static struct canvas_peer *find_peer(jint id)
{
	for (struct canvas_peer *peer = port.peers; peer; peer = peer->next)
		if ((jint)(intptr_t)peer->screen == id)
			return peer;
	return NULL;
}

/* ---- lcdlf_export.h -------------------------------------------------- */

int lcdlf_ui_init(void)
{
	size_t width = 240;
	size_t height = 320;
	size_t capacity;

	port.ui = fplinux_pm_app_ui();
	if (port.ui) {
		const struct fplinux_ui_metrics *metrics =
			fplinux_ui_metrics(port.ui);
		int surface_width = fplinux_ui_width(port.ui);
		int surface_height = fplinux_ui_height(port.ui);

		width = (size_t)(metrics->screen_width > surface_width ?
					 metrics->screen_width :
					 surface_width);
		height = (size_t)(metrics->screen_height > surface_height ?
					  metrics->screen_height :
					  surface_height);
	}
	capacity = width * height;
	/* A buffer kept for peers of an earlier run is reused if it fits. */
	if (port.pixels && port.capacity < capacity) {
		fputs("fplinux-phoneme: screen buffer still in use\n", stderr);
		return -1;
	}
	if (!port.pixels) {
		if (port.ui) {
			port.frame = fplinux_ui_frame_new(
				port.ui, (int)width, (int)height,
				FPLINUX_UI_FRAME_STAGED);
			if (port.frame) {
				port.owner_ui = port.ui;
				port.pixels = fplinux_ui_frame_pixels(
					port.frame, NULL, NULL);
				lv_obj_set_hidden(
					fplinux_ui_frame_object(port.frame),
					true);
			}
		} else {
			port.pixels = calloc(capacity, sizeof(*port.pixels));
		}
		if (!port.pixels) {
			fprintf(stderr,
				"fplinux-phoneme: cannot create screen frame: %s\n",
				strerror(errno));
			return -1;
		}
		port.capacity = capacity;
	}
	gxj_system_screen_buffer.pixelData = port.pixels;
	gxj_system_screen_buffer.alphaData = NULL;
	gxj_system_screen_buffer.width = 0;
	gxj_system_screen_buffer.height = 0;
	/* A new Canvas has no title until the MIDlet gives it one. */
	port.full_screen = false;
	port.titled = false;
	port.pending = false;
	port.rendered = false;
	port.painting = false;
	port.showing = false;
	port.resized_while_showing = false;
	port.stats = (struct frame_stats){
		.enabled = getenv("FPLINUX_PM_CANVAS_STATS") != NULL,
		.since_ms = monotonic_us() / 1000u,
	};
	port.trace = getenv("FPLINUX_PM_CANVAS_TRACE") != NULL;
	port.started_ms = monotonic_us() / 1000u;
	port.initialized = true;
	apply_size(false);
	if (port.ui) {
		port.flush = lv_timer_create(flush, 1000, NULL);
		if (!port.flush) {
			fputs("fplinux-phoneme: cannot create screen flush timer\n",
			      stderr);
			lcdlf_ui_finalize();
			return -1;
		}
		fplinux_ui_frame_set_timer(port.frame, port.flush, false);
		fplinux_pm_app_set_resize_handler(surface_resized, NULL);
	}
	return 0;
}

void lcdlf_ui_finalize(void)
{
	if (!port.initialized)
		return;
	port.initialized = false;
	if (port.ui)
		fplinux_pm_app_set_resize_handler(NULL, NULL);
	if (port.flush) {
		fplinux_ui_frame_set_timer(port.frame, NULL, false);
		lv_timer_delete(port.flush);
	}
	port.flush = NULL;
	fplinux_pm_keys_cleanup();
	fplinux_pm_device_cleanup();
	port.active = NULL;
	/* Retain storage for peers of an earlier run until their deletion. */
	if (port.frame) {
		lv_obj_t *canvas = fplinux_ui_frame_object(port.frame);

		if (canvas) {
			lv_obj_set_parent(canvas,
					  fplinux_ui_screen(port.owner_ui));
			lv_obj_set_hidden(canvas, true);
		}
	}
	if (!port.peers)
		release_buffers();
	memset(&gxj_system_screen_buffer, 0, sizeof(gxj_system_screen_buffer));
	port.ui = NULL;
}

/* paint() or a GameCanvas flush completed this rectangle of a frame. */
void lcdlf_refresh(int hardwareId, int x1, int y1, int x2, int y2)
{
	(void)hardwareId;
	if (port.trace)
		fprintf(stderr,
			"fplinux-phoneme: refresh at %llu ms %d,%d %dx%d\n",
			(unsigned long long)(monotonic_us() / 1000u -
					     port.started_ms),
			x1, y1, x2 - x1, y2 - y1);
	if (!port.active || x1 >= x2 || y1 >= y2)
		return;
	port.stats.refreshes++;
	if (x2 > 0 && y2 > 0 && x1 < gxj_system_screen_buffer.width &&
	    y1 < gxj_system_screen_buffer.height) {
		const lv_area_t area = { x1, y1, x2 - 1, y2 - 1 };

		fplinux_ui_frame_damage(port.frame, &area);
		port.pending = true;
		port.completed_refresh = true;
	}
	schedule_flush();
	/* The first frame of a slice shows at once, while it is whole. */
	if (!port.rendered)
		render();
}

jboolean lcdlf_reverse_orientation(int hardwareId)
{
	(void)hardwareId;
	return KNI_FALSE;
}

jboolean lcdlf_get_reverse_orientation(int hardwareId)
{
	(void)hardwareId;
	return KNI_FALSE;
}

void lcdlf_handle_clamshell_event(void)
{
}

int lcdlf_get_screen_width(int hardwareId)
{
	(void)hardwareId;
	return gxj_system_screen_buffer.width;
}

int lcdlf_get_screen_height(int hardwareId)
{
	(void)hardwareId;
	return gxj_system_screen_buffer.height;
}

/*
 * Every Displayable passes its mode when it is shown; only a full-screen
 * Canvas asks for true. Asking for the mode already asked for sends nothing.
 */
void lcdlf_set_fullscreen_mode(int hardwareId, jboolean mode)
{
	(void)hardwareId;
	port.full_screen = mode == KNI_TRUE;
	if (port.ui && !fplinux_pm_app_set_fullscreen(port.full_screen)) {
		fprintf(stderr, "fplinux-phoneme: display mode: %s\n",
			strerror(errno));
		/* Refused: the Canvas gets the mode the surface keeps. */
		port.full_screen = fplinux_ui_is_fullscreen(port.ui);
	}
	apply_size(false);
}

void lcdlf_gained_foreground(int hardwareId)
{
	(void)hardwareId;
}

jboolean lcdlf_direct_flush(int hardwareId, const java_graphics *g,
			    const java_imagedata *offscreen_buffer, int h)
{
	(void)hardwareId;
	(void)g;
	(void)offscreen_buffer;
	(void)h;
	return KNI_FALSE; /* Display draws into the screen buffer instead */
}

jboolean lcdlf_is_native_softbutton_layer_supported(void)
{
	return KNI_FALSE;
}

void lcdlf_set_softbutton_label_on_native_layer(unsigned short *label, int len,
						int index)
{
	(void)label;
	(void)len;
	(void)index;
}

int lcdlf_get_current_hardwareId(void)
{
	return 0;
}

char *lcdlf_get_display_name(int hardwareId)
{
	(void)hardwareId;
	return NULL;
}

jboolean lcdlf_is_display_primary(int hardwareId)
{
	(void)hardwareId;
	return KNI_TRUE;
}

jboolean lcdlf_is_display_buildin(int hardwareId)
{
	(void)hardwareId;
	return KNI_TRUE;
}

jboolean lcdlf_is_display_pen_supported(int hardwareId)
{
	(void)hardwareId;
	return KNI_FALSE;
}

jboolean lcdlf_is_display_pen_motion_supported(int hardwareId)
{
	(void)hardwareId;
	return KNI_FALSE;
}

int lcdlf_get_display_capabilities(int hardwareId)
{
	(void)hardwareId;
	return 255; /* All DisplayDevice capabilities. */
}

jint *lcdlf_get_display_device_ids(jint *n)
{
	*n = 1;
	return display_ids;
}

void lcdlf_display_device_state_changed(int hardwareId, int state)
{
	(void)hardwareId;
	(void)state;
}

/* ---- CanvasLFImpl natives -------------------------------------------- */

/* static native int createCanvas0(); a new peer, 0 headless */
KNIEXPORT KNI_RETURNTYPE_INT
KNIDECL(javax_microedition_lcdui_CanvasLFImpl_createCanvas0)
{
	struct fplinux_ui_screen *screen = create_peer();

	if (port.ui && !screen)
		KNI_ThrowNew("java/lang/OutOfMemoryError", "Canvas resources");
	KNI_ReturnInt((jint)(intptr_t)screen);
}

/*
 * static native void prepare0(boolean titled); before a Canvas is shown or
 * its title band changes: the size Display reads next counts the band.
 */
KNIEXPORT KNI_RETURNTYPE_VOID
KNIDECL(javax_microedition_lcdui_CanvasLFImpl_prepare0)
{
	port.titled = KNI_GetParameterAsBoolean(1) == KNI_TRUE;
	if (!port.active) {
		port.showing = true;
		port.resized_while_showing = false;
	}
	apply_size(false);
	KNI_ReturnVoid();
}

static void set_active(struct canvas_peer *peer)
{
	if (peer == port.active)
		return;
	fplinux_pm_keys_stop();
	port.active = peer;
	port.pending = false;
	if (peer) {
		bind(peer);
	} else if (port.frame) {
		lv_obj_t *canvas = fplinux_ui_frame_object(port.frame);

		lv_obj_set_parent(canvas, fplinux_ui_screen(port.owner_ui));
		lv_obj_set_hidden(canvas, true);
		fplinux_ui_frame_set_timer(port.frame, port.flush, false);
	}
	fplinux_pm_device_canvas_shown(peer != NULL);
	if (peer && port.resized_while_showing)
		post_size_changed();
	port.showing = false;
	port.resized_while_showing = false;
}

/*
 * static native void activate0(int peer); the peer of the shown Canvas:
 * refreshes, keys and the display light follow it.
 */
KNIEXPORT KNI_RETURNTYPE_VOID
KNIDECL(javax_microedition_lcdui_CanvasLFImpl_activate0)
{
	struct canvas_peer *peer = find_peer(KNI_GetParameterAsInt(1));

	if (peer)
		set_active(peer);
	KNI_ReturnVoid();
}

/* static native void deactivate0(int peer); its Canvas is hidden. */
KNIEXPORT KNI_RETURNTYPE_VOID
KNIDECL(javax_microedition_lcdui_CanvasLFImpl_deactivate0)
{
	jint id = KNI_GetParameterAsInt(1);

	if (port.active && (jint)(intptr_t)port.active->screen == id)
		set_active(NULL);
	KNI_ReturnVoid();
}

/*
 * static native void titleChanged0(boolean titled); the shown Canvas got or
 * lost its title band: sizeChanged() follows through a ROTATION_EVENT.
 */
KNIEXPORT KNI_RETURNTYPE_VOID
KNIDECL(javax_microedition_lcdui_CanvasLFImpl_titleChanged0)
{
	port.titled = KNI_GetParameterAsBoolean(1) == KNI_TRUE;
	apply_size(true);
	KNI_ReturnVoid();
}

/*
 * static native int predictSize0(boolean fullScreen, boolean titled); the
 * drawing area a Canvas would get, width << 16 | height, for a Canvas that
 * is not shown.
 */
KNIEXPORT KNI_RETURNTYPE_INT
KNIDECL(javax_microedition_lcdui_CanvasLFImpl_predictSize0)
{
	jboolean full_screen = KNI_GetParameterAsBoolean(1);
	jboolean titled = KNI_GetParameterAsBoolean(2);
	int width;
	int height;

	canvas_size(full_screen == KNI_TRUE, titled == KNI_TRUE, &width,
		    &height);
	KNI_ReturnInt((jint)((width & 0xffff) << 16 | (height & 0xffff)));
}

/*
 * static native void painting0(boolean painting); around Canvas.paint():
 * meanwhile the buffer holds a half-drawn frame, which is never shown.
 */
KNIEXPORT KNI_RETURNTYPE_VOID
KNIDECL(javax_microedition_lcdui_CanvasLFImpl_painting0)
{
	port.painting = KNI_GetParameterAsBoolean(1) == KNI_TRUE;
	KNI_ReturnVoid();
}

/* GameCanvasLFImpl: private static native boolean headless0(); */
KNIEXPORT KNI_RETURNTYPE_BOOLEAN
KNIDECL(com_sun_midp_lcdui_GameCanvasLFImpl_headless0)
{
	KNI_ReturnBoolean(fplinux_pm_app_ui() ? KNI_FALSE : KNI_TRUE);
}
