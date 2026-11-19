// SPDX-License-Identifier: GPL-2.0-only
/* fplinux-check: package-embedded */
#include <stdint.h>
#include <stddef.h>
#include <kni.h>
#include <sni.h>
#include <gxj_screen_buffer.h>

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
