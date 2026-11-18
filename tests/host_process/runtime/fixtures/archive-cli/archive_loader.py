# SPDX-License-Identifier: GPL-2.0-only
"""Replace the native Linux loader while retaining the delivered Python command."""

from __future__ import annotations

import os
import sys

assert sys.argv[1] == "--library-path"
os.execv(sys.executable, [sys.executable, *sys.argv[4:]])
