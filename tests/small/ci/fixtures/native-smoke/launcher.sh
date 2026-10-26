#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

[ "$1" = --events ]
events=$2
launch=${events##*/}
launch=${launch%.jsonl}
log_directory="logs/$launch"
mkdir "$log_directory"
trap 'printf "stopped\n" >"$log_directory/stopped"; exit 0' TERM
printf 'launcher initialized\n' >"$log_directory/runtime.log"
printf '{"event":"waiting-for-device"}\n' >"$events"

while :; do
	sleep 1
done
