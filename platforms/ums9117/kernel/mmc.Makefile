# SPDX-License-Identifier: GPL-2.0-only
obj-$(CONFIG_MMC_UMS9117) += ums9117-mmc.o
ums9117-mmc-y := ums9117-mmc-linux.o ums9117-sdio-slot.o ums9117-sdio-core.o
