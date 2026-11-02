# SPDX-License-Identifier: GPL-2.0-only
"""Admit an extracted headless runtime and create its empty transport capsule."""

from __future__ import annotations

import json
import os
import sys
from pathlib import Path
from types import SimpleNamespace
from unittest import mock


def main() -> None:
    """Use the delivered package without developer modules or a source checkout."""
    root = Path(sys.argv[1]).resolve()
    sys.path.insert(0, str(root / "scripts"))
    from fplinux_cli.device_data.cpio import (  # noqa: PLC0415
        create_device_data_initramfs,
    )
    from fplinux_cli.device_data.inputs import capture_phone_data  # noqa: PLC0415
    from fplinux_cli.runtime import bundle as bundle_runtime  # noqa: PLC0415
    from fplinux_cli.runtime.cli import main as runtime_main  # noqa: PLC0415

    host = "nt" if sys.argv[3] == "windows" else "posix"
    with mock.patch.object(
        bundle_runtime, "os", SimpleNamespace(name=host, access=os.access, X_OK=os.X_OK)
    ):
        admitted = bundle_runtime.admit_bundle(root)
    runtime = admitted.runtime
    assert runtime["device_data"] is None
    assert callable(capture_phone_data)
    assert callable(runtime_main)
    contents = create_device_data_initramfs({}, usb_transport="rndis")
    Path(sys.argv[2]).write_bytes(contents)
    for name, module in sys.modules.copy().items():
        if name.startswith("fplinux_cli"):
            assert module.__file__ is not None
            assert Path(module.__file__).resolve().is_relative_to(root)
    print(json.dumps({"target": runtime["target"], "preparation": runtime["device_data"]}))


if __name__ == "__main__":
    main()
