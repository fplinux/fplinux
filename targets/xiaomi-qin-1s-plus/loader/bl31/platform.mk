# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026, FPLinux contributors.

ifneq (${ARCH},aarch64)
$(error Qin 1s+ BL31 requires ARCH=aarch64)
endif
ifneq (${SPD},none)
$(error Qin 1s+ BL31 requires SPD=none)
endif

RESET_TO_BL31 := 1
PROGRAMMABLE_RESET_ADDRESS := 1
COLD_BOOT_SINGLE_CPU := 1
CTX_INCLUDE_AARCH32_REGS := 1
USE_COHERENT_MEM := 1
SEPARATE_CODE_AND_RODATA := 0

PLAT_INCLUDES := -Iplat/qin1splus/include

include drivers/arm/gic/v2/gicv2.mk

BL31_SOURCES += \
	lib/cpus/aarch64/cortex_a53.S \
	${GICV2_SOURCES} \
	lib/xlat_tables/xlat_tables_common.c \
	lib/xlat_tables/aarch64/xlat_tables.c \
	plat/common/aarch64/plat_common.c \
	plat/common/plat_psci_common.c \
	plat/common/plat_gicv2.c \
	plat/qin1splus/qin1splus_helpers.S \
	plat/qin1splus/qin1splus_setup.c \
	plat/qin1splus/qin1splus_pm.c
