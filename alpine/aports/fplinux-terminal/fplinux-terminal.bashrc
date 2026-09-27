#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only

# Readline owns command editing, completion, search and command history.
: "${FPLINUX_TERMINAL_PROMPT_TOKEN:?missing terminal prompt token}"
set -o emacs
bind 'set bell-style none'
bind 'set enable-bracketed-paste on'
bind -x '"\e[99~":READLINE_LINE=; READLINE_POINT=0; READLINE_MARK=0'
HISTSIZE=500
HISTFILESIZE=500
PS0=$'\e]777;'"${FPLINUX_TERMINAL_PROMPT_TOKEN}"$';C\a'
PS1='\u@\h:\w\$ \['$'\e]777;'"${FPLINUX_TERMINAL_PROMPT_TOKEN}"$';B\a''\]'
unset FPLINUX_TERMINAL_PROMPT_TOKEN
