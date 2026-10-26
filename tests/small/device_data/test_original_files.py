# SPDX-License-Identifier: GPL-2.0-only
"""Named-original capture and publication through the host preparation boundary."""

from __future__ import annotations

import json
from types import SimpleNamespace
from typing import TYPE_CHECKING

import pytest
from fplinux_cli.cli import device_data as cli_device_data
from fplinux_cli.device_data import prepare
from fplinux_cli.device_data.formats import (
    AudioProfileContext,
    DeviceDataPreparation,
    PreparedGroup,
)
from fplinux_cli.reporting import run as reporting

if TYPE_CHECKING:
    from collections.abc import Mapping
    from pathlib import Path


def test_original_preparation_publishes_the_complete_capture_when_source_paths_change(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """Only acquisition reads the source paths; later stages use the admitted byte capture.

    The target parser is a controlled callable. Source admission, staging,
    materialization and publication are real; no loader or phone boundary runs.
    """
    cache = tmp_path / ".cache"
    cache.mkdir()
    original = tmp_path / "radio-original.bin"
    original.write_bytes(b"abc")
    manifest = tmp_path / "originals.json"
    expected_manifest = (
        json.dumps(
            {
                "target": "example",
                "directory": ".",
                "files": [
                    {
                        "name": "radio-original.bin",
                        "size": 3,
                        "sha256": (
                            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
                        ),
                    }
                ],
            },
            indent=2,
        ).encode()
        + b"\n"
    )
    manifest.write_bytes(expected_manifest)

    def parse(originals: Mapping[str, bytes]) -> DeviceDataPreparation:
        assert dict(originals) == {"radio-original.bin": b"abc"}
        original.write_bytes(b"different source now")
        manifest.write_bytes(b"not the captured manifest")
        return DeviceDataPreparation(
            groups={
                "wcn-firmware": PreparedGroup(
                    originals=dict(originals),
                    prepared={"payload.bin": originals["radio-original.bin"]},
                )
            }
        )

    def reject_phone_access(*_args: object, **_kwargs: object) -> None:
        pytest.fail("named-original preparation must not build, load or back up a phone")

    for module in (cli_device_data, reporting):
        monkeypatch.setattr(module, "ROOT", tmp_path)
    monkeypatch.setenv("XDG_DATA_HOME", str(tmp_path / "user-data"))
    monkeypatch.setattr(cli_device_data, "preparation_recipe", lambda *_args, **_kwargs: "a" * 64)
    monkeypatch.setattr(
        cli_device_data,
        "load_target",
        lambda _target: {
            "identity": {"compatible": "example,phone"},
            "audio_policy": {
                "speaker_vibration": False,
                "speaker_from_headphones": False,
                "headfree_headphones_only": False,
            },
            "device_data": {
                "originals_parser": "controlled.py",
                "groups": {
                    "wcn-firmware": [
                        {"source": "payload.bin", "destination": "payload.bin", "size": 3}
                    ]
                },
            },
        },
    )
    monkeypatch.setattr(
        prepare,
        "_load_originals_parser",
        lambda _target, _filename: SimpleNamespace(prepare_from_originals=parse),
    )
    for name in ("build", "prepare_target", "prepare_host_tool"):
        monkeypatch.setattr(cli_device_data, name, reject_phone_access)

    cli_device_data.prepare_device_data("example", from_dump=None, from_originals=manifest, jobs=1)

    target_root = tmp_path / "user-data/fplinux/phones/example"
    phone = target_root / "phone"
    assert (phone / "groups/wcn-firmware/payload.bin").read_bytes() == b"abc"
    assert not (phone / "source").exists()
    assert not (phone / "originals").exists()
    receipt = json.loads((phone / "receipt.json").read_bytes())
    assert receipt["source"]["size"] == len(expected_manifest)
    assert receipt["groups"][0]["originals"] == [
        {
            "source": "radio-original.bin",
            "size": 3,
            "sha256": "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        }
    ]
    assert original.read_bytes() == b"different source now"
    assert manifest.read_bytes() == b"not the captured manifest"


@pytest.mark.parametrize(
    ("originals", "prepared", "message"),
    [
        ({"radio-original.bin": b"changed"}, {"payload.bin": b"abc"}, "preserve the admitted"),
        (
            {"radio-original.bin": b"abc"},
            {"payload.bin": b"abc"},
            "preserve the complete admitted",
        ),
        (
            {"radio-original.bin": b"abc", "other-original.bin": b"d"},
            {"unexpected.bin": b"abc"},
            "invalid prepared set",
        ),
    ],
    ids=["changed-original", "omitted-original", "undeclared-output"],
)
def test_original_preparer_cannot_drop_change_or_misdeclare_admitted_files(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    originals: dict[str, bytes],
    prepared: dict[str, bytes],
    message: str,
) -> None:
    """The common result boundary checks retention and declared outputs before staging."""
    parser = SimpleNamespace(
        prepare_from_originals=lambda _originals: DeviceDataPreparation(
            groups={"wcn-firmware": PreparedGroup(originals=originals, prepared=prepared)}
        )
    )
    monkeypatch.setattr(prepare, "_load_originals_parser", lambda _target, _filename: parser)

    with pytest.raises(SystemExit, match=message):
        prepare._extract_original_groups(  # noqa: SLF001 -- exercise the parser-result boundary.
            prepare.DeviceDataSpec(
                target="example",
                preparation_recipe="a" * 64,
                parser=None,
                originals_parser=tmp_path / "controlled.py",
                audio=AudioProfileContext(
                    machine_compatible=b"example,phone",
                    speaker_vibration=False,
                    speaker_from_headphones=False,
                    headfree_headphones_only=False,
                ),
                groups={
                    "wcn-firmware": [
                        {"source": "payload.bin", "destination": "payload.bin", "size": 3}
                    ]
                },
            ),
            {"radio-original.bin": b"abc", "other-original.bin": b"d"},
        )
