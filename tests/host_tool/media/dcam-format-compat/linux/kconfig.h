/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_TEST_DCAM_KCONFIG_H
#define FPLINUX_TEST_DCAM_KCONFIG_H

#define __ARG_PLACEHOLDER_1 0,
#define __take_second_arg(__ignored, val, ...) val
#define __is_defined(x) ___is_defined(x)
#define ___is_defined(val) ____is_defined(__ARG_PLACEHOLDER_##val)
#define ____is_defined(arg1_or_junk) __take_second_arg(arg1_or_junk 1, 0)
#define __or(x, y) ___or(x, y)
#define ___or(x, y) ____or(__ARG_PLACEHOLDER_##x, y)
#define ____or(arg1_or_junk, y) __take_second_arg(arg1_or_junk 1, y)
#define IS_ENABLED(option) \
	__or(__is_defined(option), __is_defined(option##_MODULE))

#endif
