/* SPDX-License-Identifier: GPL-2.0-only */
#include "fplinux-status-screen.h"

struct painter {
	const struct fplinux_status_canvas *canvas;
	uint16_t background;
};

void fplinux_status_fill(const struct fplinux_status_canvas *canvas, uint32_t x,
			 uint32_t y, uint32_t width, uint32_t height,
			 uint16_t colour)
{
	if (x >= canvas->width || y >= canvas->height)
		return;
	if (width > canvas->width - x)
		width = canvas->width - x;
	if (height > canvas->height - y)
		height = canvas->height - y;
	if (width && height)
		canvas->fill(canvas->context, x, y, width, height, colour);
}

static void fill(const struct painter *p, uint32_t x, uint32_t y,
		 uint32_t width, uint32_t height, uint16_t colour)
{
	fplinux_status_fill(p->canvas, x, y, width, height, colour);
}

uint16_t fplinux_status_blend(uint16_t foreground, uint16_t background,
			      unsigned int alpha, unsigned int scale)
{
	unsigned int r =
		(((foreground >> 11) & 31U) * alpha +
		 ((background >> 11) & 31U) * (scale - alpha) + scale / 2U) /
		scale;
	unsigned int g =
		(((foreground >> 5) & 63U) * alpha +
		 ((background >> 5) & 63U) * (scale - alpha) + scale / 2U) /
		scale;
	unsigned int b = ((foreground & 31U) * alpha +
			  (background & 31U) * (scale - alpha) + scale / 2U) /
			 scale;

	return (uint16_t)((r << 11) | (g << 5) | b);
}

static unsigned int next_glyph(const struct fplinux_status_font *font,
			       const char **text)
{
	const unsigned char *s = (const unsigned char *)*text;
	unsigned int code = *s;
	unsigned int trailing = 0;
	unsigned int low = 0, high = font->extended_count;

	if (!code)
		return '?' - ' ';
	++s;
	if (code >= 0xc2U && code <= 0xdfU) {
		trailing = 1;
		code &= 0x1fU;
	} else if (code >= 0xe0U && code <= 0xefU) {
		trailing = 2;
		code &= 0x0fU;
	} else if (code >= 0xf0U && code <= 0xf4U) {
		trailing = 3;
		code &= 0x07U;
	}
	for (unsigned int i = 0; i < trailing; ++i) {
		if ((*s & 0xc0U) != 0x80U) {
			code = 0;
			break;
		}
		code = (code << 6) | (*s++ & 0x3fU);
	}
	*text = (const char *)s;
	if (code >= 32U && code <= 126U)
		return code - 32U;
	if (code >= 0x2190U && code <= 0x2193U)
		return 95U + code - 0x2190U;
	while (low < high) {
		unsigned int middle = low + (high - low) / 2U;
		unsigned int candidate = font->extended_codes[middle];

		if (candidate < code)
			low = middle + 1U;
		else
			high = middle;
	}
	if (low < font->extended_count && font->extended_codes[low] == code)
		return 99U + low;
	return '?' - ' ';
}

static unsigned int advance(const struct fplinux_status_font *font,
			    unsigned int left, unsigned int right)
{
	int value = font->glyphs[left].advance;
	unsigned int low = 0, high = font->kerning_count;
	unsigned int key = left * 65536U + right;

	while (low < high) {
		unsigned int middle = low + (high - low) / 2U;
		const struct fplinux_status_kern *pair = &font->kerning[middle];
		unsigned int candidate = pair->left * 65536U + pair->right;

		if (candidate < key)
			low = middle + 1U;
		else
			high = middle;
	}
	if (low < font->kerning_count && font->kerning[low].left == left &&
	    font->kerning[low].right == right)
		value += font->kerning[low].adjustment;
	return value > 0 ? (unsigned int)(value + 8) >> 4 : 0;
}

static unsigned int span_width(const struct fplinux_status_font *font,
			       const char *first, const char *end)
{
	unsigned int width = 0;
	const char *s = first;

	while (s < end) {
		unsigned int glyph = next_glyph(font, &s);
		const char *next = s;
		unsigned int following = s < end ? next_glyph(font, &next) :
						   UINT16_MAX;

		width += advance(font, glyph, following);
	}
	return width;
}

static void draw_span(const struct painter *p,
		      const struct fplinux_status_font *font, int x, int y,
		      int left, int top, int right, int bottom,
		      const char *first, const char *end, uint16_t colour)
{
	const char *s = first;

	while (s < end) {
		unsigned int index = next_glyph(font, &s);
		const struct fplinux_status_glyph *glyph = &font->glyphs[index];
		int gx = x + glyph->ofs_x;
		int gy = y + font->line_height - font->base_line -
			 glyph->box_h - glyph->ofs_y;
		const char *next = s;
		unsigned int following = s < end ? next_glyph(font, &next) :
						   UINT16_MAX;

		for (unsigned int row = 0; row < glyph->box_h; ++row) {
			for (unsigned int col = 0; col < glyph->box_w; ++col) {
				unsigned int pixel = row * glyph->box_w + col;
				uint8_t byte =
					font->bitmap[glyph->bitmap_offset +
						     pixel / 2U];
				unsigned int alpha = pixel % 2U ? byte & 15U :
								  byte >> 4;
				int px = gx + (int)col, py = gy + (int)row;

				if (alpha && px >= left && px < right &&
				    py >= top && py < bottom)
					fill(p, (uint32_t)px, (uint32_t)py, 1,
					     1,
					     fplinux_status_blend(colour,
								  p->background,
								  alpha, 15));
			}
		}
		x += (int)advance(font, index, following);
	}
}

unsigned int fplinux_status_text_width(const struct fplinux_status_font *font,
				       const char *text)
{
	const char *end = text;

	while (*end)
		++end;
	return span_width(font, text, end);
}

void fplinux_status_text_draw(const struct fplinux_status_canvas *canvas,
			      const struct fplinux_status_font *font, int x,
			      int y, const char *text, uint16_t colour,
			      uint16_t background)
{
	const struct painter p = {
		.canvas = canvas,
		.background = background,
	};
	const char *end = text;

	while (*end)
		++end;
	draw_span(&p, font, x, y, 0, 0, (int)canvas->width, (int)canvas->height,
		  text, end, colour);
}

static const char *wrap_end(const struct fplinux_status_font *font,
			    const char *text, int width)
{
	const char *end = text, *word = NULL, *next;

	while (*end && *end != '\n') {
		next = end;
		(void)next_glyph(font, &next);
		if (span_width(font, text, next) > (unsigned int)width)
			break;
		if (*end == ' ')
			word = end;
		end = next;
	}
	if (*end && *end != '\n' && word)
		end = word;
	if (end == text && *end && *end != '\n')
		(void)next_glyph(font, &end);
	return end;
}

/* Measure and draw with the same word boundaries and glyph advances. */
static int centred_text(const struct painter *p,
			const struct fplinux_status_font *font, int x, int y,
			int width, int bottom, const char *text,
			uint16_t colour)
{
	int start = y;

	if (!text || width <= 0)
		return 0;
	while (*text) {
		const char *end;
		unsigned int text_width;

		while (*text == ' ')
			++text;
		if (!*text)
			break;
		end = wrap_end(font, text, width);
		text_width = span_width(font, text, end);
		if (p && y + font->line_height <= bottom)
			draw_span(p, font, x + (width - (int)text_width) / 2, y,
				  x, y, x + width, bottom, text, end, colour);
		text = end;
		if (*text == '\n')
			++text;
		y += font->line_height;
	}
	return y - start;
}

unsigned int fplinux_status_scale(uint64_t completed, uint64_t total,
				  unsigned int scale)
{
	unsigned int low = 0, high = scale;

	if (!total || !scale)
		return 0;
	if (completed >= total)
		return scale;
	while (low < high) {
		unsigned int mid = low + (high - low + 1U) / 2U;
		uint64_t threshold =
			total / scale * mid +
			((total % scale) * mid + scale - 1U) / scale;

		if (completed >= threshold)
			low = mid;
		else
			high = mid - 1U;
	}
	return low;
}

static void draw_mark(const struct painter *p,
		      const struct fplinux_status_mark *mark, unsigned int y)
{
	unsigned int x = 0, row = 0;
	unsigned int left = (p->canvas->width - mark->size) / 2U;

	for (size_t i = 0; i < mark->run_count && row < mark->size; ++i) {
		unsigned int index = mark->runs[i] >> 4;
		unsigned int count = (mark->runs[i] & 15U) + 1U;

		if (count > mark->size - x)
			return;
		if (index)
			fill(p, left + x, y + row, count, 1,
			     mark->palette[index]);
		x += count;
		if (x == mark->size) {
			x = 0;
			++row;
		}
	}
}

static void draw_activity(const struct painter *p,
			  const struct fplinux_status_mask *mask,
			  unsigned int x, unsigned int y, uint16_t colour,
			  uint16_t track)
{
	for (unsigned int row = 0; row < mask->size; ++row)
		for (unsigned int col = 0; col < mask->size; ++col) {
			unsigned int alpha =
				mask->alpha[row * mask->size + col];
			unsigned int base = mask->track[row * mask->size + col];

			if (alpha || base)
				fill(p, x + col, y + row, 1, 1,
				     fplinux_status_blend(
					     colour,
					     fplinux_status_blend(track,
								  p->background,
								  base, 255),
					     alpha, 255));
		}
}

static void draw_status(const struct painter *p,
			const struct fplinux_status_resources *r,
			const struct fplinux_status_view *view, int margin,
			int spacing, int width, int bottom, uint16_t foreground,
			uint16_t muted, uint16_t accent, uint16_t border,
			uint16_t danger)
{
	const struct fplinux_ui_profile_spec *profile = r->profile;
	int heading_height =
		centred_text(NULL, r->title, 0, 0, width, 0, view->heading, 0);
	int detail_height =
		centred_text(NULL, r->body, 0, 0, width, 0, view->detail, 0);
	int y, text_spacing = spacing;

	if (view->layout == FPLINUX_STATUS_BOOT && !view->error && r->mark &&
	    r->mark->runs && r->mark->palette &&
	    r->mark->size <= p->canvas->width &&
	    r->mark->size <= p->canvas->height) {
		y = (int)fplinux_status_mark_top(p->canvas->height,
						 r->mark->size);
		draw_mark(p, r->mark, (unsigned int)y);
		y += (int)r->mark->size;
		if (detail_height &&
		    bottom - y - heading_height - detail_height <
			    2 * text_spacing) {
			text_spacing =
				(bottom - y - heading_height - detail_height) /
				2;
			if (text_spacing < 0)
				text_spacing = 0;
		}
		y += text_spacing;
	} else if (!view->error && view->layout == FPLINUX_STATUS_PREPARATION &&
		   view->indicator != FPLINUX_STATUS_NONE) {
		/* A fixed reading envelope keeps active title and indicator still. */
		int height = 2 * r->title->line_height + profile->bar_height +
			     3 * r->body->line_height + 4 * spacing;

		y = ((int)p->canvas->height - height) / 2;
		if (y < margin)
			y = margin;
	} else {
		int height = heading_height + (detail_height ? spacing : 0) +
			     detail_height;

		y = ((int)p->canvas->height - height) / 2;
		if (y < margin)
			y = margin;
	}
	(void)centred_text(p, r->title, margin, y, width, bottom, view->heading,
			   view->error ? danger : foreground);
	if (!view->error && view->layout == FPLINUX_STATUS_PREPARATION &&
	    view->indicator != FPLINUX_STATUS_NONE &&
	    heading_height < 2 * r->title->line_height)
		heading_height = 2 * r->title->line_height;
	y += heading_height + text_spacing;
	if (!view->error && view->indicator == FPLINUX_STATUS_PROGRESS) {
		fill(p, (uint32_t)margin, (uint32_t)y, (uint32_t)width,
		     (uint32_t)profile->bar_height, border);
		fill(p, (uint32_t)margin, (uint32_t)y,
		     fplinux_status_scale(view->completed, view->total,
					  (unsigned int)width),
		     (uint32_t)profile->bar_height, accent);
		y += profile->bar_height + spacing;
		y += centred_text(p, r->body, margin, y, width, bottom,
				  view->progress_percent, muted) +
		     spacing;
		y += centred_text(p, r->body, margin, y, width, bottom,
				  view->progress_bytes, muted) +
		     spacing;
	} else if (!view->error && view->indicator == FPLINUX_STATUS_BUSY &&
		   r->activity && r->activity->size <= (unsigned int)width &&
		   y < bottom &&
		   r->activity->size <= (unsigned int)(bottom - y)) {
		draw_activity(
			p, r->activity,
			(unsigned int)(margin +
				       (width - (int)r->activity->size) / 2),
			(unsigned int)y, accent, border);
		y += (int)r->activity->size + spacing;
	}
	(void)centred_text(p, r->body, margin, y, width, bottom, view->detail,
			   muted);
}

static void surface_fill(void *context, uint32_t x, uint32_t y, uint32_t width,
			 uint32_t height, uint16_t colour)
{
	struct fplinux_status_surface *surface = context;

	for (uint32_t row = y; row < y + height; ++row) {
		uint16_t *pixels =
			(uint16_t *)((uint8_t *)surface->pixels +
				     (size_t)row * surface->stride_bytes);

		for (uint32_t col = x; col < x + width; ++col)
			pixels[col] = colour;
	}
}

int fplinux_status_surface_canvas(struct fplinux_status_canvas *canvas,
				  struct fplinux_status_surface *surface)
{
	if (!canvas || !surface || !surface->pixels || !surface->width ||
	    !surface->height || surface->width > 65535U ||
	    surface->height > 65535U ||
	    surface->stride_bytes < (size_t)surface->width * sizeof(uint16_t) ||
	    surface->stride_bytes % sizeof(uint16_t) ||
	    surface->stride_bytes > SIZE_MAX / surface->height)
		return -1;
	canvas->width = surface->width;
	canvas->height = surface->height;
	canvas->context = surface;
	canvas->fill = surface_fill;
	canvas->present = NULL;
	return 0;
}

int fplinux_status_screen_render(const struct fplinux_status_canvas *canvas,
				 const struct fplinux_status_resources *r,
				 const struct fplinux_status_view *view)
{
	const struct fplinux_ui_profile_spec *profile;
	struct painter p;
	uint16_t foreground, muted, accent, border, danger;
	int margin, spacing, width, bottom;

	if (!canvas || !canvas->fill || !canvas->width || !canvas->height ||
	    canvas->width > 65535U || canvas->height > 65535U || !r ||
	    !r->profile || !r->theme || !r->body || !r->title ||
	    !r->body->bitmap || !r->body->glyphs || !r->title->bitmap ||
	    !r->title->glyphs || !r->body->line_height ||
	    !r->title->line_height || !view ||
	    (view->indicator == FPLINUX_STATUS_BUSY && !view->error &&
	     (!r->activity || !r->activity->alpha || !r->activity->track ||
	      !r->activity->size)) ||
	    (view->indicator == FPLINUX_STATUS_PROGRESS && !view->total))
		return -1;
	profile = r->profile;
	margin = profile->margin;
	spacing = profile->spacing;
	if (margin < 0 || spacing < 0 || profile->bar_height <= 0 ||
	    margin > (int)canvas->width / 2 || margin * 2 >= (int)canvas->width)
		return -1;
	p.canvas = canvas;
	p.background =
		fplinux_ui_colour565(r->theme, FPLINUX_UI_COLOUR_BACKGROUND);
	foreground =
		fplinux_ui_colour565(r->theme, FPLINUX_UI_COLOUR_FOREGROUND);
	muted = fplinux_ui_colour565(r->theme, FPLINUX_UI_COLOUR_MUTED);
	accent = fplinux_ui_colour565(r->theme, FPLINUX_UI_COLOUR_FOCUS);
	border = fplinux_ui_colour565(r->theme, FPLINUX_UI_COLOUR_BORDER);
	danger = fplinux_ui_colour565(r->theme, FPLINUX_UI_COLOUR_DANGER);
	fill(&p, 0, 0, canvas->width, canvas->height, p.background);
	width = (int)canvas->width - 2 * margin;
	bottom = (int)canvas->height - margin;

	draw_status(&p, r, view, margin, spacing, width, bottom, foreground,
		    muted, accent, border, danger);
	if (canvas->present)
		canvas->present(canvas->context);
	return 0;
}
