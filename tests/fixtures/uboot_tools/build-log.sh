#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# The host-process harness supplies the destination for this build event.
# shellcheck disable=SC2154
printf '%s\n' build >>"$FPLINUX_TEST_BUILD_LOG"
