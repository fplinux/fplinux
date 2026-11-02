/* SPDX-License-Identifier: GPL-2.0-only */
/* The VM queue and runtime surface are controlled; LVGL and frame are real. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <kni.h>
#include <lcdlf_export.h>
#include <gxj_screen_buffer.h>
#include <midpEvents.h>
#include <fplinux/fplinux-ui-frame.h>
#include <fplinux/fplinux-ui-screen.h>

#include "fplinux-pm-app.h"
#include "fplinux-pm-canvas.h"
#include "fplinux-pm-vm.h"

extern gxj_screen_buffer gxj_system_screen_buffer;
extern jint javax_microedition_lcdui_CanvasLFImpl_createCanvas0(void);
extern void javax_microedition_lcdui_CanvasLFImpl_prepare0(void);
extern void javax_microedition_lcdui_CanvasLFImpl_activate0(void);
extern void javax_microedition_lcdui_CanvasLFImpl_deactivate0(void);
extern void javax_microedition_lcdui_CanvasLFImpl_painting0(void);

struct fplinux_ui {
	lv_display_t *display;
	lv_obj_t *root;
	struct fplinux_ui_metrics metrics;
	struct fplinux_ui_theme theme;
	int width, height;
	bool fullscreen;
};

struct fplinux_ui_screen {
	lv_obj_t *object;
};

static struct fplinux_ui ui;
static struct fplinux_ui_screen screen;
static fplinux_pm_app_resize_fn resize_handler;
static void *resize_context;
static unsigned int preset;
static MidpEvent last_event;
static unsigned int event_count;
static uint16_t draw[16];
jint fplinux_pm_test_parameters[5];

struct fplinux_ui *fplinux_pm_app_ui(void)
{
	return &ui;
}
unsigned int fplinux_pm_app_canvas_size(void)
{
	return preset;
}
struct fplinux_ui_view_settings fplinux_pm_app_view_settings(void)
{
	return (struct fplinux_ui_view_settings){ FPLINUX_RGB565_FIT,
						  FPLINUX_RGB565_ROTATE_0 };
}
void fplinux_pm_app_loaded(void)
{
}
bool fplinux_pm_app_set_fullscreen(bool fullscreen)
{
	(void)fullscreen;
	return true;
}
void fplinux_pm_app_set_resize_handler(fplinux_pm_app_resize_fn handler,
				       void *context)
{
	resize_handler = handler;
	resize_context = context;
}
enum fplinux_pm_vm_state fplinux_pm_vm_get_state(void)
{
	return FPLINUX_PM_VM_RUNNING;
}
void StoreMIDPEventInVmThread(MidpEvent event, int isolate)
{
	assert(isolate == 0);
	last_event = event;
	++event_count;
}
lv_obj_t *fplinux_ui_screen(struct fplinux_ui *value)
{
	return value->root;
}
int fplinux_ui_width(const struct fplinux_ui *value)
{
	return value->width;
}
int fplinux_ui_height(const struct fplinux_ui *value)
{
	return value->height;
}
bool fplinux_ui_is_visible(const struct fplinux_ui *value)
{
	(void)value;
	return true;
}
bool fplinux_ui_is_fullscreen(const struct fplinux_ui *value)
{
	return value->fullscreen;
}
const struct fplinux_ui_metrics *
fplinux_ui_metrics(const struct fplinux_ui *value)
{
	return &value->metrics;
}
const struct fplinux_ui_theme *fplinux_ui_theme(const struct fplinux_ui *value)
{
	return &value->theme;
}
struct fplinux_ui_screen *fplinux_ui_canvas_new(struct fplinux_ui *value,
						const char *title)
{
	(void)title;
	screen.object = lv_obj_create(value->root);
	lv_obj_remove_style_all(screen.object);
	lv_obj_set_size(screen.object, 4, 4);
	return &screen;
}
lv_obj_t *fplinux_ui_canvas_object(struct fplinux_ui_screen *canvas)
{
	return canvas->object;
}
void fplinux_ui_canvas_set_key_handler(struct fplinux_ui_screen *canvas,
				       fplinux_ui_canvas_key_cb handler,
				       void *context)
{
	(void)canvas;
	(void)handler;
	(void)context;
}
void fplinux_pm_canvas_key(struct fplinux_ui *value,
			   struct fplinux_ui_screen *canvas,
			   const struct fplinux_ui_key *key, void *context)
{
	(void)value;
	(void)canvas;
	(void)key;
	(void)context;
}
void fplinux_pm_keys_stop(void)
{
}
void fplinux_pm_keys_cleanup(void)
{
}
void fplinux_pm_device_canvas_shown(bool shown)
{
	(void)shown;
}
void fplinux_pm_device_cleanup(void)
{
}

static void flush_display(lv_display_t *display, const lv_area_t *area,
			  uint8_t *pixels)
{
	(void)area;
	(void)pixels;
	lv_display_flush_ready(display);
}

static void set_parameter(jint value, void (*native)(void))
{
	fplinux_pm_test_parameters[0] = value;
	native();
}

static void tick(void)
{
	lv_tick_inc(20);
	lv_timer_handler();
}

static void expect_published(const uint16_t expected[16])
{
	lv_obj_t *container = lv_obj_get_child(screen.object, 0);
	lv_obj_t *canvas = lv_obj_get_child(container, 0);
	const lv_image_dsc_t *image = lv_canvas_get_image(canvas);

	assert(image->header.w == 4 && image->header.h == 4);
	assert(!memcmp(lv_canvas_get_buf(canvas), expected,
		       16 * sizeof(uint16_t)));
}

static void test_native_resize(jint peer)
{
	assert(lcdlf_get_screen_width(0) == 4);
	assert(lcdlf_get_screen_height(0) == 4);
	lcdlf_set_fullscreen_mode(0, KNI_TRUE);
	assert(lcdlf_get_screen_width(0) == 4);
	assert(lcdlf_get_screen_height(0) == 6);
	lcdlf_set_fullscreen_mode(0, KNI_FALSE);
	assert(lcdlf_get_screen_height(0) == 4);
	set_parameter(peer, javax_microedition_lcdui_CanvasLFImpl_deactivate0);
	/* The runtime changed its surface while the Canvas was behind Home. */
	ui.width = 3;
	set_parameter(peer, javax_microedition_lcdui_CanvasLFImpl_activate0);
	assert(resize_handler);
	resize_handler(resize_context);
	assert(lcdlf_get_screen_width(0) == 3);
	assert(lcdlf_get_screen_height(0) == 4);
	assert(event_count == 1 && last_event.type == ROTATION_EVENT);
	assert(last_event.DISPLAY == 1);
}

static void test_preset_paint(jint peer)
{
	const uint16_t first[] = {
		0xf800, 0xf800, 0x07e0, 0x07e0, 0xf800, 0xf800, 0x07e0, 0x07e0,
		0x001f, 0x001f, 0xffff, 0xffff, 0x001f, 0x001f, 0xffff, 0xffff,
	};
	const uint16_t second[16] = { 0 };
	uint16_t *pixels = gxj_system_screen_buffer.pixelData;

	assert(lcdlf_get_screen_width(0) == 128);
	assert(lcdlf_get_screen_height(0) == 128);
	lcdlf_set_fullscreen_mode(0, KNI_TRUE);
	assert(lcdlf_get_screen_width(0) == 128);
	assert(lcdlf_get_screen_height(0) == 128);
	lcdlf_set_fullscreen_mode(0, KNI_FALSE);
	for (int y = 0; y < 128; ++y)
		for (int x = 0; x < 128; ++x)
			pixels[y * 128 + x] =
				y < 64 ? (x < 64 ? 0xf800 : 0x07e0) :
					 (x < 64 ? 0x001f : 0xffff);
	lcdlf_refresh(0, 0, 0, 128, 128);
	expect_published(first);
	set_parameter(1, javax_microedition_lcdui_CanvasLFImpl_painting0);
	memset(pixels, 0, 128 * 128 * sizeof(*pixels));
	lcdlf_refresh(0, 0, 0, 128, 128);
	lv_obj_invalidate(ui.root);
	lv_refr_now(ui.display);
	expect_published(first);
	set_parameter(0, javax_microedition_lcdui_CanvasLFImpl_painting0);
	lcdlf_refresh(0, 0, 0, 128, 128);
	tick();
	expect_published(second);
	set_parameter(peer, javax_microedition_lcdui_CanvasLFImpl_deactivate0);
	ui.width = 3;
	set_parameter(peer, javax_microedition_lcdui_CanvasLFImpl_activate0);
	resize_handler(resize_context);
	assert(lcdlf_get_screen_width(0) == 128);
	assert(lcdlf_get_screen_height(0) == 128);
	assert(event_count == 0);
}

int main(int argc, char **argv)
{
	jint peer;

	if (argc != 2)
		return 2;
	preset = strcmp(argv[1], "preset") == 0 ? 1 : 0;
	lv_init();
	ui.display = lv_display_create(4, 4);
	assert(ui.display);
	ui.root = lv_display_get_screen_active(ui.display);
	ui.width = ui.metrics.screen_width = 4;
	ui.height = 4;
	ui.metrics.screen_height = 6;
	ui.metrics.status_height = 1;
	ui.metrics.softkey_height = 1;
	ui.theme.background = lv_color_black();
	lv_display_set_color_format(ui.display, LV_COLOR_FORMAT_RGB565);
	lv_display_set_buffers(ui.display, draw, NULL, sizeof(draw),
			       LV_DISPLAY_RENDER_MODE_FULL);
	lv_display_set_flush_cb(ui.display, flush_display);
	assert(lcdlf_ui_init() == 0);
	peer = javax_microedition_lcdui_CanvasLFImpl_createCanvas0();
	assert(peer);
	set_parameter(0, javax_microedition_lcdui_CanvasLFImpl_prepare0);
	set_parameter(peer, javax_microedition_lcdui_CanvasLFImpl_activate0);
	if (preset)
		test_preset_paint(peer);
	else
		test_native_resize(peer);
	lcdlf_ui_finalize();
	lv_display_delete(ui.display);
	lv_deinit();
	printf("%s: ok\n", argv[1]);
	return 0;
}
