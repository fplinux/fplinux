/* SPDX-License-Identifier: GPL-2.0-only */
#include "fplinux-status-screen.h"

struct painter {
	const struct fplinux_status_canvas *canvas;
	uint16_t background;
};

static void fill(const struct painter *p, uint32_t x, uint32_t y,
		 uint32_t width, uint32_t height, uint16_t colour)
{
	if (x >= p->canvas->width || y >= p->canvas->height)
		return;
	if (width > p->canvas->width - x)
		width = p->canvas->width - x;
	if (height > p->canvas->height - y)
		height = p->canvas->height - y;
	if (width && height)
		p->canvas->fill(p->canvas->context, x, y, width, height,
				colour);
}

static uint16_t blend(uint16_t foreground, uint16_t background,
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

/* The supplied text uses ASCII and the four UTF-8 arrow characters. */
static unsigned int next_glyph(const char **text)
{
	const unsigned char *s = (const unsigned char *)*text;
	unsigned int index = '?' - ' ';

	if (*s >= 32U && *s <= 126U) {
		index = *s - 32U;
		++s;
	} else if (s[0] == 0xe2U && s[1] && s[1] == 0x86U && s[2] &&
		   s[2] >= 0x90U && s[2] <= 0x93U) {
		index = 95U + s[2] - 0x90U;
		s += 3;
	} else if (*s) {
		++s;
	}
	*text = (const char *)s;
	return index;
}

static unsigned int advance(const struct fplinux_status_font *font,
			    unsigned int left, unsigned int right)
{
	int value = font->glyphs[left].advance;
	unsigned int low = 0, high = font->kerning_count;
	unsigned int key = left * 256U + right;

	while (low < high) {
		unsigned int middle = low + (high - low) / 2U;
		const struct fplinux_status_kern *pair = &font->kerning[middle];
		unsigned int candidate = pair->left * 256U + pair->right;

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
		unsigned int glyph = next_glyph(&s);
		const char *next = s;
		unsigned int following = s < end ? next_glyph(&next) : 255U;

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
		unsigned int index = next_glyph(&s);
		const struct fplinux_status_glyph *glyph = &font->glyphs[index];
		int gx = x + glyph->ofs_x;
		int gy = y + font->line_height - font->base_line -
			 glyph->box_h - glyph->ofs_y;
		const char *next = s;
		unsigned int following = s < end ? next_glyph(&next) : 255U;

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
					     blend(colour, p->background, alpha,
						   15));
			}
		}
		x += (int)advance(font, index, following);
	}
}

static const char *end_of(const char *text)
{
	const char *end = text;

	while (*end)
		++end;
	return end;
}

static void line(const struct painter *p,
		 const struct fplinux_status_font *font, int x, int y,
		 int width, int bottom, const char *text, uint16_t colour)
{
	const char *end, *prefix;
	static const char dots[] = "...";
	unsigned int dots_width;

	if (!text || width <= 0 || y + font->line_height > bottom)
		return;
	end = end_of(text);
	if (span_width(font, text, end) <= (unsigned int)width) {
		draw_span(p, font, x, y, x, y, x + width, bottom, text, end,
			  colour);
		return;
	}
	dots_width = span_width(font, dots, dots + 3);
	if (dots_width > (unsigned int)width)
		return;
	prefix = text;
	while (prefix < end) {
		const char *next = prefix;

		(void)next_glyph(&next);
		if (span_width(font, text, next) + dots_width >
		    (unsigned int)width)
			break;
		prefix = next;
	}
	draw_span(p, font, x, y, x, y, x + width, bottom, text, prefix, colour);
	x += (int)span_width(font, text, prefix);
	draw_span(p, font, x, y, x, y,
		  x + width - (int)span_width(font, text, prefix), bottom, dots,
		  dots + 3, colour);
}

static const char *wrap_end(const struct fplinux_status_font *font,
			    const char *text, int width)
{
	const char *end = text, *word = NULL, *next;

	while (*end && *end != '\n') {
		next = end;
		(void)next_glyph(&next);
		if (span_width(font, text, next) > (unsigned int)width)
			break;
		if (*end == ' ')
			word = end;
		end = next;
	}
	if (*end && *end != '\n' && word)
		end = word;
	if (end == text && *end && *end != '\n')
		(void)next_glyph(&end);
	return end;
}

static int wrapped(const struct painter *p,
		   const struct fplinux_status_font *font, int x, int y,
		   int width, int bottom, const char *text, uint16_t colour)
{
	if (!text || width <= 0)
		return y;
	while (*text && y + font->line_height <= bottom) {
		const char *end;

		while (*text == ' ')
			++text;
		end = wrap_end(font, text, width);
		draw_span(p, font, x, y, x, y, x + width, bottom, text, end,
			  colour);
		text = end;
		if (*text == '\n')
			++text;
		y += font->line_height;
	}
	return y;
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
				     blend(colour,
					   blend(track, p->background, base,
						 255),
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
	uint16_t foreground, muted, accent, border, danger, surface;
	int margin, spacing, width, y, bottom, header;

	if (!canvas || !canvas->fill || !canvas->width || !canvas->height ||
	    canvas->width > 65535U || canvas->height > 65535U || !r ||
	    !r->profile || !r->theme || !r->body || !r->title || !r->legend ||
	    !r->body->bitmap || !r->body->glyphs || !r->title->bitmap ||
	    !r->title->glyphs || !r->legend->bitmap || !r->legend->glyphs ||
	    !r->body->line_height || !r->title->line_height ||
	    !r->legend->line_height || !view || view->level > 10U ||
	    (view->indicator == FPLINUX_STATUS_BUSY && !view->error &&
	     (!r->activity || !r->activity->alpha || !r->activity->track ||
	      !r->activity->size)) ||
	    (view->indicator == FPLINUX_STATUS_PROGRESS && !view->total))
		return -1;
	profile = r->profile;
	margin = profile->margin;
	spacing = profile->spacing;
	if (margin < 0 || spacing < 0 || profile->header_height <= 0 ||
	    profile->header_height > (int)canvas->height ||
	    profile->softkey_height < 0 || profile->bar_height <= 0 ||
	    margin > (int)canvas->width / 2 ||
	    margin * 2 >= (int)canvas->width ||
	    margin + profile->softkey_height > (int)canvas->height)
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
	surface = fplinux_ui_colour565(r->theme, FPLINUX_UI_COLOUR_SURFACE);
	fill(&p, 0, 0, canvas->width, canvas->height, p.background);
	width = (int)canvas->width - 2 * margin;
	bottom = (int)canvas->height - margin;

	if (view->layout != FPLINUX_STATUS_BRIGHTNESS) {
		draw_status(&p, r, view, margin, spacing, width, bottom,
			    foreground, muted, accent, border, danger);
		if (canvas->present)
			canvas->present(canvas->context);
		return 0;
	}
	header = profile->header_height;
	line(&p, r->title, margin, (header - r->title->line_height) / 2, width,
	     header, view->header, foreground);
	fill(&p, 0, (uint32_t)(header - 1), canvas->width, 1, border);
	y = header + spacing;
	line(&p, r->title, margin, y, width, bottom, view->heading,
	     view->error ? danger : foreground);
	y += r->title->line_height + spacing;

	{
		unsigned int gap = (unsigned int)profile->gap_small;
		unsigned int available = (unsigned int)width;
		unsigned int segment;

		if (profile->gap_small < 0 || gap * 9U >= available)
			return -1;
		segment = (available - gap * 9U) / 10U;
		for (unsigned int i = 0; i < 10U; ++i)
			fill(&p, (uint32_t)margin + i * (segment + gap),
			     (uint32_t)y, segment,
			     (uint32_t)profile->bar_height,
			     i < view->level ? accent : surface);
		y += profile->bar_height + spacing;
	}
	if (view->action) {
		bottom -= profile->softkey_height;
		fill(&p, 0, (uint32_t)bottom, canvas->width, 1, border);
		line(&p, r->legend, margin,
		     bottom + (profile->softkey_height -
			       r->legend->line_height) /
				      2,
		     width, (int)canvas->height, view->action, muted);
	}
	(void)wrapped(&p, r->body, margin, y, width, bottom, view->detail,
		      view->error ? danger : muted);
	if (canvas->present)
		canvas->present(canvas->context);
	return 0;
}
