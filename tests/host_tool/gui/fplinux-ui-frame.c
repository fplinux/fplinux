/* SPDX-License-Identifier: GPL-2.0-only */
#include "fplinux-ui-frame.h"
#include "fplinux-ui-frame-private.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>

struct fplinux_ui {
	lv_display_t *display;
	lv_obj_t *root;
	bool visible;
};

struct display {
	struct fplinux_ui ui;
	uint16_t draw[12 * 10];
	uint16_t shown[12 * 10];
	unsigned int flushes;
};

lv_obj_t *fplinux_ui_screen(struct fplinux_ui *ui)
{
	return ui->root;
}

bool fplinux_ui_is_visible(const struct fplinux_ui *ui)
{
	return ui->visible;
}

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
	struct display *output = lv_display_get_user_data(display);
	const uint16_t *source = (const uint16_t *)pixels;

	for (int y = area->y1; y <= area->y2; ++y)
		for (int x = area->x1; x <= area->x2; ++x)
			output->shown[y * 12 + x] = *source++;
	output->flushes++;
	lv_display_flush_ready(display);
}

static void open_display(struct display *output)
{
	struct fplinux_ui *ui = &output->ui;

	ui->display = lv_display_create(12, 10);
	assert(ui->display);
	ui->root = lv_display_get_screen_active(ui->display);
	ui->visible = true;
	lv_display_set_color_format(ui->display, LV_COLOR_FORMAT_RGB565);
	lv_display_set_buffers(ui->display, output->draw, NULL,
			       sizeof(output->draw),
			       LV_DISPLAY_RENDER_MODE_FULL);
	lv_display_set_user_data(ui->display, output);
	lv_display_set_flush_cb(ui->display, flush);
	lv_obj_remove_style_all(ui->root);
	lv_obj_set_style_bg_color(ui->root, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(ui->root, LV_OPA_COVER, 0);
}

static void set_visible(struct fplinux_ui *ui, bool visible)
{
	ui->visible = visible;
	fplinux_ui_frames_set_visible(ui, visible);
}

static void close_display(struct display *output)
{
	set_visible(&output->ui, false);
	lv_display_delete(output->ui.display);
	output->ui.display = NULL;
	output->ui.root = NULL;
}

static void expect_canvas(struct fplinux_ui_frame *frame, int width, int height,
			  const uint16_t *expected)
{
	lv_obj_t *canvas = fplinux_ui_frame_object(frame);
	const lv_image_dsc_t *image = lv_canvas_get_image(canvas);
	const uint16_t *published = lv_canvas_get_buf(canvas);

	assert(image->header.cf == LV_COLOR_FORMAT_RGB565);
	assert(image->header.w == (unsigned int)width);
	assert(image->header.h == (unsigned int)height);
	assert(image->header.stride == (unsigned int)width * 2);
	assert(!memcmp(published, expected,
		       (size_t)width * (size_t)height * sizeof(*expected)));
	lv_obj_update_layout(canvas);
	assert(lv_obj_get_width(canvas) == width);
	assert(lv_obj_get_height(canvas) == height);
}

static void expect_rendered(const struct display *output, int left, int top,
			    int width, int height, const uint16_t *expected)
{
	assert(output->flushes);
	for (int y = 0; y < height; ++y)
		for (int x = 0; x < width; ++x)
			assert(output->shown[(top + y) * 12 + left + x] ==
			       expected[y * width + x]);
}

static void test_direct(struct display *output)
{
	const uint16_t expected[] = { 0xf800, 0x07e0, 0x001f, 0xffff };
	struct fplinux_ui_frame *frame = fplinux_ui_frame_new(
		&output->ui, 2, 2, FPLINUX_UI_FRAME_DIRECT);
	uint16_t *pixels;
	lv_obj_t *canvas;
	int width, height;

	assert(frame);
	pixels = fplinux_ui_frame_pixels(frame, &width, &height);
	assert(pixels && width == 2 && height == 2);
	assert(!memcmp(pixels, (uint16_t[4]){ 0 }, sizeof(expected)));
	canvas = fplinux_ui_frame_object(frame);
	assert(lv_canvas_get_buf(canvas) == pixels);
	lv_obj_set_pos(canvas, 3, 2);
	memcpy(pixels, expected, sizeof(expected));
	assert(fplinux_ui_frame_damage(frame, NULL));
	assert(fplinux_ui_frame_present(frame));
	expect_canvas(frame, 2, 2, expected);
	expect_rendered(output, 3, 2, 2, 2, expected);
	fplinux_ui_frame_delete(frame);
}

static void test_staged(struct display *output)
{
	const uint16_t blue[] = { 0x001f, 0x001f, 0x001f, 0x001f,
				  0x001f, 0x001f, 0x001f, 0x001f,
				  0x001f, 0x001f, 0x001f, 0x001f };
	uint16_t expected[] = { 0x001f, 0x001f, 0x001f, 0x001f, 0x001f, 0x07e0,
				0x001f, 0x001f, 0x001f, 0x001f, 0x001f, 0x001f };
	struct fplinux_ui_frame *frame = fplinux_ui_frame_new(
		&output->ui, 4, 3, FPLINUX_UI_FRAME_STAGED);
	uint16_t *pixels;
	lv_obj_t *canvas;

	assert(frame);
	pixels = fplinux_ui_frame_pixels(frame, NULL, NULL);
	canvas = fplinux_ui_frame_object(frame);
	assert(lv_canvas_get_buf(canvas) != pixels);
	lv_obj_set_pos(canvas, 3, 2);
	memcpy(pixels, blue, sizeof(blue));
	assert(fplinux_ui_frame_damage(frame, NULL));
	assert(fplinux_ui_frame_present(frame));
	pixels[0] = 0xf800;
	pixels[5] = 0x07e0;
	assert(fplinux_ui_frame_damage(frame, &(lv_area_t){ 1, 1, 1, 1 }));
	/* Another UI refresh during painting still draws the completed image. */
	lv_obj_invalidate(output->ui.root);
	lv_refr_now(output->ui.display);
	expect_canvas(frame, 4, 3, blue);
	expect_rendered(output, 3, 2, 4, 3, blue);
	assert(fplinux_ui_frame_present(frame));
	expect_canvas(frame, 4, 3, expected);
	expect_rendered(output, 3, 2, 4, 3, expected);
	assert(fplinux_ui_frame_damage(frame, &(lv_area_t){ -2, -2, 0, 0 }));
	assert(fplinux_ui_frame_present(frame));
	expected[0] = 0xf800;
	expect_canvas(frame, 4, 3, expected);
	pixels[10] = 0xffff;
	assert(fplinux_ui_frame_damage(frame, &(lv_area_t){ 2, 2, 2, 2 }));
	set_visible(&output->ui, false);
	assert(!fplinux_ui_frame_present(frame) && errno == EAGAIN);
	expect_canvas(frame, 4, 3, expected);
	set_visible(&output->ui, true);
	assert(fplinux_ui_frame_present(frame));
	expected[10] = 0xffff;
	expect_canvas(frame, 4, 3, expected);
	pixels[4] = 0xf800;
	pixels[5] = 0xffff;
	pixels[6] = 0x07e0;
	pixels[7] = 0xffff;
	assert(fplinux_ui_frame_damage(frame, &(lv_area_t){ 0, 1, 0, 1 }));
	assert(fplinux_ui_frame_damage(frame, &(lv_area_t){ 2, 1, 2, 1 }));
	assert(fplinux_ui_frame_present(frame));
	expected[4] = 0xf800;
	expected[5] = 0xffff;
	expected[6] = 0x07e0;
	expect_canvas(frame, 4, 3, expected);
	assert(fplinux_ui_frame_damage(frame, &(lv_area_t){ 8, 8, 9, 9 }));
	assert(fplinux_ui_frame_damage(frame, &(lv_area_t){ 2, 1, 1, 1 }));
	assert(fplinux_ui_frame_present(frame));
	expect_canvas(frame, 4, 3, expected);
	fplinux_ui_frame_delete(frame);
}

static void test_resize(struct display *output)
{
	const uint16_t initial[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };
	const uint16_t repainted[] = { 0xf800, 0x07e0, 0x001f,
				       0xffff, 0x0000, 0xffe0 };
	struct fplinux_ui_frame *frame = fplinux_ui_frame_new(
		&output->ui, 4, 3, FPLINUX_UI_FRAME_STAGED);
	struct rlimit original, limited;
	uint16_t *pixels;
	bool resized;
	int error;
	int width, height;

	assert(frame);
	pixels = fplinux_ui_frame_pixels(frame, NULL, NULL);
	memcpy(pixels, initial, sizeof(initial));
	assert(fplinux_ui_frame_damage(frame, NULL));
	assert(fplinux_ui_frame_present(frame));
	assert(fplinux_ui_frame_resize(frame, 4, 3));
	expect_canvas(frame, 4, 3, initial);
	assert(fplinux_ui_frame_damage(frame, NULL));
	assert(fplinux_ui_frame_resize(frame, 3, 2));
	assert(fplinux_ui_frame_pixels(frame, &width, &height) == pixels);
	assert(width == 3 && height == 2);
	expect_canvas(frame, 3, 2, (uint16_t[6]){ 0 });
	/* A geometry change discarded the old damage as well as its image. */
	assert(fplinux_ui_frame_present(frame));
	expect_canvas(frame, 3, 2, (uint16_t[6]){ 0 });
	memcpy(pixels, repainted, sizeof(repainted));
	assert(fplinux_ui_frame_damage(frame, NULL));
	assert(fplinux_ui_frame_present(frame));
	assert(!fplinux_ui_frame_resize(frame, 0, 2) && errno == EINVAL);
	assert(!fplinux_ui_frame_resize(frame, 4097, 2) && errno == EINVAL);
	expect_canvas(frame, 3, 2, repainted);
	/* A 32 MiB buffer cannot fit under this temporary 1 MiB limit. */
	assert(getrlimit(RLIMIT_AS, &original) == 0);
	limited = original;
	limited.rlim_cur = 1024 * 1024;
	assert(setrlimit(RLIMIT_AS, &limited) == 0);
	resized = fplinux_ui_frame_resize(frame, 4096, 4096);
	error = errno;
	assert(setrlimit(RLIMIT_AS, &original) == 0);
	assert(!resized && error == ENOMEM);
	assert(fplinux_ui_frame_pixels(frame, &width, &height) == pixels);
	assert(width == 3 && height == 2);
	assert(!memcmp(pixels, repainted, sizeof(repainted)));
	expect_canvas(frame, 3, 2, repainted);
	assert(fplinux_ui_frame_resize(frame, 5, 3));
	pixels = fplinux_ui_frame_pixels(frame, &width, &height);
	assert(pixels);
	assert(width == 5 && height == 3);
	assert(!memcmp(pixels, (uint16_t[15]){ 0 }, 15 * sizeof(*pixels)));
	expect_canvas(frame, 5, 3, (uint16_t[15]){ 0 });
	fplinux_ui_frame_delete(frame);
}

static void timer_tick(lv_timer_t *timer)
{
	unsigned int *count = lv_timer_get_user_data(timer);

	++*count;
}

static void advance_timers(void)
{
	lv_tick_inc(20);
	lv_timer_handler();
}

static void test_timer(struct display *output)
{
	struct fplinux_ui_frame *frame = fplinux_ui_frame_new(
		&output->ui, 2, 2, FPLINUX_UI_FRAME_DIRECT);
	unsigned int first_count = 0, second_count = 0;
	lv_timer_t *first = lv_timer_create(timer_tick, 10, &first_count);
	lv_timer_t *second = lv_timer_create(timer_tick, 10, &second_count);

	assert(frame && first && second);
	lv_timer_pause(second);
	set_visible(&output->ui, false);
	assert(fplinux_ui_frame_set_timer(frame, first, true));
	assert(lv_timer_get_paused(first));
	advance_timers();
	assert(first_count == 0);
	set_visible(&output->ui, true);
	advance_timers();
	assert(first_count == 1);
	set_visible(&output->ui, false);
	advance_timers();
	assert(first_count == 1);
	assert(fplinux_ui_frame_set_timer(frame, first, false));
	set_visible(&output->ui, true);
	advance_timers();
	assert(first_count == 1);
	assert(fplinux_ui_frame_set_timer(frame, first, true));
	assert(!fplinux_ui_frame_set_timer(frame, NULL, true) &&
	       errno == EINVAL);
	assert(!lv_timer_get_paused(first));
	assert(fplinux_ui_frame_set_timer(frame, second, true));
	assert(lv_timer_get_paused(first));
	advance_timers();
	assert(first_count == 1 && second_count == 1);
	assert(fplinux_ui_frame_set_timer(frame, NULL, false));
	set_visible(&output->ui, false);
	set_visible(&output->ui, true);
	advance_timers();
	assert(second_count == 1 && lv_timer_get_paused(second));
	fplinux_ui_frame_delete(frame);
	/* The application still owns both timers after frame cleanup. */
	lv_timer_resume(first);
	lv_timer_resume(second);
	advance_timers();
	assert(first_count == 2 && second_count == 2);
	lv_timer_delete(first);
	lv_timer_delete(second);
}

static void test_lifetime(struct display *output)
{
	struct fplinux_ui_frame *frame = fplinux_ui_frame_new(
		&output->ui, 2, 2, FPLINUX_UI_FRAME_STAGED);
	lv_obj_t *parent = lv_obj_create(output->ui.root);
	unsigned int count = 0;
	lv_timer_t *timer = lv_timer_create(timer_tick, 10, &count);

	assert(frame && parent && timer);
	lv_obj_set_parent(fplinux_ui_frame_object(frame), parent);
	assert(fplinux_ui_frame_set_timer(frame, timer, true));
	lv_obj_delete(parent);
	assert(!fplinux_ui_frame_object(frame) && errno == ENODEV);
	assert(!fplinux_ui_frame_present(frame) && errno == ENODEV);
	assert(lv_timer_get_paused(timer));
	set_visible(&output->ui, false);
	set_visible(&output->ui, true);
	advance_timers();
	assert(count == 0);
	fplinux_ui_frame_delete(frame);
	lv_timer_resume(timer);
	advance_timers();
	assert(count == 1);
	frame = fplinux_ui_frame_new(&output->ui, 2, 2,
				     FPLINUX_UI_FRAME_DIRECT);
	assert(frame);
	assert(fplinux_ui_frame_set_timer(frame, timer, true));
	fplinux_ui_frame_delete(frame);
	assert(lv_timer_get_paused(timer));
	advance_timers();
	assert(count == 1);
	frame = fplinux_ui_frame_new(&output->ui, 2, 2,
				     FPLINUX_UI_FRAME_DIRECT);
	assert(frame);
	assert(fplinux_ui_frame_set_timer(frame, timer, true));
	close_display(output);
	assert(!fplinux_ui_frame_object(frame) && errno == ENODEV);
	assert(lv_timer_get_paused(timer));
	advance_timers();
	assert(count == 1);
	fplinux_ui_frame_delete(frame);
	lv_timer_resume(timer);
	advance_timers();
	assert(count == 2);
	lv_timer_delete(timer);
}

int main(int argc, char **argv)
{
	struct display output = { 0 };

	if (argc != 2)
		return 2;
	lv_init();
	open_display(&output);
	if (!strcmp(argv[1], "direct"))
		test_direct(&output);
	else if (!strcmp(argv[1], "staged"))
		test_staged(&output);
	else if (!strcmp(argv[1], "resize"))
		test_resize(&output);
	else if (!strcmp(argv[1], "timer"))
		test_timer(&output);
	else if (!strcmp(argv[1], "lifetime"))
		test_lifetime(&output);
	else
		return 2;
	if (output.ui.display)
		close_display(&output);
	lv_deinit();
	printf("%s: ok\n", argv[1]);
	return 0;
}
