# SPDX-License-Identifier: MIT

FPLINUX_KEYPAD_MOD_DIR := $(USERMOD_DIR)
SRC_USERMOD_C += $(FPLINUX_KEYPAD_MOD_DIR)/fplinux_keypad.c
SRC_USERMOD_C += $(FPLINUX_KEYPAD_MOD_DIR)/fplinux-input-session.c
SRC_USERMOD_C += $(FPLINUX_KEYPAD_MOD_DIR)/fplinux-keyboard-text.c
SRC_USERMOD_C += $(FPLINUX_KEYPAD_MOD_DIR)/fplinux-multitap.c
SRC_USERMOD_C += $(FPLINUX_KEYPAD_MOD_DIR)/fplinux_multitap_native.c
CFLAGS_USERMOD += -std=gnu11 -Wall -Wextra
# The APKBUILD stages libxkbcommon's headers and static archive here.
CFLAGS_USERMOD += -I$(FPLINUX_KEYPAD_MOD_DIR)/xkbcommon/include
LDFLAGS_USERMOD += $(FPLINUX_KEYPAD_MOD_DIR)/xkbcommon/libxkbcommon.a
LDFLAGS_USERMOD += -linput -ludev
