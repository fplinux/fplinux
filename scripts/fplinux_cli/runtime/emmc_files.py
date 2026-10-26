# SPDX-License-Identifier: GPL-2.0-only
"""Read selected originals from a phone's explicitly read-only internal MMC card."""

from __future__ import annotations

import re
import shlex
import tarfile
import tempfile
from pathlib import Path
from typing import TYPE_CHECKING, cast

from fplinux_cli.device_data.emmc import EmmcError, validate_file_selections

if TYPE_CHECKING:
    from collections.abc import Sequence
    from typing import Any, BinaryIO

    from fplinux_cli.device_data.emmc import FileSelection
    from fplinux_cli.runtime.nand_backup import SshTransport

_COLLECTOR = Path(__file__).with_name("emmc-files") / "collect.sh"
_READ_TIMEOUT_SECONDS = 120
_CARD = re.compile(r"/dev/mmcblk[0-9]+")


def _read_archive(source: BinaryIO, selections: Sequence[FileSelection]) -> dict[str, bytes]:
    expected = {selection.name: selection.max_bytes for selection in selections}
    originals: dict[str, bytes] = {}
    offset = 0
    try:
        with tarfile.open(fileobj=source, mode="r:") as archive:
            for member in archive:
                if (
                    member.offset != offset
                    or member.offset_data != offset + tarfile.BLOCKSIZE
                    or member.pax_headers
                ):
                    message = "eMMC file archive contains extra metadata"
                    raise EmmcError(message)
                if (
                    not member.isreg()
                    or member.sparse is not None
                    or member.name not in expected
                    or member.name in originals
                ):
                    message = "eMMC file archive contains an unexpected or repeated file"
                    raise EmmcError(message)
                if not 0 < member.size <= expected[member.name]:
                    raise EmmcError(f"eMMC original {member.name} has an invalid size")
                contents = archive.extractfile(member)
                if contents is None:
                    raise EmmcError(f"eMMC original {member.name} cannot be read")
                with contents:
                    data = contents.read(member.size + 1)
                if len(data) != member.size:
                    raise EmmcError(f"eMMC original {member.name} is incomplete")
                originals[member.name] = data
                offset = member.offset_data + (
                    (member.size + tarfile.BLOCKSIZE - 1) // tarfile.BLOCKSIZE * tarfile.BLOCKSIZE
                )
        if originals.keys() != expected.keys():
            message = "eMMC file archive is missing required originals"
            raise EmmcError(message)
        source.seek(offset)
        trailer = source.read()
        if len(trailer) < 2 * tarfile.BLOCKSIZE or any(trailer):
            message = "eMMC file archive has an incomplete or invalid trailer"
            raise EmmcError(message)
    except (tarfile.TarError, OSError, EOFError) as error:
        raise EmmcError(f"eMMC file archive cannot be read: {error}") from error
    return originals


def read_originals(
    ssh: SshTransport,
    session: dict[str, Any],
    raw_device: str,
    selections: Sequence[FileSelection],
) -> dict[str, bytes]:
    """Return only the requested files after remote unmount and successful SSH completion."""
    if _CARD.fullmatch(raw_device) is None:
        message = "eMMC source must name a whole /dev/mmcblkN card"
        raise EmmcError(message)
    declarations = [
        {
            "name": selection.name,
            "locations": [list(location) for location in selection.locations],
            "max_bytes": selection.max_bytes,
        }
        for selection in selections
    ]
    admitted = validate_file_selections(declarations)
    arguments = [raw_device, str(len(admitted))]
    for selection in admitted:
        arguments.extend((selection.name, str(selection.max_bytes), str(len(selection.locations))))
        for partition, path in selection.locations:
            arguments.extend((partition, path))
    arguments.extend(selection.name for selection in admitted)
    command = shlex.join(
        ["sh", "-c", _COLLECTOR.read_text(encoding="utf-8"), "fplinux-emmc-files", *arguments]
    )
    archive_limit = 10240 + sum(
        tarfile.BLOCKSIZE
        + (selection.max_bytes + tarfile.BLOCKSIZE - 1) // tarfile.BLOCKSIZE * tarfile.BLOCKSIZE
        for selection in admitted
    )

    def check_size(size: int) -> None:
        if size > archive_limit:
            message = "eMMC file archive exceeds the selected size limits"
            raise EmmcError(message)

    with tempfile.TemporaryFile(mode="w+b") as temporary:
        source = cast("BinaryIO", temporary)
        ssh.stream_remote(
            session,
            command,
            source,
            timeout=_READ_TIMEOUT_SECONDS,
            progress=check_size,
        )
        check_size(source.tell())
        source.seek(0)
        return _read_archive(source, admitted)
