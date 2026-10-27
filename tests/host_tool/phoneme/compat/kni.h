/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_PHONEME_TEST_KNI_H
#define FPLINUX_PHONEME_TEST_KNI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Host handles retain pointer width; the phone VM supplies ILP32 jint values. */
typedef intptr_t jint;
typedef int jsize;
typedef uint16_t jchar;
typedef void *jobject;

extern jint fplinux_pm_test_parameters[5];

#define KNIEXPORT
#define KNI_RETURNTYPE_VOID void
#define KNI_RETURNTYPE_BOOLEAN bool
#define KNI_RETURNTYPE_INT jint
#define KNI_GetParameterAsInt(index) fplinux_pm_test_parameters[(index) - 1]
#define KNI_GetParameterAsBoolean(index) (KNI_GetParameterAsInt(index) != 0)
#define KNI_ReturnVoid() return
#define KNI_ReturnBoolean(value) return (value)
#define KNI_ReturnInt(value) return (value)
#define KNI_StartHandles(count) ((void)(count))
#define KNI_DeclareHandle(name) jobject name = NULL
#define KNI_GetParameterAsObject(index, handle) ((void)(index), (handle) = NULL)
#define KNI_EndHandles() ((void)0)
#define KNI_IsNullHandle(handle) ((handle) == NULL)

static inline jsize KNI_GetStringLength(jobject string)
{
	(void)string;
	return 0;
}

static inline void KNI_GetStringRegion(jobject string, jsize first,
				       jsize length, jchar *chars)
{
	(void)string;
	(void)first;
	(void)length;
	(void)chars;
}

#endif
