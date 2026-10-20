#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
: "${FPLINUX_TERMINAL_PROMPT_TOKEN:?missing terminal prompt token}"
PS0=$'\e]777;'"${FPLINUX_TERMINAL_PROMPT_TOKEN}"$';C\a'
PS1='\['$'\e]777;'"${FPLINUX_TERMINAL_PROMPT_TOKEN}"$';B\a''\]'
unset FPLINUX_TERMINAL_PROMPT_TOKEN
