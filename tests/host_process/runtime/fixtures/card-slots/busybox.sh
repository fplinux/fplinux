#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu
real_busybox=${CARD_TEST_BUSYBOX:?}
sync_log=${CARD_TEST_SYNC_LOG:?}
failed_sync=${CARD_TEST_FAIL_SYNC:?}

if [ "$1" = sync ]; then
	printf '%s\n' sync >>"$sync_log"
	calls=$("$real_busybox" wc -l <"$sync_log")
	if [ "$calls" -eq "$failed_sync" ]; then
		exit 1
	fi
fi
exec "$real_busybox" "$@"
