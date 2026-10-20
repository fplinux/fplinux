# SPDX-License-Identifier: GPL-2.0-only
"""Import an extracted archive without any source-checkout Python path."""

from __future__ import annotations

import importlib
import importlib.util
import json
import sys
from pathlib import Path
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from types import ModuleType


def load_file(path: Path, name: str) -> ModuleType:
    """Load a delivered target or platform module through its normal module specification."""
    specification = importlib.util.spec_from_file_location(name, path)
    assert specification is not None
    assert specification.loader is not None
    module = importlib.util.module_from_spec(specification)
    sys.modules[name] = module
    specification.loader.exec_module(module)
    return module


def require_unavailable(name: str) -> None:
    """Require the module itself to be absent, rather than a broken transitive import."""
    try:
        importlib.import_module(name)
    except ModuleNotFoundError as error:
        if error.name != name:
            raise AssertionError((name, error.name)) from error
    else:
        raise AssertionError(f"archive includes unrelated runtime: {name}")


def main() -> None:
    """Check the dependency closure and forbidden producer interfaces of one archive."""
    root = Path(sys.argv[1]).resolve()
    sys.path[:0] = [str(root / "scripts"), str(root)]
    manifest = json.loads((root / "runtime-manifest.json").read_text(encoding="utf-8"))
    from fplinux_cli.device_data.cpio import create_device_data_initramfs  # noqa: PLC0415
    from fplinux_cli.device_data.inputs import (  # noqa: PLC0415
        capture_phone_data,
    )
    from fplinux_cli.runtime.cli import main  # noqa: PLC0415
    from fplinux_cli.runtime.nand_backup import backup_nand  # noqa: PLC0415

    for operation in (
        create_device_data_initramfs,
        capture_phone_data,
        main,
        backup_nand,
    ):
        assert callable(operation)
    descriptor = manifest["device_data"]
    parser = load_file(root / descriptor["parser"], "archive_target_parser")
    assert callable(parser.prepare_device_data)
    ecc = descriptor["ecc_module"] is not None
    if ecc:
        corrector = load_file(root / descriptor["ecc_module"], "archive_nand_ecc")
        assert callable(corrector.decode_main)
    for name in ("cli", "build", "environment", "reporting"):
        require_unavailable(f"fplinux_cli.{name}")
    if manifest["target"] == "bq-2830l-tank-connect-max":
        for name in ("cm4", "bluetooth_firmware", "fitted", "fm_radio"):
            require_unavailable(f"fplinux_cli.device_data.{name}")
    for name, module in sys.modules.copy().items():
        if name.startswith("fplinux_cli"):
            assert module.__file__ is not None
            assert Path(module.__file__).resolve().is_relative_to(root)
    print(json.dumps({"target": manifest["target"], "ecc": ecc}))


if __name__ == "__main__":
    main()
