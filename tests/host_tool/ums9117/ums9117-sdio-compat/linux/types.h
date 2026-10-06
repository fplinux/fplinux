// SPDX-License-Identifier: GPL-2.0-only
#ifndef FPLINUX_TEST_LINUX_TYPES_H
#define FPLINUX_TEST_LINUX_TYPES_H

#include <stdbool.h>
#include <stdint.h>

typedef int32_t s32;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef uint16_t __le16;
typedef uint32_t __le32;

#define __packed __attribute__((packed))

#endif
