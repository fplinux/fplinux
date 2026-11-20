// SPDX-License-Identifier: GPL-2.0-only AND MIT
// SPDX-FileCopyrightText: 2026 Roman Lahin
// SPDX-FileCopyrightText: 2026 Yury Kharchenko
/* fplinux-check: package-embedded */
#include <kni.h>
#include <sni.h>
#include <stdint.h>

enum {
	FRACTION_BITS = 12,
	FIXED_ONE = 1 << FRACTION_BITS,
	FIXED_INTEGER_LIMIT = 1 << (31 - FRACTION_BITS),
	PALETTE_SIZE = 256,
};

struct span_edges {
	uint32_t left, left_step, right, right_step;
	uint32_t u, u_row_step, u_step;
	uint32_t v, v_row_step, v_step;
};

static int32_t fixed_floor(uint32_t value)
{
	/* Fixed-point products and steps wrap as Java ints before the shift. */
	_Static_assert(
		(int32_t)UINT32_MAX == -1 && (-1 >> 1) == -1,
		"Java fixed-point conversion requires a signed right shift");
	return (int32_t)value >> FRACTION_BITS;
}

static void fill_texture_spans(jint *frame, int width, int first, int end,
			       int clip_left, int clip_right,
			       struct span_edges edges, const uint8_t *bitmap,
			       uint32_t bitmap_mask, unsigned int width_shift,
			       const jint *palette,
			       unsigned int transparent_index)
{
	if (edges.u_row_step)
		edges.u += (int32_t)edges.u_row_step > 0 ? 1U : UINT32_MAX;
	if (edges.v_row_step)
		edges.v += (int32_t)edges.v_row_step > 0 ? 1U : UINT32_MAX;

	for (int row = first; row < end; row += width) {
		int left = fixed_floor(edges.left);
		int right = fixed_floor(edges.right);
		uint32_t subpixel = FIXED_ONE - (edges.left & (FIXED_ONE - 1));
		uint32_t u = edges.u + fixed_floor(edges.u_step * subpixel);
		uint32_t v = edges.v + fixed_floor(edges.v_step * subpixel);

		if (left < clip_left) {
			uint32_t skipped = clip_left - left;

			u += edges.u_step * skipped;
			v += edges.v_step * skipped;
			left = clip_left;
		}
		if (right > clip_right)
			right = clip_right;

		for (int x = left; x < right; x++) {
			uint32_t texel =
				((uint32_t)fixed_floor(v) << width_shift) |
				(uint32_t)fixed_floor(u);
			unsigned int index = bitmap[texel & bitmap_mask];

			if (index != transparent_index)
				frame[row + x] = palette[index];
			u += edges.u_step;
			v += edges.v_step;
		}

		edges.left += edges.left_step;
		edges.right += edges.right_step;
		edges.u += edges.u_row_step;
		edges.v += edges.v_row_step;
	}
}

KNIEXPORT KNI_RETURNTYPE_BOOLEAN
KNIDECL(com_mascotcapsule_micro3d_v3_Rasterizer_fillTextureSpans0)
{
	int width = KNI_GetParameterAsInt(4);
	int clip_left = KNI_GetParameterAsInt(5);
	int clip_right = KNI_GetParameterAsInt(6);
	int first_row = KNI_GetParameterAsInt(7);
	int end_row = KNI_GetParameterAsInt(8);
	int64_t end = (int64_t)end_row * width;
	struct span_edges edges = {
		.left = KNI_GetParameterAsInt(9),
		.left_step = KNI_GetParameterAsInt(10),
		.right = KNI_GetParameterAsInt(11),
		.right_step = KNI_GetParameterAsInt(12),
		.u = KNI_GetParameterAsInt(13),
		.u_row_step = KNI_GetParameterAsInt(14),
		.u_step = KNI_GetParameterAsInt(15),
		.v = KNI_GetParameterAsInt(16),
		.v_row_step = KNI_GetParameterAsInt(17),
		.v_step = KNI_GetParameterAsInt(18),
	};
	unsigned int width_shift = (uint32_t)KNI_GetParameterAsInt(19) & 31U;
	unsigned int transparent_index =
		KNI_GetParameterAsBoolean(20) ? 0 : PALETTE_SIZE;
	int shade = KNI_GetParameterAsInt(21);
	jboolean handled = KNI_FALSE;

	KNI_StartHandles(3);
	KNI_DeclareHandle(frame);
	KNI_DeclareHandle(bitmap);
	KNI_DeclareHandle(palette);
	KNI_GetParameterAsObject(1, frame);
	KNI_GetParameterAsObject(2, bitmap);
	KNI_GetParameterAsObject(3, palette);

	/* Keep even off-clip Java row additions and span widths in int range. */
	if (width <= 0 || clip_left < 0 || clip_right < clip_left ||
	    clip_right > width || first_row < 0 || end_row < first_row ||
	    end > INT32_MAX - FIXED_INTEGER_LIMIT || KNI_IsNullHandle(frame) ||
	    end > KNI_GetArrayLength(frame) || KNI_IsNullHandle(bitmap) ||
	    KNI_IsNullHandle(palette) || shade < 0 ||
	    (shade & (PALETTE_SIZE - 1)))
		goto done;

	int bitmap_length = KNI_GetArrayLength(bitmap);
	if (bitmap_length <= 0 ||
	    shade > KNI_GetArrayLength(palette) - PALETTE_SIZE)
		goto done;

	SNI_BEGIN_RAW_POINTERS
	const jint *colors = SNI_GetRawArrayPointer(palette);
	fill_texture_spans(SNI_GetRawArrayPointer(frame), width,
			   first_row * width, (int)end, clip_left, clip_right,
			   edges, SNI_GetRawArrayPointer(bitmap),
			   bitmap_length - 1, width_shift, colors + shade,
			   transparent_index);
	SNI_END_RAW_POINTERS
	handled = KNI_TRUE;

done:
	KNI_EndHandles();
	KNI_ReturnBoolean(handled);
}
