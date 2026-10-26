# SPDX-License-Identifier: GPL-2.0-only
# ruff: noqa: INP001
"""Extract only requested originals in an isolated, caller-owned dependency lifetime."""

from __future__ import annotations

import io
import json
import sys
from pathlib import Path
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from typing import Any

sys.path[:0] = [str(Path(sys.argv[1])), str(Path(__file__).resolve().parents[3])]

from ext4 import (  # type: ignore[import-not-found]  # noqa: E402 -- delivered pinned dependency.
    Volume,
)
from ext4.inode import File  # type: ignore[import-not-found]  # noqa: E402
from fplinux_cli.device_data.emmc import (  # noqa: E402
    EmmcError,
    PartitionReader,
    read_gpt,
    validate_file_selections,
)


def extract(request: dict[str, Any]) -> None:
    """Fallback only for absent files; malformed or oversized originals stop extraction."""
    selections = validate_file_selections(request["selections"])
    output = Path(request["output"])
    with Path(request["image"]).open("rb") as source:
        partitions = read_gpt(source)
        for selection in selections:
            for partition_name, path in selection.locations:
                partition = partitions.get(partition_name)
                if partition is None:
                    raise EmmcError(f"eMMC GPT lacks required partition {partition_name}")
                with io.BufferedReader(PartitionReader(source, partition)) as bounded:
                    volume = Volume(bounded)
                    try:
                        inode = volume.inode_at(path)
                    except FileNotFoundError:
                        continue
                    if not isinstance(inode, File) or not 0 < inode.i_size <= selection.max_bytes:
                        raise EmmcError(
                            f"eMMC original {selection.name} is not a bounded regular file"
                        )
                    with inode.open() as contents:
                        data = contents.read(selection.max_bytes + 1)
                    if len(data) != inode.i_size:
                        raise EmmcError(f"eMMC original {selection.name} is incomplete")
                    destination = output / selection.name
                    with destination.open("xb") as stream:
                        destination.chmod(0o600)
                        stream.write(data)
                    break
            else:
                raise EmmcError(f"eMMC backup lacks required original {selection.name}")


if __name__ == "__main__":
    try:
        extract(json.load(sys.stdin))
    except (OSError, ValueError, RuntimeError) as error:
        print(f"eMMC file extraction failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
