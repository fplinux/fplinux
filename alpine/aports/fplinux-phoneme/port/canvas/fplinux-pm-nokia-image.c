// SPDX-License-Identifier: GPL-2.0-only
/* fplinux-check: package-embedded */
#include <stdint.h>
#include <stddef.h>
#include <kni.h>
#include <sni.h>
#include <gxj_screen_buffer.h>
#include <gxapi_graphics.h>
#include <gxapi_intern_graphics.h>

static void draw_4444_rows(const java_graphics *graphics, const jshort *pixels,
			   int offset, int stride, int x, int y, int width,
			   int height, jboolean alpha)
{
	const java_imagedata *image = graphics->img ? graphics->img->imageData :
						      NULL;
	const jshort clip[] = { graphics->clipX1, graphics->clipY1,
				graphics->clipX2, graphics->clipY2 };
	int left = x > clip[0] ? x : clip[0];
	int top = y > clip[1] ? y : clip[1];
	int right = x + width < clip[2] ? x + width : clip[2];
	int bottom = y + height < clip[3] ? y + height : clip[3];
	enum { SPAN = 128 };
	jint argb[SPAN];

	if (left >= right || top >= bottom)
		return;
	for (int row = top; row < bottom; ++row) {
		const jshort *source =
			pixels +
			(offset + (int64_t)(row - y) * stride + (left - x));

		for (int col = left; col < right; col += SPAN) {
			int count = right - col < SPAN ? right - col : SPAN;

			for (int i = 0; i < count; ++i) {
				uint32_t p = (uint16_t)*source++;
				uint32_t channels = (p & 0xf000) << 12 |
						    (p & 0xf00) << 8 |
						    (p & 0xf0) << 4 | (p & 0xf);

				argb[i] = (jint)(channels * 17U);
			}
			gx_draw_rgb(clip, image, argb, 0, count, col, row,
				    count, 1, alpha);
		}
	}
}

KNIEXPORT KNI_RETURNTYPE_BOOLEAN KNIDECL(com_nokia_mid_ui_DirectUtils_draw4444)
{
	int offset = KNI_GetParameterAsInt(3);
	int stride = KNI_GetParameterAsInt(4);
	int x = KNI_GetParameterAsInt(5);
	int y = KNI_GetParameterAsInt(6);
	int width = KNI_GetParameterAsInt(7);
	int height = KNI_GetParameterAsInt(8);
	jboolean alpha = KNI_GetParameterAsBoolean(9);
	jboolean handled = KNI_FALSE;

	KNI_StartHandles(2);
	KNI_DeclareHandle(graphics);
	KNI_DeclareHandle(pixels);
	KNI_GetParameterAsObject(1, graphics);
	KNI_GetParameterAsObject(2, pixels);
	if (!KNI_IsNullHandle(graphics) && !KNI_IsNullHandle(pixels) &&
	    width > 0 && height > 0) {
		const java_graphics *g = GXAPI_GET_GRAPHICS_PTR(graphics);
		int64_t left = (int64_t)x + g->transX;
		int64_t top = (int64_t)y + g->transY;
		int64_t last = offset + ((int64_t)height - 1) * stride;
		int length = KNI_GetArrayLength(pixels);

		/* Other coordinates keep the original drawRGB integer arithmetic. */
		if (left >= INT16_MIN && left + width <= INT16_MAX &&
		    top >= INT16_MIN && top + height <= INT16_MAX &&
		    offset >= 0 && (int64_t)offset + width <= length &&
		    last >= 0 && last + width <= length) {
			if (GRAPHICS_OP_IS_ALLOWED(graphics)) {
				SNI_BEGIN_RAW_POINTERS
				draw_4444_rows(g,
					       SNI_GetRawArrayPointer(pixels),
					       offset, stride, (int)left,
					       (int)top, width, height, alpha);
				SNI_END_RAW_POINTERS
			}
			handled = KNI_TRUE;
		}
	}
	KNI_EndHandles();
	KNI_ReturnBoolean(handled);
}

KNIEXPORT KNI_RETURNTYPE_VOID
KNIDECL(javax_microedition_lcdui_FPLinuxNokiaImage_readScreen0)
{
	const gxj_screen_buffer *screen = &gxj_system_screen_buffer;
	int offset = KNI_GetParameterAsInt(2);
	int stride = KNI_GetParameterAsInt(3);
	int x = KNI_GetParameterAsInt(4);
	int y = KNI_GetParameterAsInt(5);
	int width = KNI_GetParameterAsInt(6);
	int height = KNI_GetParameterAsInt(7);
	int64_t last = offset + ((int64_t)height - 1) * stride;
	int64_t first = offset < last ? offset : last;
	int64_t end = (offset > last ? offset : last) + width;

	KNI_StartHandles(1);
	KNI_DeclareHandle(pixels);
	KNI_GetParameterAsObject(1, pixels);
	if (KNI_IsNullHandle(pixels)) {
		KNI_ThrowNew("java/lang/NullPointerException", NULL);
	} else if (x < 0 || y < 0 || width < 0 || height < 0 ||
		   (int64_t)x + width > screen->width ||
		   (int64_t)y + height > screen->height || !screen->pixelData) {
		KNI_ThrowNew("java/lang/IllegalArgumentException",
			     "pixel region");
	} else if (width && height) {
		if (first < 0 || end > KNI_GetArrayLength(pixels)) {
			KNI_ThrowNew("java/lang/ArrayIndexOutOfBoundsException",
				     NULL);
		} else {
			jint *target = SNI_GetRawArrayPointer(pixels);

			for (int row = 0; row < height; ++row) {
				const gxj_pixel_type *source =
					screen->pixelData +
					(y + row) * screen->width + x;
				jint *line = target + offset + row * stride;

				for (int col = 0; col < width; ++col)
					line[col] =
						(jint)(0xff000000U |
						       GXJ_RGB16TORGB24(
							       source[col]));
			}
		}
	}
	KNI_EndHandles();
	KNI_ReturnVoid();
}
