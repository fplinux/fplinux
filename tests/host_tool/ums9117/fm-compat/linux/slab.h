/* SPDX-License-Identifier: GPL-2.0-only */
#include <stdlib.h>
#define GFP_KERNEL 0
#define kzalloc(bytes, flags) calloc(1, bytes)
#define kzalloc_obj(object, flags) kzalloc(sizeof(object), flags)
#define kfree(ptr) free(ptr)
