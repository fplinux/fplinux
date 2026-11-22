# SPDX-License-Identifier: GPL-2.0-only
obj-$(CONFIG_MARLIN2_SDIO) += marlin2.o
marlin2-y := marlin2-radio.o marlin2-sdio.o \
	marlin2-packet-codec.o marlin2-packet-sdio.o \
	marlin2-transport.o marlin2-transport-wire.o
marlin2-$(CONFIG_MARLIN2_WLAN) += marlin2-wlan.o marlin2-wlan-wire.o
marlin2-$(CONFIG_MARLIN2_BLUETOOTH) += marlin2-bluetooth.o marlin2-bluetooth-wire.o
marlin2-$(CONFIG_MARLIN2_FM) += marlin2-fm.o
marlin2-$(CONFIG_MACH_BQ_2830L_TANK_CONNECT_MAX) += bq2830l-marlin2.o
marlin2-$(CONFIG_MACH_ITEL_NEO_R60_PLUS) += itel-neo-r60-marlin2.o
