/* SPDX-License-Identifier: GPL-2.0-only */
#include "test-quake-video.h"

void GL_Init(void);
void GL_Shutdown(void);
void QPic32_InitPalettes(const byte *palette);
void GL_BeginRendering(int *x, int *y, int *width, int *height);
void GL_EndRendering(void);
qboolean GL_ExtensionCheck_String(const char *extension, const char *search);
