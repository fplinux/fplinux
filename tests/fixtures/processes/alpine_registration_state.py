# SPDX-License-Identifier: GPL-2.0-only
"""Report recipes and cache admission from the explicitly supplied source checkout."""

import json
import sys
from pathlib import Path
from unittest import mock

sys.dont_write_bytecode = True
root = Path(sys.argv[1])
sys.path.insert(0, str(root / "scripts"))
from fplinux_cli.alpine import packages, recipes  # noqa: E402 -- select the supplied checkout.

names = json.loads(sys.argv[2])
selected = json.loads(sys.argv[3])
with mock.patch.object(packages, "CACHE", root / ".cache"):
    result = {
        "packages": {
            name: {
                "recipe": recipes.alpine_package_recipe(name, "1" * 64, "d" * 64),
                "cache_hit": (
                    packages._cached_aport_packages(  # noqa: SLF001 -- observe the real cache reader.
                        name, "1" * 64, "d" * 64
                    )
                    is not None
                ),
            }
            for name in names
        },
        "rootfs": recipes.alpine_rootfs_recipe("1" * 64, "d" * 64, selected),
    }
print(json.dumps(result))
