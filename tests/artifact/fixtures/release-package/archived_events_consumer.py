# SPDX-License-Identifier: GPL-2.0-only
"""Import the delivered package and inspect its open event record."""

import sys
from pathlib import Path


def main() -> None:
    """Use only the extracted package closure through its normal import boundary."""
    root = Path(sys.argv[1]).resolve()
    sys.path.insert(0, str(root / "scripts"))
    from fplinux_cli import common  # noqa: PLC0415 -- delivered package path is explicit.
    from fplinux_cli.runtime import events  # noqa: PLC0415

    assert Path(common.__file__).resolve().is_relative_to(root)
    assert Path(events.__file__).resolve().is_relative_to(root)
    output = Path(sys.argv[2])
    with events.record_events(
        output, target="nokia-ta1618", profile=None, build_type="release"
    ) as records:
        records.emit("waiting-for-device")
        sys.stdout.write(output.read_text(encoding="utf-8"))


if __name__ == "__main__":
    main()
