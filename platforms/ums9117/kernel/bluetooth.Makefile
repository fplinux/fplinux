# SPDX-License-Identifier: GPL-2.0-only
obj-$(CONFIG_BT_UMS9117_CM4) += ums9117-bt.o
ccflags-y += -I$(srctree)/drivers/bluetooth
ums9117-bt-y := ums9117-bluetooth.o cm4-mailbox.o cm4-setup.o cm4-hci.o
