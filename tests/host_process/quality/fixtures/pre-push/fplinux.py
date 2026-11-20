#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Use the installed hook handler without executing the incoming Git object."""

import sys
from pathlib import Path

from fplinux_cli import common
from fplinux_cli.quality.pre_push import check_pre_push

common.ROOT = Path.cwd()
if sys.argv[1:] != ["_pre-push"]:
    sys.exit("expected the _pre-push hook entry point")
check_pre_push()
