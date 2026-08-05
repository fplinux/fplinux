// SPDX-License-Identifier: GPL-2.0-only
/* Minimal DT machine descriptor for Unisoc UMS9117/T117. */
#include <asm/mach/arch.h>

static const char *const ums9117_dt_compat[] __initconst = {
	"sprd,ums9117",
	NULL,
};

DT_MACHINE_START(UMS9117_DT, "Unisoc UMS9117/T117").dt_compat =
	ums9117_dt_compat,
			     MACHINE_END
