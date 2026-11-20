/* SPDX-License-Identifier: GPL-2.0-only */
#include <lvgl/lvgl.h>

#include <assert.h>
#include <stdio.h>

static uint16_t draw_buffer[128 * 16];

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
	(void)area;
	(void)pixels;
	lv_display_flush_ready(display);
}

int main(int argc, char **argv)
{
	lv_display_t *display;
	lv_obj_t *label;
	FILE *previous;
	char prefix[14];
	int byte;

	assert(argc == 2);
	lv_init();
	display = lv_display_create(128, 128);
	assert(display);
	lv_display_set_buffers(display, draw_buffer, NULL, sizeof(draw_buffer),
			       LV_DISPLAY_RENDER_MODE_PARTIAL);
	lv_display_set_flush_cb(display, flush);
	label = lv_label_create(lv_display_get_screen_active(display));
	lv_label_set_text(label, "Before refresh");
	lv_refr_now(display);

	previous = fopen(argv[1], "r");
	assert(previous && setvbuf(previous, NULL, _IONBF, 0) == 0);
	assert(fread(prefix, 1, sizeof(prefix), previous) == sizeof(prefix));
	lv_label_set_text(label, "New snapshot");
	lv_refr_now(display);

	assert(fwrite(prefix, 1, sizeof(prefix), stdout) == sizeof(prefix));
	while ((byte = fgetc(previous)) != EOF)
		assert(fputc(byte, stdout) != EOF);
	assert(!ferror(previous) && fclose(previous) == 0);
	lv_deinit();
	return 0;
}
