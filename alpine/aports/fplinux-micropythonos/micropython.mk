# SPDX-License-Identifier: MIT

FPLINUX_KEYPAD_MOD_DIR := $(USERMOD_DIR)
SRC_USERMOD_C += $(FPLINUX_KEYPAD_MOD_DIR)/fplinux_keypad.c
SRC_USERMOD_C += $(FPLINUX_KEYPAD_MOD_DIR)/fplinux-input-session.c
SRC_USERMOD_C += $(FPLINUX_KEYPAD_MOD_DIR)/fplinux-drm-session.c
SRC_USERMOD_C += $(FPLINUX_KEYPAD_MOD_DIR)/fplinux-keyboard-text.c
SRC_USERMOD_C += $(FPLINUX_KEYPAD_MOD_DIR)/fplinux-multitap.c
SRC_USERMOD_C += $(FPLINUX_KEYPAD_MOD_DIR)/fplinux_multitap_native.c
CFLAGS_USERMOD += -Wall -Wextra
CFLAGS_USERMOD += $(shell pkg-config --cflags libdrm xkbcommon)
LDFLAGS_USERMOD += $(shell pkg-config --libs libdrm xkbcommon) -linput -ludev

# The Unix port appends GNU99 after user-module flags; UTF-32 needs C11.
$(BUILD)/fplinux_keypad/fplinux-multitap.o: CFLAGS += -std=gnu11
