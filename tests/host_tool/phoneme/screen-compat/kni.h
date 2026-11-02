/* SPDX-License-Identifier: GPL-2.0-only */
#include "../compat/kni.h"

typedef bool jboolean;
#define KNI_TRUE true
#define KNI_FALSE false
#define KNIDECL(name) name(void)
#define KNI_ThrowNew(class_name, message) abort()
