# SPDX-License-Identifier: GPL-2.0-only
"""Prepare this archive's device data from a saved physical NAND backup."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT / "scripts"))

from fplinux_cli.device_data.portable import prepare_portable_device_data  # noqa: E402


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--from-dump",
        required=True,
        type=Path,
        metavar="PATH",
        help="complete physical NAND backup from the phone selected by this archive",
    )
    arguments = parser.parse_args()
    generation = prepare_portable_device_data(from_dump=arguments.from_dump)
    print(f"Device data is ready in {generation}.")
    reports = generation / "reports"
    if reports.is_dir():
        for report in sorted(reports.glob("*/*")):
            print(f"Review {report}.")


if __name__ == "__main__":
    main()
