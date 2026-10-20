# SPDX-License-Identifier: GPL-2.0-only
obj-$(CONFIG_BT_QIN1SPLUS) += qin1splus-bt.o
qin1splus-bt-y := hci_qin1splus.o qin1splus-bt-protocol.o
