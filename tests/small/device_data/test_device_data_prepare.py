# SPDX-License-Identifier: GPL-2.0-only
"""Phone-store admission and publication with controlled target parser boundaries."""

from __future__ import annotations

import hashlib
import json
from dataclasses import dataclass, replace
from pathlib import Path
from types import SimpleNamespace
from typing import TYPE_CHECKING, Any

import pytest
from fplinux_cli import common
from fplinux_cli.cli import device_data as cli
from fplinux_cli.device_data import prepare
from fplinux_cli.device_data.formats import (
    AudioProfileContext,
    DeviceDataPreparation,
    PhysicalNand,
    PreparedGroup,
)
from fplinux_cli.device_data.inputs import capture_phone_data
from fplinux_cli.device_data.nand_geometry import NandGeometry
from fplinux_cli.environment import definition
from fplinux_cli.environment import runtime as environment_runtime
from fplinux_cli.manifests import releases
from fplinux_cli.reporting import run as reporting
from fplinux_cli.workspace import build_inputs as workspace_build_inputs
from fplinux_cli.workspace import capture as workspace

if TYPE_CHECKING:
    from collections.abc import Callable

TARGET = "demo-phone"
RAW_DUMP = b"controlled physical NAND backup"
DECLARED_NAND = {"raw_device": "/dev/demo-nand-raw", "id": 0x21E5, "raw_page_bytes": 2112}


@dataclass(frozen=True)
class PhoneData:
    """A saved immutable input, explicit policy and controlled target parser."""

    root: Path
    backup: Path
    phone: Path
    spec: prepare.DeviceDataSpec
    groups: dict[str, PreparedGroup]

    def prepare(
        self, *, replace: bool = False, tools: prepare.DeviceDataTools | None = None
    ) -> Path:
        """Exercise shared admission, materialization, receipt and publication."""
        return prepare.prepare_from_nand(
            self.spec,
            self.backup,
            self.phone,
            tools or prepare.DeviceDataTools(),
            replace=replace,
        )


@pytest.fixture
def phone_data(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> PhoneData:
    """Own short captured NAND and replace only complete-dump admission and parser loading."""
    backup = tmp_path / "nand.bin"
    backup.write_bytes(RAW_DUMP)
    monkeypatch.setenv("HOME", str(tmp_path / "home"))
    monkeypatch.setenv("XDG_DATA_HOME", str(tmp_path / "data"))
    monkeypatch.setenv("LOCALAPPDATA", str(tmp_path / "local"))
    groups = {
        "bluetooth": PreparedGroup(
            originals={"radio-original.bin": b"fitted radio"}, prepared={"radio.bin": b"radio"}
        ),
        "audio-profile": PreparedGroup(
            originals={"nv426.bin": b"headset"},
            prepared={"audio.bin": b"profile"},
            reports={"summary.txt": b"selected profile\n"},
        ),
    }
    spec = prepare.DeviceDataSpec(
        target=TARGET,
        parser=tmp_path / "device_data.py",
        groups={
            "bluetooth": [{"source": "radio.bin", "destination": "chip/radio.bin", "size": 5}],
            "audio-profile": [
                {"source": "audio.bin", "destination": "fplinux/audio.bin", "size": 7}
            ],
        },
        audio=AudioProfileContext(
            machine_compatible=b"demo,phone",
            speaker_vibration=False,
            speaker_from_headphones=False,
            headfree_headphones_only=False,
        ),
        preparation_recipe="a" * 64,
        nand=DECLARED_NAND,
    )
    monkeypatch.setattr(
        PhysicalNand,
        "from_dump",
        lambda raw, *, page_bytes, main_decoder: PhysicalNand(raw, page_bytes, main_decoder),
    )

    def parse(_nand: PhysicalNand, *, audio: AudioProfileContext) -> DeviceDataPreparation:
        assert audio == spec.audio
        return DeviceDataPreparation(groups)

    monkeypatch.setattr(
        prepare,
        "_load_device_data_parser",
        lambda _target, _path: SimpleNamespace(prepare_device_data=parse),
    )
    return PhoneData(tmp_path, backup, tmp_path / "phone", spec, groups)


def test_saved_dump_publishes_complete_private_phone_data_without_source_copies(
    phone_data: PhoneData,
) -> None:
    """One backup feeds independent groups; the store contains prepared bytes and reports."""
    phone = phone_data.prepare()

    assert phone == phone_data.phone
    assert phone_data.backup.read_bytes() == RAW_DUMP
    assert (phone / "groups/bluetooth/radio.bin").read_bytes() == b"radio"
    assert (phone / "groups/audio-profile/audio.bin").read_bytes() == b"profile"
    assert (phone / "reports/audio-profile/summary.txt").read_bytes() == b"selected profile\n"
    assert not (phone / "source").exists()
    assert not (phone / "originals").exists()
    assert list(phone_data.root.glob("**/nand.bin")) == [phone_data.backup]
    receipt = json.loads((phone / "receipt.json").read_bytes())
    assert receipt["source"] == {
        "kind": "physical-nand",
        "size": len(RAW_DUMP),
        "sha256": hashlib.sha256(RAW_DUMP).hexdigest(),
    }
    assert [record["name"] for record in receipt["groups"]] == ["audio-profile", "bluetooth"]
    assert str(phone_data.backup) not in json.dumps(receipt)
    for path in (phone, *phone.rglob("*")):
        assert path.stat().st_mode & 0o777 == (0o700 if path.is_dir() else 0o600)
    captured = capture_phone_data(phone_data.spec.groups, phone)
    assert captured["bluetooth"][0].contents == b"radio"
    assert captured["audio-profile"][0].contents == b"profile"


def test_host_private_policy_runs_before_any_staging_contents_are_written(
    phone_data: PhoneData,
) -> None:
    """Windows can establish inherited ACLs before the first private output exists."""
    protected: list[Path] = []

    def protect(path: Path) -> None:
        assert path.is_dir()
        assert list(path.iterdir()) == []
        protected.append(path)

    phone_data.prepare(tools=prepare.DeviceDataTools(protect_private=protect))

    assert len(protected) == 1
    assert not protected[0].exists()
    assert (phone_data.phone / "groups/bluetooth/radio.bin").read_bytes() == b"radio"


def test_normal_prepare_preserves_existing_phone_data_before_reading_changed_backup(
    phone_data: PhoneData,
) -> None:
    """Only the explicit developer replacement policy can replace selected bytes."""
    phone_data.prepare()
    previous_receipt = (phone_data.phone / "receipt.json").read_bytes()
    phone_data.backup.write_bytes(b"incomplete")

    with pytest.raises(SystemExit, match="already exists"):
        phone_data.prepare()

    assert (phone_data.phone / "receipt.json").read_bytes() == previous_receipt
    assert (phone_data.phone / "groups/bluetooth/radio.bin").read_bytes() == b"radio"
    assert list(phone_data.root.glob(".phone-*")) == []


def test_failed_admission_preserves_previous_phone_data_and_cleans_staging(
    phone_data: PhoneData,
) -> None:
    """A malformed group can never expose a mixture of old and new prepared data."""
    phone_data.prepare()
    previous_receipt = (phone_data.phone / "receipt.json").read_bytes()
    phone_data.groups["audio-profile"] = PreparedGroup(
        originals={"nv426.bin": b"headset"}, prepared={"audio.bin": b"wrong size"}
    )

    with pytest.raises(SystemExit, match=r"audio-profile.*expected 7"):
        phone_data.prepare(replace=True)

    assert (phone_data.phone / "receipt.json").read_bytes() == previous_receipt
    assert (phone_data.phone / "groups/audio-profile/audio.bin").read_bytes() == b"profile"
    assert list(phone_data.root.glob(".phone-*")) == []


def test_explicit_reprepare_replaces_all_groups_without_accumulating_source_copies(
    phone_data: PhoneData,
) -> None:
    """Successful replacement keeps one store and the same single backup."""
    phone_data.prepare()
    phone_data.groups["bluetooth"] = PreparedGroup(
        originals={"radio-original.bin": b"another original"}, prepared={"radio.bin": b"other"}
    )

    phone_data.prepare(replace=True)

    assert (phone_data.phone / "groups/bluetooth/radio.bin").read_bytes() == b"other"
    assert phone_data.backup.read_bytes() == RAW_DUMP
    assert list(phone_data.root.glob(".phone-*")) == []
    assert list(phone_data.root.glob("**/nand.bin")) == [phone_data.backup]


def test_install_error_restores_previous_phone_data(
    phone_data: PhoneData, monkeypatch: pytest.MonkeyPatch
) -> None:
    """An ordinary failure installing a complete staging directory rolls the old store back."""
    phone_data.prepare()
    previous_receipt = (phone_data.phone / "receipt.json").read_bytes()
    original_replace: Callable[[Path, Path], Path] = Path.replace

    def fail_new_install(path: Path, destination: Path) -> Path:
        if path.name.startswith(".phone-") and "previous" not in path.name:
            message = "controlled install error"
            raise OSError(message)
        return original_replace(path, destination)

    monkeypatch.setattr(Path, "replace", fail_new_install)

    with pytest.raises(SystemExit, match="controlled install error"):
        phone_data.prepare(replace=True)

    assert (phone_data.phone / "receipt.json").read_bytes() == previous_receipt
    assert (phone_data.phone / "groups/bluetooth/radio.bin").read_bytes() == b"radio"
    assert list(phone_data.root.glob(".phone-*")) == []


def test_saved_dump_without_declared_geometry_never_reaches_the_parser(
    phone_data: PhoneData,
) -> None:
    """An unknown page layout needs a reader receipt and cannot be inferred from file size."""
    unknown = replace(phone_data.spec, nand={"raw_device": "/dev/demo-nand-raw"})

    with pytest.raises(SystemExit, match="declares no NAND chip and the backup has no"):
        prepare.prepare_from_nand(
            unknown, phone_data.backup, phone_data.phone, prepare.DeviceDataTools()
        )

    assert not phone_data.phone.exists()
    assert list(phone_data.root.glob(".phone-*")) == []
    assert phone_data.backup.read_bytes() == RAW_DUMP


def test_target_without_groups_rejects_explicit_preparation_before_build_or_source_access(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """Headless targets have no synthetic preparation prerequisite."""
    monkeypatch.setattr(cli, "ROOT", tmp_path)
    monkeypatch.setattr(cli, "load_target", lambda _target: {"device_data": {"groups": {}}})
    with pytest.raises(SystemExit, match="not supported for target demo-phone"):
        cli.prepare_device_data(TARGET, from_dump=None, jobs=1)
    assert not (tmp_path / ".cache/device-data").exists()


@pytest.mark.parametrize(
    "group_names", [("bluetooth",), ("audio-profile",), ("bluetooth", "audio-profile")]
)
def test_preparation_publishes_only_selected_independent_groups(
    phone_data: PhoneData, group_names: tuple[str, ...]
) -> None:
    """Audio and Bluetooth do not introduce dependencies on one another."""
    for name in tuple(phone_data.groups):
        if name not in group_names:
            phone_data.groups.pop(name)
    selected = replace(
        phone_data.spec,
        groups={name: phone_data.spec.groups[name] for name in group_names},
    )
    prepare.prepare_from_nand(
        selected, phone_data.backup, phone_data.phone, prepare.DeviceDataTools()
    )
    assert {path.name for path in (phone_data.phone / "groups").iterdir()} == set(group_names)


@pytest.mark.parametrize("explicit", [False, True], ids=["default-store", "explicit-phone"])
def test_saved_source_cli_imports_canonical_backup_without_loading_a_phone(
    phone_data: PhoneData,
    monkeypatch: pytest.MonkeyPatch,
    capsys: pytest.CaptureFixture[str],
    *,
    explicit: bool,
) -> None:
    """The shared store retains the admitted source and prepared outputs for later launches."""
    monkeypatch.setattr(cli, "ROOT", phone_data.root)
    monkeypatch.setattr(reporting, "ROOT", phone_data.root)
    config: dict[str, Any] = {
        "platform": "demo",
        "identity": {"compatible": "demo,phone"},
        "audio_policy": {
            "speaker_vibration": False,
            "speaker_from_headphones": False,
            "headfree_headphones_only": False,
        },
        "device_data": {"parser": "device_data.py", "groups": dict(phone_data.spec.groups)},
        "nand": DECLARED_NAND,
    }
    monkeypatch.setattr(cli, "load_target", lambda _target: config)
    monkeypatch.setattr(cli, "preparation_recipe", lambda *_args, **_kwargs: "a" * 64)

    def reject_phone(*_arguments: object, **_options: object) -> None:
        pytest.fail("saved-source preparation must not build or load a phone")

    for name in ("build", "prepare_target", "prepare_host_tool"):
        monkeypatch.setattr(cli, name, reject_phone)

    selected = phone_data.root / "other-phone" if explicit else None
    cli.prepare_device_data(TARGET, from_dump=phone_data.backup, jobs=2, phone_data=selected)

    default = phone_data.root / "data/fplinux/phones/demo-phone/phone"
    phone = default if selected is None else selected / "phone"
    assert (phone / "groups/bluetooth/radio.bin").read_bytes() == b"radio"
    assert phone_data.backup.read_bytes() == RAW_DUMP
    assert (phone.parent / "nand.bin").read_bytes() == RAW_DUMP
    assert (phone.parent / "nand.bin").stat().st_mode & 0o777 == 0o600
    assert not (phone.parent / "nand.bin.json").exists()
    output = capsys.readouterr().out
    assert "./fplinux run demo-phone" in output
    assert "./fplinux build demo-phone" not in output
    assert "rebuild that card with its selected profile" in output
    if selected is not None:
        assert not default.exists()
        assert f"./fplinux run demo-phone --phone-data {selected}" in output


def test_unprepared_environment_nested_build_failure_leaves_no_running_receipt(
    phone_data: PhoneData,
    monkeypatch: pytest.MonkeyPatch,
    capsys: pytest.CaptureFixture[str],
) -> None:
    """A missing environment finishes preparation without loading a phone."""
    cache = phone_data.root / ".data"
    monkeypatch.setenv("FPLINUX_DATA_DIR", str(cache))
    monkeypatch.setattr(common, "ROOT", phone_data.root)
    monkeypatch.setattr(cli, "ROOT", phone_data.root)
    monkeypatch.setattr(cli, "preparation_recipe", lambda *_args, **_kwargs: "a" * 64)
    monkeypatch.setattr(reporting, "ROOT", phone_data.root)
    monkeypatch.setattr(
        cli,
        "load_target",
        lambda _target: {
            "platform": "demo",
            "identity": {"compatible": "demo,phone"},
            "audio_policy": {
                "speaker_vibration": False,
                "speaker_from_headphones": False,
                "headfree_headphones_only": False,
            },
            "device_data": {"parser": "device_data.py", "groups": dict(phone_data.spec.groups)},
            "nand": DECLARED_NAND,
        },
    )
    monkeypatch.setattr(releases, "load_release", lambda _target: {"image": "image/ramboot.bin"})
    monkeypatch.setattr(
        workspace_build_inputs,
        "target_workspace_snapshot",
        lambda _target, _profile, **_options: workspace.WorkspaceSnapshot((), "a" * 64),
    )
    monkeypatch.setattr(definition, "load_environment_lock", dict)
    monkeypatch.setattr(definition, "environment_recipe_digest", lambda _lock: "e" * 64)
    monkeypatch.setattr(environment_runtime, "load_environment_lock", dict)
    monkeypatch.setattr(environment_runtime, "environment_recipe_digest", lambda _lock: "e" * 64)

    def missing_kern(_lock: object, *, cache: Path) -> bool:
        assert cache == phone_data.root / ".data"
        return False

    monkeypatch.setattr(environment_runtime, "kern_available", missing_kern)

    def reject_load(*_args: object, **_kwargs: object) -> None:
        pytest.fail("a failed build must not load a phone")

    monkeypatch.setattr(cli, "prepare_target", reject_load)

    with pytest.raises(SystemExit) as raised:
        reporting.run_entrypoint(lambda: cli.prepare_device_data(TARGET, from_dump=None, jobs=1))

    assert raised.value.code == 1
    assert capsys.readouterr().err.count("run ./fplinux setup demo-phone") == 1
    receipts = list((cache / "logs").rglob("run.json"))
    assert receipts
    assert all(json.loads(receipt.read_bytes())["status"] == "failed" for receipt in receipts)


@pytest.mark.parametrize(
    ("nand", "receipt", "expected"),
    [
        (DECLARED_NAND, None, 2112),
        ({"id": 0xB1A1, "raw_page_bytes": 2176}, None, 2176),
        (None, NandGeometry("e521", "demo", 2048, 64, 64, 1024, 138412032), 2112),
        (
            {"raw_device": "/dev/ums9117-nand-raw"},
            NandGeometry("e521", "DS35M1GA", 2048, 64, 64, 1024, 138412032),
            2112,
        ),
        (
            {"raw_device": "/dev/ums9117-nand-raw"},
            NandGeometry("a1a5", "FM25LS01", 2048, 128, 64, 1024, 142606336),
            2176,
        ),
        (DECLARED_NAND, NandGeometry("e521", "demo", 2048, 64, 64, 1024, 138412032), 2112),
    ],
)
def test_dump_page_size_comes_from_receipt_or_declared_chip(
    nand: dict[str, Any] | None, receipt: NandGeometry | None, expected: int
) -> None:
    assert prepare.dump_page_bytes(TARGET, nand, receipt) == expected


@pytest.mark.parametrize(
    ("nand", "receipt", "message"),
    [
        ({"raw_device": "/dev/demo"}, None, "declares no NAND"),
        (
            DECLARED_NAND,
            NandGeometry("a1b1", "demo", 2048, 128, 64, 1024, 142606336),
            "reported id_bytes=a1b1",
        ),
        (
            None,
            NandGeometry("c8b4", "demo", 4096, 256, 64, 1024, 285212672),
            "4096-byte main pages",
        ),
        (None, NandGeometry("c8f1", "demo", 2048, 64, 64, 512, 69206016), "512 blocks"),
    ],
)
def test_dump_missing_contradicting_or_unsupported_geometry_is_refused(
    nand: dict[str, Any] | None, receipt: NandGeometry | None, message: str
) -> None:
    with pytest.raises(SystemExit, match=message):
        prepare.dump_page_bytes(TARGET, nand, receipt)
