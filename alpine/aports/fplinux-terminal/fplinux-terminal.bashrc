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
HISTFILE="${XDG_STATE_HOME:-$HOME/.local/state}/fplinux/org.fplinux.terminal/history"
mkdir -p "${HISTFILE%/*}"
# Append history at every prompt so a killed shell keeps earlier commands.
shopt -s histappend
PROMPT_COMMAND="history -a${PROMPT_COMMAND:+; $PROMPT_COMMAND}"
PS0=$'\e]777;'"${FPLINUX_TERMINAL_PROMPT_TOKEN}"$';C\a'
# Under 30 columns the prompt keeps only the directory's name. Bash sets
# COLUMNS from the PTY before it reads this file (checkwinsize, on by
# default); test compares integers only and leaves other values long.
prompt='\u@\h:\w\$ '
if [ "${COLUMNS:-80}" -lt 30 ] 2>/dev/null; then
	prompt='\W\$ '
fi
PS1=$prompt'\['$'\e]777;'"${FPLINUX_TERMINAL_PROMPT_TOKEN}"$';B\a''\]'
unset prompt FPLINUX_TERMINAL_PROMPT_TOKEN
