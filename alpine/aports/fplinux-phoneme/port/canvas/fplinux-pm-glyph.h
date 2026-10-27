/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * fplinux-check: package-embedded
 *
 * One line of Canvas text in an LVGL bitmap font, without the VM, phoneME
 * or LVGL headers, so host tests can check it: the font slot of a MIDP font,
 * UTF-16 decoding, the advance of a run of characters and its drawing into an
 * RGB565 buffer. fplinux-pm-text.c supplies a theme font through struct
 * fplinux_pm_font.
 */
#ifndef FPLINUX_PM_GLYPH_H
#define FPLINUX_PM_GLYPH_H

#include <stdbool.h>
#include <stdint.h>

/* javax.microedition.lcdui.Font styles and sizes (gxapi_constants.h). */
enum {
	FPLINUX_PM_STYLE_BOLD = 1,
	FPLINUX_PM_STYLE_UNDERLINED = 4,
	FPLINUX_PM_SIZE_MEDIUM = 0,
	FPLINUX_PM_SIZE_SMALL = 8,
	FPLINUX_PM_SIZE_LARGE = 16,
};

/* The theme font a MIDP font is drawn with. */
enum fplinux_pm_font_slot {
	FPLINUX_PM_FONT_BODY, /* theme font */
	FPLINUX_PM_FONT_LARGE, /* theme font_large, already bold */
	FPLINUX_PM_FONT_SMALL, /* theme font_softkey */
};

struct fplinux_pm_font_choice {
	enum fplinux_pm_font_slot slot;
	/*
	 * Bold over a regular slot: every glyph is drawn one pixel wider and
	 * advances one pixel more, so the line height stays that of plain.
	 */
	bool synthetic_bold;
	bool underline;
};

/*
 * The slot and decorations of a MIDP font style and size (a Font SIZE_*
 * constant). One face serves every Font face; italic is drawn upright.
 */
struct fplinux_pm_font_choice fplinux_pm_font_choose(int style, int size);

/*
 * The code point at chars[*index], n characters in all, advancing *index past
 * it: a surrogate pair is one code point, an unpaired surrogate is U+FFFD.
 */
uint32_t fplinux_pm_utf16_next(const uint16_t *chars, int n, int *index);

/* The LVGL placement of one glyph. */
struct fplinux_pm_glyph {
	int advance; /* to the next pen position, kerning included */
	int box_w, box_h; /* coverage size; 0 for blank glyphs */
	int ofs_x; /* box left from the pen */
	int ofs_y; /* box bottom above the baseline */
};

/* A font: LVGL metrics and callbacks over its glyphs. */
struct fplinux_pm_font {
	int line_height;
	int base_line; /* baseline above the bottom of the line */
	int underline_position; /* underline top from the baseline, < 0 below */
	int underline_thickness;
	/*
	 * Describes letter followed by next (0 at the end of the text) for
	 * kerning; false when the font has no glyph for letter.
	 */
	bool (*glyph)(void *context, uint32_t letter, uint32_t next,
		      struct fplinux_pm_glyph *glyph);
	/*
	 * The A8 coverage of the glyph glyph() found last: box_h rows of box_w
	 * bytes, *stride bytes apart, valid until release(). NULL draws nothing.
	 */
	const uint8_t *(*coverage)(void *context, int *stride);
	void (*release)(void *context);
	void *context;
};

/* An RGB565 pixel buffer, rows width pixels apart. */
struct fplinux_pm_target {
	uint16_t *pixels;
	int width, height;
};

/* A pixel rectangle; x2 and y2 are exclusive. */
struct fplinux_pm_rect {
	int x1, y1, x2, y2;
};

/*
 * The part of a putpixel clip (x1, y1, x2, y2, the last two exclusive) inside
 * a width x height buffer; false when nothing is left.
 */
bool fplinux_pm_clip(const int16_t clip[4], int width, int height,
		     struct fplinux_pm_rect *out);

/* RGB565 bg with fg over it at 8-bit coverage. */
uint16_t fplinux_pm_blend(uint16_t bg, uint16_t fg, unsigned int coverage);

/*
 * Blends color at coverage (box_w x box_h bytes, stride apart) with its top
 * left at x, y, inside clip (within the target). Synthetic bold widens the
 * box by one column, each pixel taking the larger coverage of its own and its
 * left neighbour's. Coordinates must lie within +-2^24.
 */
void fplinux_pm_glyph_blend(const struct fplinux_pm_target *target,
			    const struct fplinux_pm_rect *clip, int x, int y,
			    const uint8_t *coverage, int stride, int box_w,
			    int box_h, bool bold, uint16_t color);

/*
 * The advance of n UTF-16 characters, as fplinux_pm_text_draw() moves:
 * kerning towards the next character, '?' for a character the font lacks
 * and one pixel more per glyph for synthetic bold.
 */
int fplinux_pm_text_width(const struct fplinux_pm_font *font, bool bold,
			  const uint16_t *chars, int n);

/*
 * Draws n UTF-16 characters in color with the top left of the line at x, y,
 * inside clip (within the target); the baseline is line_height - base_line
 * below y. Only pixels change; an underline runs under the whole advance.
 */
void fplinux_pm_text_draw(const struct fplinux_pm_font *font,
			  const struct fplinux_pm_font_choice *choice,
			  const struct fplinux_pm_target *target,
			  const struct fplinux_pm_rect *clip, uint16_t color,
			  int x, int y, const uint16_t *chars, int n);

#endif
