#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only

# Readline owns command editing, completion, search and command history.
set -o emacs
bind 'set bell-style none'
bind 'set enable-bracketed-paste on'
HISTSIZE=500
HISTFILESIZE=500
HISTFILE="${XDG_STATE_HOME:-$HOME/.local/state}/fplinux/org.fplinux.terminal/history"
(
	umask 077
	mkdir -p "${HISTFILE%/*}"
) 2>/dev/null
# Append history at every prompt so a killed shell keeps earlier commands.
shopt -s histappend
fplinux_history_unsaved=false
fplinux_history_sync() {
	if history -a 2>/dev/null; then
		fplinux_history_unsaved=false
	elif [ "$fplinux_history_unsaved" = false ]; then
		printf '%s\n' 'History cannot be saved. It remains available in this shell.' >&2
		fplinux_history_unsaved=true
	fi
}
PROMPT_COMMAND="fplinux_history_sync${PROMPT_COMMAND:+; $PROMPT_COMMAND}"
# Bash's final implicit write would repeat a failed prompt's diagnostic.
trap 'fplinux_history_sync; unset HISTFILE' EXIT
# Under 30 columns the prompt keeps only the directory's name. Bash sets
# COLUMNS from the PTY before it reads this file (checkwinsize, on by
# default); test compares integers only and leaves other values long.
prompt='\u@\h:\w\$ '
if [ "${COLUMNS:-80}" -lt 30 ] 2>/dev/null; then
	prompt='\W\$ '
fi
PS1=$prompt
unset prompt
