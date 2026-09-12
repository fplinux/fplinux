# SPDX-License-Identifier: GPL-2.0-only
obj-$(CONFIG_VIDEO_UMS9117_ROTA) += ums9117-rota.o
obj-$(CONFIG_VIDEO_UMS9117_JPEG) += ums9117-jpeg-dec.o
ums9117-jpeg-dec-y := ums9117-jpeg.o ums9117-jpeg-codec.o ums9117-jpeg-hw.o
