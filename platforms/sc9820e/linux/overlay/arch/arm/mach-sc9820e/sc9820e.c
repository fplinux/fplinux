// SPDX-License-Identifier: GPL-2.0-only
#include <linux/init.h>

#include <asm/mach/arch.h>

#include "fplinux-platform-identity.h"

static const char *const sc9820e_dt_compat[] __initconst = {
	FPLINUX_PLATFORM_COMPATIBLE,
	NULL,
};

DT_MACHINE_START(SC9820E_DT, FPLINUX_PLATFORM_DISPLAY_NAME).dt_compat =
	sc9820e_dt_compat,
			     MACHINE_END
