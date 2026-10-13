# SPDX-License-Identifier: GPL-2.0-only
"""Import the selected staged closure and reuse an independently verified cache file."""

from __future__ import annotations

import sys
from pathlib import Path


def main() -> None:
    staged = Path(sys.argv[1])
    cache = Path(sys.argv[2])
    sys.path.insert(0, str(staged / "scripts"))

    from fplinux_cli.build import sources  # noqa: PLC0415 -- imports must resolve from staging.
    from fplinux_cli.environment import downloads  # noqa: PLC0415
    from fplinux_cli.manifests import values  # noqa: PLC0415

    for module in (sources, downloads, values):
        assert module.__file__ is not None
        assert Path(module.__file__).is_relative_to(staged)
    result = sources.fetch(
        "https://example.invalid/source.tar",
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        cache,
        "source.tar",
    )
    assert result.read_bytes() == b"abc"


if __name__ == "__main__":
    main()
