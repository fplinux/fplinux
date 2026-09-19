# SPDX-License-Identifier: GPL-2.0-only
.PHONY: all
%_defconfig:
	@mkdir -p "$(O)"
	@cp configs/$@ "$(O)/.config"
all:
	@if grep -qx 'CONFIG_TEST_FAIL=y' "$(O)/.config"; then exit 42; fi
	@sh scripts/build-log.sh
	@python3 scripts/fake-build.py "$(O)"
