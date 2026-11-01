#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Run independent screen captures concurrently and replay their output in order.
# Source this file from a capture script that uses set -e.

# Captures mostly wait on real-time frame and timer deadlines. A few overlap
# without competing for the processor; more would stretch the deadlines that
# the captures themselves measure.
screen_capture_limit=${FPLINUX_SCREEN_CAPTURE_JOBS:-4}
case $screen_capture_limit in
*[!0-9]* | '' | 0*)
	echo "FPLINUX_SCREEN_CAPTURE_JOBS must be a positive integer" >&2
	exit 1
	;;
esac
screen_capture_pids=

# Usage: screen_captures_run COMMAND TASKS LOGS
#
# Call COMMAND once for each line of the TASKS file, passing the line as its
# only argument, in at most screen_capture_limit worker processes. Each call
# keeps its standard output and error under the new LOGS directory; both are
# replayed in task order after every worker has stopped, so the combined output
# matches a sequential run. A failed call stops its worker, and the function
# returns 1 after the remaining workers finish.
screen_captures_run() {
	screen_capture_command=$1
	screen_capture_tasks=$2
	screen_capture_logs=$3
	mkdir -- "$screen_capture_logs"
	screen_capture_worker=0
	while [ "$screen_capture_worker" -lt "$screen_capture_limit" ]; do
		screen_capture_work "$screen_capture_worker" &
		screen_capture_pids="$screen_capture_pids $!"
		screen_capture_worker=$((screen_capture_worker + 1))
	done
	screen_capture_status=0
	for screen_capture_pid in $screen_capture_pids; do
		wait "$screen_capture_pid" || screen_capture_status=1
	done
	screen_capture_pids=
	screen_capture_replay
	return "$screen_capture_status"
}

# Run every task whose zero-based line number belongs to this worker.
screen_capture_work() {
	screen_capture_index=0
	while IFS= read -r screen_capture_task; do
		if [ $((screen_capture_index % screen_capture_limit)) -eq "$1" ]; then
			screen_capture_log=$screen_capture_logs/$screen_capture_index
			# A separate child keeps set -e active inside COMMAND while its
			# status is tested here.
			"$screen_capture_command" "$screen_capture_task" </dev/null \
				>"$screen_capture_log.out" 2>"$screen_capture_log.err" &
			if ! wait "$!"; then
				echo "screen capture failed: $screen_capture_task" >>"$screen_capture_log.err"
				return 1
			fi
		fi
		screen_capture_index=$((screen_capture_index + 1))
	done <"$screen_capture_tasks"
}

screen_capture_replay() {
	screen_capture_index=0
	while IFS= read -r _; do
		screen_capture_log=$screen_capture_logs/$screen_capture_index
		if [ -e "$screen_capture_log.out" ]; then
			cat -- "$screen_capture_log.out"
			cat -- "$screen_capture_log.err" >&2
		fi
		screen_capture_index=$((screen_capture_index + 1))
	done <"$screen_capture_tasks"
}

# Stop and reap running workers before their caller removes the capture
# directory. A signal to the whole process group, as from Ctrl+C or a test
# deadline, also stops the captures that the workers started.
screen_captures_stop() {
	if [ -n "$screen_capture_pids" ]; then
		# The list holds only PIDs recorded by screen_captures_run.
		# shellcheck disable=SC2086
		kill $screen_capture_pids 2>/dev/null || :
		wait
	fi
}
