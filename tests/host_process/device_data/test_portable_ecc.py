# SPDX-License-Identifier: GPL-2.0-only
"""Exercise selected NAND ECC sectors through a controlled child process."""

from __future__ import annotations

import hashlib
import shutil
from dataclasses import replace
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.fixtures.executables import install_python_script
from tests.small.device_data.portable_fixtures import (
    shared_extraction as shared_extraction,  # noqa: PLC0414 -- pytest fixture export.
)

if TYPE_CHECKING:
    from tests.small.device_data.portable_fixtures import SharedExtraction


@pytest.fixture
def shared_ecc_extraction(shared_extraction: SharedExtraction) -> SharedExtraction:
    """Use the production ECC module with one selected sector and controlled correction."""
    extraction = shared_extraction
    assert extraction.spec.parser is not None
    shutil.copyfile(
        ROOT / "tests/host_process/device_data/fixtures/portable/portable_ecc_parser.py",
        extraction.spec.parser,
    )
    selected_sector = b"B" * 100 + b"damaged!" + b"B" * 916
    page = (
        b"!" * 1024
        + selected_sector
        + b"\xffabc"
        + b"1" * 21
        + b"ignored"
        + b"defg"
        + b"2" * 21
        + b"outside"
        + b"Z" * 64
    )
    with extraction.backup.open("wb") as stream:
        stream.truncate(142606336)
        stream.write(page + b"\xff" * 2176)
    install_python_script(
        ROOT / "tests/fixtures/nand-ecc/nand_ecc_child.py", extraction.tools / "ums9117-nand-ecc"
    )
    (extraction.tools / "input.bin").write_bytes(selected_sector + b"2" * 21)
    (extraction.tools / "output.bin").write_bytes(b"b" * 100 + b"selected" + b"b" * 916)
    (extraction.tools / "exit-status.txt").write_text("0", encoding="ascii")
    (extraction.tools / "diagnostic.txt").write_text("", encoding="utf-8")
    return replace(
        extraction,
        spec=replace(
            extraction.spec,
            nand={"id": 0x81C8, "raw_page_bytes": 2176, "ecc_format": "ums9117-bch12"},
            ecc_module=ROOT / "platforms/ums9117/host/nand_ecc.py",
            groups={
                "bluetooth": [{"source": "radio.bin", "destination": "chip/radio.bin", "size": 8}]
            },
        ),
    )


def test_declared_ecc_publishes_corrected_partition_and_keeps_raw_backup(
    shared_ecc_extraction: SharedExtraction,
) -> None:
    extraction = shared_ecc_extraction
    with extraction.backup.open("rb") as stream:
        original_digest = hashlib.file_digest(stream, "sha256").hexdigest()

    phone = extraction.prepare()

    assert (phone / "groups/bluetooth/radio.bin").read_bytes() == b"selected"
    with extraction.backup.open("rb") as stream:
        assert hashlib.file_digest(stream, "sha256").hexdigest() == original_digest
        stream.seek(1124)
        assert stream.read(8) == b"damaged!"


def test_uncorrectable_selected_sector_does_not_publish_phone_data(
    shared_ecc_extraction: SharedExtraction,
) -> None:
    extraction = shared_ecc_extraction
    (extraction.tools / "exit-status.txt").write_text("1", encoding="ascii")
    (extraction.tools / "diagnostic.txt").write_text("uncorrectable codeword\n", encoding="utf-8")

    with pytest.raises(
        SystemExit, match="NAND ECC correction at main offset 0x400: uncorrectable codeword"
    ):
        extraction.prepare()

    assert not extraction.phone.exists()
    assert list(extraction.root.glob(".phone-*")) == []


@pytest.mark.parametrize("marker", [2048, 4224], ids=["first-page", "second-page"])
def test_corrected_partition_with_raw_factory_marker_is_not_published(
    shared_ecc_extraction: SharedExtraction, marker: int
) -> None:
    extraction = shared_ecc_extraction
    with extraction.backup.open("r+b") as stream:
        stream.seek(marker)
        stream.write(b"\x00")

    with pytest.raises(SystemExit, match="selected NAND block 0 is marked bad"):
        extraction.prepare()

    assert not extraction.phone.exists()
    assert list(extraction.root.glob(".phone-*")) == []
    with extraction.backup.open("rb") as stream:
        stream.seek(marker)
        assert stream.read(1) == b"\x00"
