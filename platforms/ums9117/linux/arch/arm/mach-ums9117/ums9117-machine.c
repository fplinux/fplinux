// SPDX-License-Identifier: GPL-2.0-only
/* Minimal DT machine descriptor for Unisoc UMS9117. */
#include <asm/mach/arch.h>

#include "fplinux-platform-identity.h"

static const char *const ums9117_dt_compat[] __initconst = {
	FPLINUX_PLATFORM_COMPATIBLE,
	NULL,
};

DT_MACHINE_START(UMS9117_DT, FPLINUX_PLATFORM_DISPLAY_NAME).dt_compat =
	ums9117_dt_compat,
			     MACHINE_END
