#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

source_root=$1
build_dir=$2
prepared_inputs=$3
# shellcheck source=site/scripts/screens/ui-host-build.sh
. "$source_root/site/scripts/screens/ui-host-build.sh"
ui_host_prepare "$source_root" "$build_dir" "$prepared_inputs"
ui_host_build_application "$build_dir/fplinux-example-counter.c" \
	-o "$build_dir/fplinux-example-counter"
