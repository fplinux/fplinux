# SPDX-License-Identifier: GPL-2.0-only
"""Behavioral checks for one-source device-data generation publication."""

from __future__ import annotations

import contextlib
import hashlib
import io
import json
import shutil
import tempfile
from dataclasses import dataclass
from pathlib import Path
from types import SimpleNamespace
from typing import TYPE_CHECKING
from unittest import mock

import pytest
from fplinux_cli import common
from fplinux_cli.cli import device_data as device_data_prepare
from fplinux_cli.device_data import prepare as device_data_prepare_prepare
from fplinux_cli.device_data.formats import (
    AudioProfileContext,
    DeviceDataPreparation,
    NandGeometry,
    PhysicalNand,
    PreparedGroup,
)
from fplinux_cli.environment import images, kern
from fplinux_cli.manifests import releases
from fplinux_cli.reporting.run import run_entrypoint
from fplinux_cli.workspace import build_inputs as workspace_build_inputs
from fplinux_cli.workspace import capture as workspace

from tests import ROOT
from tests.fixtures.stock_board_report import agreeing_vbm, backup, signed_image

if TYPE_CHECKING:
    from collections.abc import Iterator

TARGET = "demo-phone"
RAW_PAGE_BYTES = 2112
DECLARED_NAND = {"raw_device": "/dev/demo-nand-raw", "id": 0x21E5, "raw_page_bytes": 2112}
RAW_DUMP = b"controlled physical NAND backup"
BLUETOOTH = PreparedGroup(
    originals={"radio-original.bin": b"fitted radio"},
    prepared={"radio.bin": b"radio"},
)
AUDIO_PROFILE = PreparedGroup(
    originals={"nv425.bin": b"mode", "nv426.bin": b"headset", "nv440.bin": b"eq"},
    prepared={"audio.bin": b"profile"},
)
BOARD_REPORT = b'{"keypad": {"rows": 5}}\n'
BOARD_MAPS = PreparedGroup(
    originals={"stock-image.bin": b"stock image"},
    prepared={"pinmap.bin": b"pins", "keymap.bin": b"keys"},
    reports={"board-report.json": BOARD_REPORT},
)


@dataclass(frozen=True)
class DeviceDataTree:
    """Isolated source checkout, selected generations and immutable saved dump."""

    root: Path
    cache: Path
    saved_dump: Path

    @staticmethod
    def group(source: str, destination: str, size: int) -> list[dict[str, object]]:
        """Declare one controlled source file and its admitted destination size."""
        return [
            {
                "source": source,
                "destination": destination,
                "size": size,
            }
        ]

    def config(
        self,
        group_names: tuple[str, ...] = ("bluetooth", "audio-profile"),
        *,
        nand: dict[str, object] = DECLARED_NAND,
    ) -> dict[str, object]:
        """Declare the selected groups and geometry without loading a real target."""
        groups: dict[str, list[dict[str, object]]] = {}
        if "bluetooth" in group_names:
            groups["bluetooth"] = self.group("radio.bin", "chip/radio.bin", 5)
        if "audio-profile" in group_names:
            groups["audio-profile"] = self.group(
                "audio.bin",
                "fplinux/audio.bin",
                7,
            )
        return {
            "identity": {"compatible": "demo,phone"},
            "audio_policy": {
                "speaker_vibration": False,
                "speaker_from_headphones": False,
                "headfree_headphones_only": False,
            },
            "device_data": {"parser": "device_data.py", "groups": groups},
            "nand": nand,
        }

    @staticmethod
    def preparation(
        group_names: tuple[str, ...] = ("bluetooth", "audio-profile"),
        *,
        audio: PreparedGroup = AUDIO_PROFILE,
    ) -> DeviceDataPreparation:
        """Return controlled parser output for the selected independent groups."""
        groups = {}
        if "bluetooth" in group_names:
            groups["bluetooth"] = BLUETOOTH
        if "audio-profile" in group_names:
            groups["audio-profile"] = audio
        return DeviceDataPreparation(groups=groups)

    def run_from_dump(
        self,
        group_names: tuple[str, ...] = ("bluetooth", "audio-profile"),
        *,
        preparation: DeviceDataPreparation | None = None,
    ) -> str:
        """Prepare this saved dump with controlled parsing and prohibited phone access."""
        parse = mock.Mock(return_value=preparation or self.preparation(group_names))
        parser = SimpleNamespace(prepare_device_data=parse)
        admitted = PhysicalNand(RAW_DUMP, RAW_PAGE_BYTES)
        with (
            mock.patch.object(device_data_prepare, "ROOT", self.root),
            mock.patch.object(
                device_data_prepare,
                "load_target",
                return_value=self.config(group_names),
            ),
            mock.patch.object(
                device_data_prepare_prepare,
                "_load_device_data_parser",
                return_value=parser,
            ),
            mock.patch.object(
                PhysicalNand,
                "from_dump",
                return_value=admitted,
            ),
            mock.patch.object(
                device_data_prepare,
                "build",
                side_effect=AssertionError("saved-dump preparation must not build a loader"),
            ),
            mock.patch.object(
                device_data_prepare,
                "run_target_noninteractive",
                side_effect=AssertionError("saved-dump preparation must not load a phone"),
            ),
            mock.patch.object(
                device_data_prepare,
                "backup_target_nand",
                side_effect=AssertionError("saved-dump preparation must not access SSH"),
            ),
            contextlib.redirect_stdout(io.StringIO()) as stdout,
        ):
            device_data_prepare.prepare_device_data(
                TARGET,
                from_dump=self.saved_dump,
                jobs=3,
                offline=True,
            )
        return stdout.getvalue()

    def current_generation(self) -> Path:
        """Resolve the generation selected by the producer's published current pointer."""
        target_root = self.cache / "device-data" / TARGET
        generation_name = (target_root / "current").read_text(encoding="ascii").strip()
        return target_root / "generations" / generation_name

    def run_board_maps_from_dump(
        self,
        *,
        with_bluetooth: bool,
        extract: object,
    ) -> str:
        """Prepare board maps with a stub extraction and pinned host-tool preparation."""
        groups: dict[str, list[dict[str, object]]] = {
            "board-maps": [
                # Board maps are declared without sizes; each phone's own maps are admitted.
                {"source": "pinmap.bin", "destination": "pinmap.bin"},
                {"source": "keymap.bin", "destination": "keymap.bin"},
            ]
        }
        device_data: dict[str, object] = {"groups": groups}
        if with_bluetooth:
            groups["bluetooth"] = self.group("radio.bin", "chip/radio.bin", 5)
            device_data["parser"] = "device_data.py"
        parser = SimpleNamespace(
            prepare_device_data=lambda _nand, **_context: DeviceDataPreparation(
                groups={"bluetooth": BLUETOOTH}
            )
        )
        with (
            mock.patch.object(device_data_prepare, "ROOT", self.root),
            mock.patch.object(
                device_data_prepare,
                "load_target",
                return_value={
                    **self.config(),
                    "platform": "demo-platform",
                    "device_data": device_data,
                    "nand": DECLARED_NAND,
                },
            ),
            mock.patch.object(
                device_data_prepare_prepare,
                "_load_device_data_parser",
                return_value=parser,
                side_effect=None if with_bluetooth else AssertionError("no parser group"),
            ),
            mock.patch.object(
                device_data_prepare_prepare,
                "_load_board_maps_provider",
                return_value=SimpleNamespace(prepare_board_maps=extract),
            ),
            mock.patch.object(
                device_data_prepare,
                "prepare_host_tool",
                return_value=None,
            ),
            mock.patch.object(
                PhysicalNand,
                "from_dump",
                return_value=PhysicalNand(RAW_DUMP, RAW_PAGE_BYTES),
            ),
            contextlib.redirect_stdout(io.StringIO()) as stdout,
        ):
            device_data_prepare.prepare_device_data(
                TARGET,
                from_dump=self.saved_dump,
                jobs=1,
                offline=True,
            )
        return stdout.getvalue()


@pytest.fixture
def device_data_tree() -> Iterator[DeviceDataTree]:
    """Own an isolated source root and saved dump until case teardown."""
    with contextlib.ExitStack() as resources:
        temporary = tempfile.TemporaryDirectory()
        resources.enter_context(temporary)
        root = Path(temporary.name) / "source"
        cache = root / ".cache"
        cache.mkdir(parents=True)
        resources.enter_context(mock.patch("fplinux_cli.reporting.run.ROOT", root))
        saved_dump = root / "saved-nand.bin"
        saved_dump.write_bytes(RAW_DUMP)
        yield DeviceDataTree(root, cache, saved_dump)


class DeviceDataPrepareTests:
    """Exercise real staging, receipts and pointer publication under a temporary root.

    Stubbed: the target configuration (load_target); loading of the target parser and
    platform board-map modules, replaced by in-test callables; PhysicalNand.from_dump
    admission of the short dump; pinned host-tool preparation;
    and the loader build, RAM load and NAND backup. One case instead runs the real
    build up to its offline rejection with stubbed release, workspace, image and Kern
    inputs. The cold-dump case instead runs full-size admission, the real platform
    module and an external fake utility. Real target TOML, target parser files,
    pinned container builds, real-tool discovery and phone transport are not exercised.
    """

    def test_saved_dump_publishes_both_groups_with_one_private_source_receipt(
        self, device_data_tree: DeviceDataTree
    ) -> None:
        """One immutable input is copied once and owns provenance for both independent groups."""
        output = device_data_tree.run_from_dump()

        generation = device_data_tree.current_generation()
        assert (device_data_tree.saved_dump.read_bytes()) == (RAW_DUMP)
        assert ((generation / "source/nand.bin").read_bytes()) == (RAW_DUMP)
        assert ((generation / "groups/bluetooth/radio.bin").read_bytes()) == (b"radio")
        assert ((generation / "groups/audio-profile/audio.bin").read_bytes()) == (b"profile")
        assert ((generation / "originals/audio-profile/nv426.bin").read_bytes()) == (b"headset")
        receipt = json.loads((generation / "receipt.json").read_text(encoding="utf-8"))
        assert (receipt["source"]) == (
            {
                "kind": "saved-dump",
                "size": len(RAW_DUMP),
                "sha256": hashlib.sha256(RAW_DUMP).hexdigest(),
            }
        )
        assert ([group["name"] for group in receipt["groups"]]) == (["audio-profile", "bluetooth"])
        assert (str(device_data_tree.saved_dump)) not in (json.dumps(receipt))
        assert ("./fplinux build demo-phone") in (output)

    def test_failed_staging_does_not_switch_the_current_generation(
        self, device_data_tree: DeviceDataTree
    ) -> None:
        """A partial or malformed requested group cannot expose a mixed current state."""
        device_data_tree.run_from_dump()
        target_root = device_data_tree.cache / "device-data" / TARGET
        pointer = target_root / "current"
        previous_pointer = pointer.read_bytes()
        previous_generation = device_data_tree.current_generation()
        malformed = PreparedGroup(
            originals=AUDIO_PROFILE.originals,
            prepared={"audio.bin": b"wrong size"},
        )

        with pytest.raises(SystemExit, match="device-data group audio-profile"):
            device_data_tree.run_from_dump(
                preparation=device_data_tree.preparation(audio=malformed)
            )

        assert (pointer.read_bytes()) == (previous_pointer)
        assert (device_data_tree.current_generation()) == (previous_generation)
        assert ((previous_generation / "groups/audio-profile/audio.bin").read_bytes()) == (
            b"profile"
        )
        staging = list((target_root / "generations").glob(".staging-*"))
        assert (staging) == ([])

    def test_live_acquisition_runs_once_and_the_same_raw_feeds_both_groups(
        self, device_data_tree: DeviceDataTree
    ) -> None:
        """One public NAND backup is the observable safety boundary for all parser outputs."""
        events: list[object] = []
        admitted = PhysicalNand(RAW_DUMP, RAW_PAGE_BYTES)

        def backup(target: str, destination: Path, **_logging: object) -> Path:
            events.append(("backup", target))
            destination.write_bytes(RAW_DUMP)
            destination.chmod(0o600)
            return destination

        def extract(nand: PhysicalNand, *, audio: AudioProfileContext) -> DeviceDataPreparation:
            del audio
            events.append(("extract", nand))
            return device_data_tree.preparation()

        def admit(raw: bytes, *, page_bytes: int) -> PhysicalNand:
            events.append(("admit", raw, page_bytes))
            return admitted

        with (
            mock.patch.object(device_data_prepare, "ROOT", device_data_tree.root),
            mock.patch.object(
                device_data_prepare,
                "load_target",
                return_value=device_data_tree.config(),
            ),
            mock.patch.object(
                device_data_prepare,
                "build",
                side_effect=lambda *_args, **_kwargs: events.append("build"),
            ),
            mock.patch.object(
                device_data_prepare,
                "run_target_noninteractive",
                side_effect=lambda *_args, **_kwargs: events.append("load"),
            ),
            mock.patch.object(device_data_prepare, "backup_target_nand", side_effect=backup),
            mock.patch.object(
                PhysicalNand,
                "from_dump",
                side_effect=admit,
            ),
            mock.patch.object(
                device_data_prepare_prepare,
                "_load_device_data_parser",
                return_value=SimpleNamespace(prepare_device_data=extract),
            ),
            contextlib.redirect_stdout(io.StringIO()),
        ):
            device_data_prepare.prepare_device_data(
                TARGET,
                from_dump=None,
                jobs=2,
                offline=True,
            )

        assert (events) == (
            [
                "build",
                "load",
                ("backup", TARGET),
                ("admit", RAW_DUMP, RAW_PAGE_BYTES),
                ("extract", admitted),
            ]
        )
        assert ((device_data_tree.current_generation() / "source/nand.bin").read_bytes()) == (
            RAW_DUMP
        )

    @pytest.mark.parametrize(
        "group_names",
        [
            pytest.param(("bluetooth",), id="bluetooth"),
            pytest.param(("audio-profile",), id="audio-profile"),
            pytest.param(("bluetooth", "audio-profile"), id="both-groups"),
        ],
    )
    def test_bluetooth_audio_and_combined_targets_publish_only_their_declared_groups(
        self, device_data_tree: DeviceDataTree, group_names: tuple[str, ...]
    ) -> None:
        """Neither named group creates a dependency on the other."""
        device_data_tree.run_from_dump(group_names)
        published = device_data_tree.current_generation() / "groups"
        assert {path.name for path in published.iterdir()} == set(group_names)

    def test_saved_dump_board_maps_publish_without_a_runtime_bundle(
        self,
        device_data_tree: DeviceDataTree,
    ) -> None:
        """Platform output is published beside parser groups; its report is not a build input."""
        received: list[Path] = []

        def extract(nand: PhysicalNand, *, host_tools: Path) -> PreparedGroup:
            assert (nand.raw) == (RAW_DUMP)
            received.append(host_tools)
            return BOARD_MAPS

        output = device_data_tree.run_board_maps_from_dump(with_bluetooth=True, extract=extract)

        generation = device_data_tree.current_generation()
        assert (len(received)) == (1)
        assert not (received[0].exists())
        assert ((generation / "groups/board-maps/pinmap.bin").read_bytes()) == (b"pins")
        assert ((generation / "groups/board-maps/keymap.bin").read_bytes()) == (b"keys")
        assert ((generation / "groups/bluetooth/radio.bin").read_bytes()) == (b"radio")
        assert ((generation / "originals/board-maps/stock-image.bin").read_bytes()) == (
            b"stock image"
        )
        report = generation / "reports/board-maps/board-report.json"
        assert (report.read_bytes()) == (BOARD_REPORT)
        assert (report.stat().st_mode & 0o777) == (0o600)
        assert not ((generation / "groups/board-maps/board-report.json").exists())
        receipt = json.loads((generation / "receipt.json").read_text(encoding="utf-8"))
        groups = {group["name"]: group for group in receipt["groups"]}
        assert (groups["board-maps"]["reports"]) == (
            [
                {
                    "name": "board-report.json",
                    "size": len(BOARD_REPORT),
                    "sha256": hashlib.sha256(BOARD_REPORT).hexdigest(),
                }
            ]
        )
        assert ("reports") not in (groups["bluetooth"])
        assert (f"Review {report}.") in (output)

    def test_cold_saved_dump_publishes_maps_from_a_tool_process(
        self, device_data_tree: DeviceDataTree
    ) -> None:
        """A fake external utility feeds the real platform extractor without a runtime bundle.

        Pinned container preparation is replaced by installation of the controlled utility.
        Full dump admission, stock-image parsing, process execution and publication are real.
        This does not establish real-tool table discovery or the container build boundary.
        """
        project = ROOT
        image = signed_image(bytes(0x5000))
        small = backup(image, vbm=agreeing_vbm())
        with device_data_tree.saved_dump.open("wb") as dump_writer:
            dump_writer.write(small.raw)
            dump_writer.truncate(138412032)
        with device_data_tree.saved_dump.open("rb") as dump_reader:
            original_digest = hashlib.file_digest(dump_reader, "sha256").hexdigest()
        provider = device_data_tree.root / "platforms/ums9117/host/stock_image.py"
        provider.parent.mkdir(parents=True)
        shutil.copyfile(project / "platforms/ums9117/host/stock_image.py", provider)
        pinmap = bytes.fromhex("b0002a40 10000000 ffffffff ffffffff")
        keys = bytes.fromhex("2a00 3100")
        scenario = {
            "image_sha256": hashlib.sha256(image).hexdigest(),
            "commands": {
                "unpack": {
                    "stdout": (
                        project / "tests/fixtures/stock-board-report/maps-only.txt"
                    ).read_text(encoding="utf-8"),
                    "files": {"pinmap.bin": pinmap.hex(), "keymap.bin": keys.hex()},
                }
            },
        }

        def install_tool(
            _target: str,
            _platform: str,
            _tool: str,
            output: Path,
            *,
            offline: bool,
            reporter: object,
        ) -> None:
            del reporter
            assert offline
            executable = output / "fphelper_t117"
            shutil.copyfile(project / "tests/fixtures/fphelper/fake_fphelper.py", executable)
            executable.chmod(0o755)
            (output / "scenario.json").write_text(json.dumps(scenario), encoding="utf-8")

        config = {
            **device_data_tree.config(),
            "platform": "ums9117",
            "nand": DECLARED_NAND,
            "device_data": {
                "groups": {
                    "board-maps": [
                        {"source": "pinmap.bin", "destination": "pinmap.bin"},
                        {"source": "keymap.bin", "destination": "keymap.bin"},
                    ]
                }
            },
        }
        with (
            mock.patch.object(common, "ROOT", device_data_tree.root),
            mock.patch.object(device_data_prepare, "ROOT", device_data_tree.root),
            mock.patch.object(device_data_prepare, "load_target", return_value=config),
            mock.patch.object(
                device_data_prepare, "prepare_host_tool", side_effect=install_tool, create=True
            ),
            contextlib.redirect_stdout(io.StringIO()),
        ):
            device_data_prepare.prepare_device_data(
                TARGET, from_dump=device_data_tree.saved_dump, jobs=1, offline=True
            )

        generation = device_data_tree.current_generation()
        assert ((generation / "groups/board-maps/pinmap.bin").read_bytes()) == (pinmap)
        assert ((generation / "groups/board-maps/keymap.bin").read_bytes()) == (keys)
        report = json.loads((generation / "reports/board-maps/board-report.json").read_text())
        assert (report["keypad"]["boot_key"]) == ({"code": "0x2a", "key": "FPLINUX_KEY_STAR"})
        assert (report["pads"]["display"]) == ({"0x402a00b0": "0x00000010"})
        assert ((generation / "originals/board-maps/stock-image.bin").read_bytes()) == (image)
        with device_data_tree.saved_dump.open("rb") as dump_reader:
            assert (hashlib.file_digest(dump_reader, "sha256").hexdigest()) == (original_digest)
        assert not ((device_data_tree.cache / "out").exists())

    def test_failed_board_map_extraction_publishes_nothing(
        self, device_data_tree: DeviceDataTree
    ) -> None:
        """A failed extraction leaves no selected generation."""

        def unavailable(_nand: PhysicalNand, *, host_tools: Path) -> PreparedGroup:
            del host_tools
            message = "board maps not found in the stock image"
            raise ValueError(message)

        with pytest.raises(
            SystemExit,
            match="device-data extraction failed: board maps not found in the stock image",
        ):
            device_data_tree.run_board_maps_from_dump(with_bluetooth=False, extract=unavailable)
        target_root = device_data_tree.cache / "device-data" / TARGET
        assert not ((target_root / "current").exists())
        assert (list((target_root / "generations").iterdir())) == ([])

    def test_saved_dump_without_declared_chip_or_receipt_publishes_nothing(
        self, device_data_tree: DeviceDataTree
    ) -> None:
        """A backup's page layout is never inferred from its length."""
        with (
            mock.patch.object(device_data_prepare, "ROOT", device_data_tree.root),
            mock.patch.object(
                device_data_prepare,
                "load_target",
                return_value=device_data_tree.config(nand={"raw_device": "/dev/demo-nand-raw"}),
            ),
            mock.patch.object(
                PhysicalNand,
                "from_dump",
                side_effect=AssertionError("an unknown layout must not be admitted"),
            ),
            mock.patch.object(
                device_data_prepare_prepare,
                "_load_device_data_parser",
                side_effect=AssertionError("an unknown layout must not reach a parser"),
            ),
            pytest.raises(SystemExit, match="declares no NAND chip and the backup has no"),
        ):
            device_data_prepare.prepare_device_data(
                TARGET,
                from_dump=device_data_tree.saved_dump,
                jobs=1,
                offline=True,
            )

        target_root = device_data_tree.cache / "device-data" / TARGET
        assert not ((target_root / "current").exists())
        assert (list((target_root / "generations").iterdir())) == ([])
        assert (device_data_tree.saved_dump.read_bytes()) == (RAW_DUMP)

    def test_target_without_declared_groups_stops_before_source_or_phone_access(
        self, device_data_tree: DeviceDataTree
    ) -> None:
        """Unsupported preparation cannot acquire a source or create a generation."""
        with (
            mock.patch.object(device_data_prepare, "ROOT", device_data_tree.root),
            mock.patch.object(
                device_data_prepare,
                "load_target",
                return_value={"device_data": {"groups": {}}},
            ),
            mock.patch.object(device_data_prepare, "build") as build,
            pytest.raises(SystemExit, match="not supported for target demo-phone"),
        ):
            device_data_prepare.prepare_device_data(
                TARGET,
                from_dump=None,
                jobs=1,
                offline=True,
            )
        build.assert_not_called()
        assert not ((device_data_tree.cache / "device-data").exists())

    def test_offline_nested_build_failure_leaves_no_running_receipt(
        self, device_data_tree: DeviceDataTree
    ) -> None:
        """A real build rejection finishes its enclosing preparation without loading a phone."""
        terminal = io.StringIO()
        with (
            mock.patch.object(common, "ROOT", device_data_tree.root),
            mock.patch.object(device_data_prepare, "ROOT", device_data_tree.root),
            mock.patch.object(
                device_data_prepare, "load_target", return_value=device_data_tree.config()
            ),
            mock.patch.object(
                releases, "load_release", return_value={"image": "image/ramboot.bin"}
            ),
            mock.patch.object(
                workspace_build_inputs,
                "target_workspace_snapshot",
                return_value=workspace.WorkspaceSnapshot((), "a" * 64),
            ),
            mock.patch.object(
                images,
                "load_container_lock",
                return_value={"oci": {"repository": "localhost/fplinux-build"}},
            ),
            mock.patch.object(images, "container_image_recipe_digest", return_value="e" * 64),
            mock.patch.object(kern, "kern_available", return_value=False),
            mock.patch.object(
                device_data_prepare,
                "run_target_noninteractive",
                side_effect=AssertionError("failed preparation must not load a phone"),
            ),
            contextlib.redirect_stderr(terminal),
            pytest.raises(SystemExit) as raised,
        ):
            run_entrypoint(
                lambda: device_data_prepare.prepare_device_data(
                    TARGET, from_dump=None, jobs=1, offline=True
                )
            )
        assert (raised.value.code) == (1)
        assert (terminal.getvalue().count("offline build requires")) == (1)
        receipts = list((device_data_tree.cache / "logs").rglob("run.json"))
        assert receipts
        for receipt in receipts:
            assert (json.loads(receipt.read_text())["status"]) == ("failed")


def _reported(  # noqa: PLR0913 -- each reported value stays visible at the call site.
    *,
    id_bytes: str,
    page_main_bytes: int = 2048,
    oob_bytes: int,
    pages_per_block: int = 64,
    block_count: int = 1024,
    raw_bytes: int,
) -> NandGeometry:
    return NandGeometry(
        id_bytes=id_bytes,
        chip="demo",
        page_main_bytes=page_main_bytes,
        oob_bytes=oob_bytes,
        pages_per_block=pages_per_block,
        block_count=block_count,
        raw_bytes=raw_bytes,
    )


class DumpGeometryTests:
    """Select a saved backup's page layout without guessing it from the backup length."""

    @pytest.mark.parametrize(
        "case",
        [
            "declared chip only",
            "declared 128-byte OOB chip only",
            "receipt only",
            "receipt without a NAND table",
            "receipt and declared chip agree",
        ],
        ids=["declared-64-oob", "declared-128-oob", "receipt", "receipt-no-table", "agree"],
    )
    def test_page_size_comes_from_the_receipt_or_the_declared_chip(self, case: str) -> None:
        """Either source is sufficient, and agreeing sources give the same page size."""
        receipt_2112 = _reported(id_bytes="e521", oob_bytes=64, raw_bytes=138412032)
        cases = (
            ("declared chip only", DECLARED_NAND, None, 2112),
            (
                "declared 128-byte OOB chip only",
                {"raw_device": "/dev/demo-nand-raw", "id": 0xB1A1, "raw_page_bytes": 2176},
                None,
                2176,
            ),
            ("receipt only", {"raw_device": "/dev/demo-nand-raw"}, receipt_2112, 2112),
            ("receipt without a NAND table", None, receipt_2112, 2112),
            ("receipt and declared chip agree", DECLARED_NAND, receipt_2112, 2112),
        )
        _, nand, receipt, expected = next(item for item in cases if item[0] == case)
        assert device_data_prepare_prepare.dump_page_bytes(TARGET, nand, receipt) == expected

    @pytest.mark.parametrize(
        "case",
        ["neither source", "receipt from another chip", "4 KiB main pages", "half the pages"],
        ids=["no-source", "different-chip", "unsupported-main-page", "unsupported-block-count"],
    )
    def test_missing_contradicting_or_unsupported_geometry_is_refused(self, case: str) -> None:
        """A receipt from another chip or outside the interpreted layout cannot be used."""
        cases = (
            ("neither source", {"raw_device": "/dev/demo-nand-raw"}, None, "declares no NAND"),
            (
                "receipt from another chip",
                DECLARED_NAND,
                _reported(id_bytes="a1b1", oob_bytes=128, raw_bytes=142606336),
                "reported id_bytes=a1b1 with 2176-byte pages; target declares id_bytes=e521",
            ),
            (
                "4 KiB main pages",
                None,
                _reported(
                    id_bytes="c8b4",
                    page_main_bytes=4096,
                    oob_bytes=256,
                    raw_bytes=285212672,
                ),
                "4096-byte main pages",
            ),
            (
                "half the pages",
                None,
                _reported(id_bytes="c8f1", oob_bytes=64, block_count=512, raw_bytes=69206016),
                "512 blocks",
            ),
        )
        _, nand, receipt, message = next(item for item in cases if item[0] == case)
        with pytest.raises(SystemExit, match=message):
            device_data_prepare_prepare.dump_page_bytes(TARGET, nand, receipt)
