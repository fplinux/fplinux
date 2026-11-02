#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Use the installed hook handler without executing the incoming Git object."""

from pathlib import Path

from fplinux_cli import common
from fplinux_cli.quality.pre_push import check_pre_push

common.ROOT = Path.cwd()
check_pre_push()
