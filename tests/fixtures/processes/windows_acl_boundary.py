# SPDX-License-Identifier: GPL-2.0-only
"""Return test-owned ACL metadata in place of the native Get-Acl API."""

import json
import os
import sys
from pathlib import Path

assert Path(sys.argv[sys.argv.index("-Path") + 1]).is_file()
state = Path(os.environ["FPLINUX_TEST_ACL_STATE"])
print(json.dumps(json.loads(state.read_text(encoding="utf-8"))))
