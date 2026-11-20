# SPDX-License-Identifier: GPL-2.0-only
"""Read the selected card producer and template through an isolated staged import."""

from __future__ import annotations

import json
import sys

from fplinux_cli.build.storage.layout import genimage_config
from fplinux_cli.build.storage.sd import compressed_image_name
from fplinux_cli.manifests.targets import load_target


def main() -> None:
    """Render one profile's card config without executing external image tools."""
    target = load_target(sys.argv[1], sys.argv[2])
    storage = target["storage"]
    print(
        json.dumps(
            {
                "image": compressed_image_name(storage),
                "configuration": genimage_config(storage).decode("ascii"),
            }
        )
    )


if __name__ == "__main__":
    main()
