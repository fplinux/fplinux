# SPDX-License-Identifier: GPL-2.0-only
"""Contributor report extraction from synthetic stock firmware and a controlled utility."""

from __future__ import annotations

import hashlib
import json
import shutil
from dataclasses import replace
from types import SimpleNamespace
from typing import TYPE_CHECKING

import pytest
from fplinux_cli.cli import device_data as cli
from fplinux_cli.device_data.board_report import write_board_report
from fplinux_cli.device_data.formats import AudioProfileContext
from fplinux_cli.device_data.prepare import DeviceDataSpec, DeviceDataTools
from fplinux_cli.reporting import run as reporting

from tests import ROOT
from tests.fixtures.stock_board_report import STOCK_IMAGE, agreeing_vbm, backup, signed_image

if TYPE_CHECKING:
    from pathlib import Path


@pytest.fixture
def report_input(tmp_path: Path) -> tuple[Path, Path, DeviceDataSpec]:
    """Own a complete sparse NAND with actual selected stock-image and partition extents."""
    image = signed_image(bytes(0x5000))
    physical = backup(image, vbm=agreeing_vbm())
    saved = tmp_path / "nand.bin"
    with saved.open("wb") as stream:
        stream.write(physical.raw)
        stream.truncate(138412032)
    tools = tmp_path / "tools"
    tools.mkdir()
    executable = tools / "fphelper_t117"
    shutil.copyfile(ROOT / "tests/fixtures/fphelper/fake_fphelper.py", executable)
    executable.chmod(0o755)
    scenario = {
        "image_sha256": hashlib.sha256(image).hexdigest(),
        "commands": {
            "unpack": {
                "stdout": (ROOT / "tests/fixtures/stock-board-report/maps-only.txt").read_text(),
                "files": {
                    "pinmap.bin": "b0002a40 10000000 ffffffff ffffffff",
                    "keymap.bin": "2a00 3100",
                },
            }
        },
    }
    (tools / "scenario.json").write_text(json.dumps(scenario), encoding="utf-8")
    spec = DeviceDataSpec(
        target="headless",
        parser=None,
        groups={},
        audio=AudioProfileContext(
            machine_compatible=b"demo,headless",
            speaker_vibration=False,
            speaker_from_headphones=False,
            headfree_headphones_only=False,
        ),
        nand={"raw_device": "/dev/demo", "id": 0x21E5, "raw_page_bytes": 2112},
    )
    return saved, tools, spec


@pytest.mark.parametrize("geometry_source", ["target", "receipt"])
def test_board_report_needs_no_runtime_groups_and_keeps_selected_phone_data(
    tmp_path: Path, report_input: tuple[Path, Path, DeviceDataSpec], geometry_source: str
) -> None:
    """Real platform parsing and an external fake utility publish only the contributor report."""
    saved, tools, spec = report_input
    selected = tmp_path / "phone/groups/bluetooth/radio.bin"
    selected.parent.mkdir(parents=True)
    selected.write_bytes(b"already selected")
    result = tmp_path / "report"
    result.mkdir()
    with saved.open("rb") as stream:
        original_digest = hashlib.file_digest(stream, "sha256").hexdigest()
    if geometry_source == "receipt":
        receipt = {
            "target": "headless",
            "id_bytes": "e521",
            "chip": "controlled chip",
            "page_main_bytes": 2048,
            "oob_bytes": 64,
            "pages_per_block": 64,
            "block_count": 1024,
            "raw_bytes": 138412032,
            "sha256": original_digest,
        }
        saved.with_name("nand.bin.json").write_text(json.dumps(receipt), encoding="utf-8")
        spec = replace(spec, nand={"raw_device": "/dev/demo"})

    report = write_board_report(
        spec,
        saved,
        result,
        DeviceDataTools(host_tools=tools),
        extractor=ROOT / "platforms/ums9117/host/stock_image.py",
    )

    contents = json.loads(report.read_bytes())
    assert contents["keypad"]["boot_key"] == {"code": "0x2a", "key": "FPLINUX_KEY_STAR"}
    assert contents["pads"]["display"] == {"0x402a00b0": "0x00000010"}
    assert list(result.iterdir()) == [report]
    assert report.stat().st_mode & 0o777 == 0o600
    assert selected.read_bytes() == b"already selected"
    with saved.open("rb") as stream:
        assert hashlib.file_digest(stream, "sha256").hexdigest() == original_digest


def test_board_report_cli_prepares_pinned_tool_without_a_runtime_parser(
    tmp_path: Path,
    report_input: tuple[Path, Path, DeviceDataSpec],
    monkeypatch: pytest.MonkeyPatch,
    capsys: pytest.CaptureFixture[str],
) -> None:
    """The developer CLI works for headless configuration and leaves phone selection absent."""
    saved, tools, _spec = report_input
    provider = tmp_path / "platforms/ums9117/host/stock_image.py"
    provider.parent.mkdir(parents=True)
    shutil.copyfile(ROOT / "platforms/ums9117/host/stock_image.py", provider)
    monkeypatch.setattr(cli, "ROOT", tmp_path)
    monkeypatch.setattr(reporting, "ROOT", tmp_path)
    monkeypatch.setattr(
        cli,
        "load_target",
        lambda _target: {
            "platform": "ums9117",
            "identity": {"compatible": "demo,headless"},
            "audio_policy": {
                "speaker_vibration": False,
                "speaker_from_headphones": False,
                "headfree_headphones_only": False,
            },
            "device_data": {"groups": {}},
            "nand": {"id": 0x21E5, "raw_page_bytes": 2112},
        },
    )

    def install_tool(  # noqa: PLR0913 -- mirrors the host-tool preparation boundary.
        target: str,
        platform: str,
        tool: str,
        output: Path,
        *,
        offline: bool,
        reporter: object,
    ) -> None:
        del reporter
        assert (target, platform, tool, offline) == ("headless", "ums9117", "fphelper_t117", True)
        shutil.copyfile(tools / "fphelper_t117", output / "fphelper_t117")
        (output / "fphelper_t117").chmod(0o755)
        shutil.copyfile(tools / "scenario.json", output / "scenario.json")

    monkeypatch.setattr(cli, "prepare_host_tool", install_tool)

    cli.report_device_data("headless", from_dump=saved, offline=True)

    reports = list(
        (tmp_path / ".cache/diagnostics/device-data/headless").glob("*/board-report.json")
    )
    assert len(reports) == 1
    assert json.loads(reports[0].read_bytes())["keypad"]["boot_key"]["key"] == "FPLINUX_KEY_STAR"
    assert f"Review {reports[0]}." in capsys.readouterr().out
    assert not (tmp_path / ".cache/device-data").exists()


def test_board_report_extractor_selects_windows_executable_name(
    report_input: tuple[Path, Path, DeviceDataSpec], monkeypatch: pytest.MonkeyPatch
) -> None:
    """The platform module chooses the native basename with a controlled decoder on this host."""
    _saved, tools, _spec = report_input
    (tools / "fphelper_t117").rename(tools / "fphelper_t117.exe")
    monkeypatch.setattr(STOCK_IMAGE, "os", SimpleNamespace(name="nt"))
    nand = backup(signed_image(bytes(0x5000)), vbm=agreeing_vbm())

    report = json.loads(STOCK_IMAGE.extract_board_report(nand, host_tools=tools))

    assert report["keypad"]["boot_key"] == {"code": "0x2a", "key": "FPLINUX_KEY_STAR"}
    assert report["pads"]["display"] == {"0x402a00b0": "0x00000010"}


def test_failed_board_report_tool_publishes_no_report(
    tmp_path: Path, report_input: tuple[Path, Path, DeviceDataSpec]
) -> None:
    """An extractor process error cannot expose a partial report or selected maps."""
    saved, tools, spec = report_input
    scenario_path = tools / "scenario.json"
    scenario = json.loads(scenario_path.read_bytes())
    scenario["commands"]["unpack"] = {"exit_status": 1, "stderr": "controlled extraction error"}
    scenario_path.write_text(json.dumps(scenario), encoding="utf-8")
    result = tmp_path / "report"
    result.mkdir()

    with pytest.raises(SystemExit, match="controlled extraction error"):
        write_board_report(
            spec,
            saved,
            result,
            DeviceDataTools(host_tools=tools),
            extractor=ROOT / "platforms/ums9117/host/stock_image.py",
        )

    assert list(result.iterdir()) == []
    assert not (tmp_path / "phone").exists()
