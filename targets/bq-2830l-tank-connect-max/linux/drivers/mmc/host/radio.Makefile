# SPDX-License-Identifier: GPL-2.0-only
obj-$(CONFIG_MMC_SDHCI_UMS9117_BQ_RADIO) += bq2830l-radio.o
bq2830l-radio-y := bq2830l-radio-power.o marlin2-sdio.o \
	marlin2-packet-codec.o marlin2-packet-sdio.o \
	marlin2-transport.o marlin2-transport-wire.o
bq2830l-radio-$(CONFIG_BQ2830L_WLAN) += marlin2-wlan.o marlin2-wlan-wire.o
bq2830l-radio-$(CONFIG_BQ2830L_BLUETOOTH) += marlin2-bluetooth.o marlin2-bluetooth-wire.o
bq2830l-radio-$(CONFIG_BQ2830L_FM) += marlin2-fm.o marlin2-fm-wire.o
