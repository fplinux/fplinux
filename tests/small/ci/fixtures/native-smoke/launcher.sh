#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

if [ "$#" -eq 0 ]; then
	cat menu.txt
	read -r choice
	[ "$choice" = 0 ]
	exit 0
fi

[ "$1" = run ]
[ "$2" = --target ]
target=$3
[ "$4" = --events ]
events=$5
if [ "$target" = nokia-ta1618 ]; then
	"${FPLINUX_TEST_PYTHON:?fixture Python is required}" -B phone-store.py "$target"
fi
launch=${events##*/}
launch=${launch%.jsonl}
scope=${events%/*}
log_directory="$scope/logs/$launch"
mkdir "$log_directory"
trap 'printf "stopped\n" >"$log_directory/stopped"; exit 0' TERM
printf 'launcher initialized\n' >"$log_directory/runtime.log"
printf '{"event":"waiting-for-device","target":"%s"}\n' "$target" >"$events"

while :; do
	sleep 1
done
