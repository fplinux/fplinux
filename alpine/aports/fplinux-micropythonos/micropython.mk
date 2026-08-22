# SPDX-License-Identifier: MIT

FPLINUX_KEYPAD_MOD_DIR := $(USERMOD_DIR)
SRC_USERMOD_C += $(FPLINUX_KEYPAD_MOD_DIR)/fplinux_keypad.c
SRC_USERMOD_C += $(FPLINUX_KEYPAD_MOD_DIR)/fplinux-multitap.c
SRC_USERMOD_C += $(FPLINUX_KEYPAD_MOD_DIR)/fplinux_multitap_native.c
CFLAGS_USERMOD += -std=gnu11 -Wall -Wextra
