# SPDX-License-Identifier: MIT
# ruff: noqa: INP001
"""Remove MicroPythonOS libraries outside the FPLinux runtime closure."""

import argparse
import shutil
from pathlib import Path

_REMOVED_PATHS = (
    "aiohttp",
    "aiorepl.py",
    "drivers",
    "localPTZtime.py",
    "secp256k1.py",
    "secp256k1_compat.py",
    "uaiowebsocket.py",
    "unittest",
    "mpos/audio",
    "mpos/battery_manager.py",
    "mpos/camera_manager.py",
    "mpos/clipboard.py",
    "mpos/coverage.py",
    "mpos/device_manager.py",
    "mpos/gps_manager.py",
    "mpos/imu",
    "mpos/ir_manager.py",
    "mpos/lights.py",
    "mpos/lora_manager.py",
    "mpos/net",
    "mpos/partitions.py",
    "mpos/sensor_manager.py",
    "mpos/testing",
    "mpos/time_zone.py",
    "mpos/time_zones.py",
    "mpos/webserver",
    "mpos/ui/camera_activity.py",
    "mpos/ui/camera_settings.py",
)

_REQUIRED_PATHS = (
    "mpos/__init__.py",
    "mpos/board/fplinux.py",
    "mpos/fplinux_multitap.py",
    "mpos/fplinux_storage.py",
    "mpos/main.py",
    "mpos/ui/file_explorer_activity.py",
    "mpos/ui/fplinux_small_screen_layout.py",
)


def remove_path(path: Path) -> None:
    if path.is_dir():
        shutil.rmtree(path)
    elif path.exists():
        path.unlink()


def select_libraries(source_root: Path) -> None:
    library = source_root / "internal_filesystem" / "lib"
    if not library.is_dir():
        raise FileNotFoundError(str(library))

    for relative in _REQUIRED_PATHS:
        required = library / relative
        if not required.is_file():
            raise FileNotFoundError("missing FPLinux library: " + str(required))

    for relative in _REMOVED_PATHS:
        remove_path(library / relative)

    boards = library / "mpos/board"
    for child in boards.iterdir():
        if child.name != "fplinux.py":
            remove_path(child)

    remaining = [relative for relative in _REMOVED_PATHS if (library / relative).exists()]
    if remaining:
        raise RuntimeError("library selection did not converge: " + ", ".join(remaining))
    if {child.name for child in boards.iterdir()} != {"fplinux.py"}:
        message = "board selection did not converge"
        raise RuntimeError(message)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("source_root", type=Path)
    arguments = parser.parse_args()
    select_libraries(arguments.source_root)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
