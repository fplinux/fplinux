# SPDX-License-Identifier: MIT
# ruff: noqa: INP001
"""Select the builtin MicroPythonOS closure shipped by FPLinux."""

import argparse
import shutil
from pathlib import Path

_TRIMMED_APP_FILES = (
    "com.micropythonos.settings/bootloader.py",
    "com.micropythonos.settings/calibrate_imu.py",
    "com.micropythonos.settings/check_imu_calibration.py",
)


def read_allowlist(path: Path) -> set[str]:
    names = {
        line.strip() for line in path.read_text(encoding="utf-8").splitlines() if line.strip()
    }
    if not names or any("/" in name or name in {".", ".."} for name in names):
        message = "invalid builtin application allowlist"
        raise ValueError(message)
    return names


def select_builtin(source_root: Path, allowlist: set[str]) -> None:
    builtin = source_root / "internal_filesystem" / "builtin"
    apps = builtin / "apps"
    if not apps.is_dir():
        raise FileNotFoundError(str(apps))

    available = {child.name for child in apps.iterdir() if child.is_dir()}
    missing = allowlist - available
    if missing:
        raise FileNotFoundError("missing builtin apps: " + ", ".join(sorted(missing)))

    for child in apps.iterdir():
        if child.is_dir() and child.name not in allowlist:
            shutil.rmtree(child)

    for relative in _TRIMMED_APP_FILES:
        candidate = apps / relative
        if candidate.is_file():
            candidate.unlink()

    for relative in ("firmware", "html", "res/emojis"):
        candidate = builtin / relative
        if candidate.is_dir():
            shutil.rmtree(candidate)

    selected = {child.name for child in apps.iterdir() if child.is_dir()}
    if selected != allowlist:
        message = "builtin app selection did not converge"
        raise RuntimeError(message)
    if any((apps / relative).exists() for relative in _TRIMMED_APP_FILES):
        message = "builtin settings selection did not converge"
        raise RuntimeError(message)
    if not (builtin / "res" / "MicroPythonOS-logo-white-long-w296.png").is_file():
        message = "missing builtin logo"
        raise FileNotFoundError(message)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("source_root", type=Path)
    parser.add_argument("allowlist", type=Path)
    arguments = parser.parse_args()
    select_builtin(arguments.source_root, read_allowlist(arguments.allowlist))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
