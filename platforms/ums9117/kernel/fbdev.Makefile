# SPDX-License-Identifier: GPL-2.0-only
obj-$(CONFIG_FB_UMS9117) += ums9117-fb.o
ums9117-fb-y := ums9117-fb-core.o ums9117-fb-spi.o ums9117-fb-lcm.o
