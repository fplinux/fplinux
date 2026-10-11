#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
fplinux_archive=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd) || exit 1
exec sh "$fplinux_archive/Run.sh" --ssh "$@"
