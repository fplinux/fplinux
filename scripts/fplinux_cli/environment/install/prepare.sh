#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

environment_lock=$1
environment_inputs=$2
environment_resources=$3
environment_recipe=$4
environment_generation=$5
shift 5

printf '%.32s\n' "$environment_recipe" >/etc/machine-id

# The base rootfs has no Python interpreter; the complete local APK closure supplies it.
apk add --no-network --no-cache "$@"

exec python3 -B "$environment_resources/scripts/fplinux_cli/environment/install.py" \
	--lock "$environment_lock" \
	--inputs "$environment_inputs" \
	--resources "$environment_resources" \
	--recipe "$environment_recipe" \
	--generation "$environment_generation"
