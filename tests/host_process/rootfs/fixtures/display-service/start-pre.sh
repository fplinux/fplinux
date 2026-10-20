#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

service_script=$1
started_service=$2

service_started() {
	[ "$1" = "$started_service" ]
}

eerror() {
	printf '%s\n' "$*" >&2
}

# OpenRC normally sources the script and calls this hook before the command.
# The test supplies the production service path.
# shellcheck disable=SC1090
. "$service_script"
start_pre
printf 'startup allowed\n'
