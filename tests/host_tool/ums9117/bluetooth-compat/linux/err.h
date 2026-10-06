/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef BLUETOOTH_HOST_ERR_H
#define BLUETOOTH_HOST_ERR_H

#include <stdint.h>

#define ERR_PTR(error) ((void *)(intptr_t)(error))
#define PTR_ERR(pointer) ((long)(intptr_t)(pointer))
#define IS_ERR(pointer) ((uintptr_t)(void *)(pointer) >= (uintptr_t)-4095)

#endif
