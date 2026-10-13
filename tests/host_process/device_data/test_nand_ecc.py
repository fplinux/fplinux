# SPDX-License-Identifier: GPL-2.0-only
"""Read real NAND projections through a child that replaces only native BCH correction.

Literal child responses establish sector selection, OOB framing and slicing,
not BCH correctness or hardware behavior.
"""

from __future__ import annotations

import importlib.util
import sys
from functools import partial
from types import SimpleNamespace
from typing import TYPE_CHECKING

import pytest
from fplinux_cli.device_data.formats import PhysicalNand

from tests import ROOT
from tests.fixtures.executables import install_python_script

if TYPE_CHECKING:
    from pathlib import Path
    from types import ModuleType


@pytest.fixture(scope="module")
def ecc_module() -> ModuleType:
    """Import the platform decoder normally without replacing its implementation."""
    path = ROOT / "platforms/ums9117/host/nand_ecc.py"
    spec = importlib.util.spec_from_file_location("ums9117_nand_ecc_under_test", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load the platform module: {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


@pytest.fixture
def correction_process(tmp_path: Path) -> Path:
    """Install an exact-response child at the expensive native tool boundary."""
    tools = tmp_path / "host-tools"
    tools.mkdir()
    install_python_script(
        ROOT / "tests/fixtures/nand-ecc/nand_ecc_child.py", tools / "ums9117-nand-ecc"
    )
    return tools


def _child_response(
    tools: Path,
    *,
    records: bytes,
    corrected: bytes,
    exit_status: int = 0,
    diagnostic: str = "",
) -> None:
    """Provide literal protocol bytes and one controlled process result."""
    (tools / "input.bin").write_bytes(records)
    (tools / "output.bin").write_bytes(corrected)
    (tools / "exit-status.txt").write_text(str(exit_status), encoding="ascii")
    (tools / "diagnostic.txt").write_text(diagnostic, encoding="utf-8")


def _raw_page(first: bytes, second: bytes, first_parity: bytes, second_parity: bytes) -> bytes:
    """Build two main sectors and two 32-byte OOB records, followed by unused OOB."""
    return (
        first
        + second
        + b"\xffabc"
        + first_parity
        + b"ignored"
        + b"defg"
        + second_parity
        + b"outside"
        + b"Z" * 64
    )


def _nand(raw: bytes, module: ModuleType, tools: Path) -> PhysicalNand:
    return PhysicalNand(raw, 2176, partial(module.decode_main, host_tools=tools))


def test_sector_crossing_uses_only_main_and_parity_and_returns_the_exact_slice(
    ecc_module: ModuleType, correction_process: Path
) -> None:
    """Metadata and the unused OOB half never enter the child's correction records."""
    first_parity = bytes.fromhex("0102030405060708090a0b0c0d0e0f101112131415")
    second_parity = bytes.fromhex("2122232425262728292a2b2c2d2e2f303132333435")
    raw = _raw_page(b"A" * 1024, b"B" * 1024, first_parity, second_parity)
    _child_response(
        correction_process,
        records=b"A" * 1024 + first_parity + b"B" * 1024 + second_parity,
        corrected=b"a" * 1020 + b"SECT" + b"OR" + b"b" * 1022,
    )
    nand = _nand(raw, ecc_module, correction_process)

    assert nand.main_bytes(1020, 8) == b"SECTORbb"
    assert nand.raw == raw
    assert PhysicalNand(raw, 2176).main_bytes(1020, 8) == b"AAAABBBB"


def test_page_crossing_selects_adjacent_sectors_and_skips_physical_oob(
    ecc_module: ModuleType, correction_process: Path
) -> None:
    """A requested span reaches the next main page without decoding neighboring sectors."""
    raw = _raw_page(b"A" * 1024, b"B" * 1024, b"1" * 21, b"2" * 21) + _raw_page(
        b"C" * 1024, b"D" * 1024, b"3" * 21, b"4" * 21
    )
    _child_response(
        correction_process,
        records=b"B" * 1024 + b"2" * 21 + b"C" * 1024 + b"3" * 21,
        corrected=b"b" * 1020 + b"PAGE" + b"NEXTDATA" + b"c" * 1016,
    )
    nand = _nand(raw, ecc_module, correction_process)

    assert nand.partition_bytes((2044, 12)) == b"PAGENEXTDATA"
    assert nand.raw == raw


def test_read_inside_one_sector_excludes_a_corrupt_sector_on_the_same_page(
    ecc_module: ModuleType, correction_process: Path
) -> None:
    """Unreadable neighboring sectors cannot prevent extracting selected bytes."""
    raw = _raw_page(b"!" * 1024, b"B" * 1024, bytes(21), b"2" * 21)
    _child_response(
        correction_process,
        records=b"B" * 1024 + b"2" * 21,
        corrected=b"b" * 100 + b"selected" + b"b" * 916,
    )

    assert _nand(raw, ecc_module, correction_process).main_bytes(1124, 8) == b"selected"
    with pytest.raises(ValueError, match="unexpected BCH record stream"):
        _nand(raw, ecc_module, correction_process).main_bytes(0, 8)


@pytest.mark.parametrize("marker", [2048, 4224], ids=["first-page", "second-page"])
def test_corrected_main_reads_do_not_clear_either_raw_factory_marker(
    marker: int, ecc_module: ModuleType, correction_process: Path
) -> None:
    """Partition admission still uses both factory markers from the physical backup."""
    raw = bytearray(
        _raw_page(b"A" * 1024, b"B" * 1024, b"1" * 21, b"2" * 21)
        + _raw_page(b"C" * 1024, b"D" * 1024, b"3" * 21, b"4" * 21)
    )
    raw[marker] = 0
    original = bytes(raw)
    _child_response(
        correction_process,
        records=b"B" * 1024 + b"2" * 21,
        corrected=b"b" * 100 + b"selected" + b"b" * 916,
    )
    nand = _nand(original, ecc_module, correction_process)

    assert nand.main_bytes(1124, 8) == b"selected"
    with pytest.raises(ValueError, match="selected NAND block 0 is marked bad"):
        nand.partition_bytes((1124, 8))
    assert nand.raw == original
    with pytest.raises(ValueError, match="selected NAND block 0 is marked bad"):
        PhysicalNand(original, 2176).require_good_blocks(1124, 8)


def test_native_rejection_is_reported_with_the_selected_main_offset(
    ecc_module: ModuleType, correction_process: Path
) -> None:
    """An uncorrectable selected codeword stops extraction rather than returning raw bytes."""
    raw = _raw_page(b"A" * 1024, b"B" * 1024, b"1" * 21, b"2" * 21)
    _child_response(
        correction_process,
        records=b"B" * 1024 + b"2" * 21,
        corrected=b"",
        exit_status=1,
        diagnostic="uncorrectable codeword\n",
    )

    with pytest.raises(
        ValueError, match="NAND ECC correction at main offset 0x400: uncorrectable codeword"
    ):
        _nand(raw, ecc_module, correction_process).main_bytes(1124, 8)


def test_short_native_output_cannot_be_returned_as_a_complete_projection(
    ecc_module: ModuleType, correction_process: Path
) -> None:
    """Successful exit alone does not establish that all selected main bytes were returned."""
    raw = _raw_page(b"A" * 1024, b"B" * 1024, b"1" * 21, b"2" * 21)
    _child_response(
        correction_process,
        records=b"B" * 1024 + b"2" * 21,
        corrected=b"b" * 1023,
    )

    with pytest.raises(ValueError, match="incomplete main-area projection"):
        _nand(raw, ecc_module, correction_process).main_bytes(1124, 8)


def test_windows_executable_name_returns_the_controlled_corrected_slice(
    ecc_module: ModuleType, correction_process: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """Select the Windows basename while executing a controlled child on the host."""
    (correction_process / "ums9117-nand-ecc").rename(correction_process / "ums9117-nand-ecc.exe")
    monkeypatch.setattr(ecc_module, "os", SimpleNamespace(name="nt"))
    raw = _raw_page(b"A" * 1024, b"B" * 1024, b"1" * 21, b"2" * 21)
    _child_response(
        correction_process,
        records=b"B" * 1024 + b"2" * 21,
        corrected=b"b" * 100 + b"selected" + b"b" * 916,
    )

    assert _nand(raw, ecc_module, correction_process).main_bytes(1124, 8) == b"selected"
