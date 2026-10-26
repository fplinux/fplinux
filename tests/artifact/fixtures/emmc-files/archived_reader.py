# SPDX-License-Identifier: GPL-2.0-only
"""Exercise extracted preparation resources without importing the source checkout."""

from __future__ import annotations

import json
import sys
from pathlib import Path

bundle = Path(sys.argv[1])
sys.path.insert(0, str(bundle / "scripts"))

from fplinux_cli.device_data.emmc import FileSelection, extract_files  # noqa: E402
from fplinux_cli.runtime.bundle import load_module  # noqa: E402

parser = load_module(
    bundle / "targets/xiaomi-qin-1s-plus/qin1splus_device_data.py", "archived_phone_data"
)
originals = extract_files(
    Path(sys.argv[2]),
    [
        FileSelection(
            "audio_para",
            (("userdata", "/local/media/audio_para"), ("system", "/etc/audio_para")),
            100,
        )
    ],
    python_modules=bundle / "scripts",
)
print(
    json.dumps(
        {
            "original": originals["audio_para"].decode("ascii"),
            "rf_config": parser.encode_rf_template(b"conf_version = 1\n", kind="config").hex(),
        }
    )
)
