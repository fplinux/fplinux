/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_MARLIN2_SETUP_HOST_SLAB_H
#define FPLINUX_MARLIN2_SETUP_HOST_SLAB_H

#include <stddef.h>

#define GFP_KERNEL 0
void *kmalloc(size_t bytes, int flags);
void *kzalloc(size_t bytes, int flags);
void kfree(void *pointer);
#define kzalloc_obj(object, flags) kzalloc(sizeof(object), flags)

#endif
