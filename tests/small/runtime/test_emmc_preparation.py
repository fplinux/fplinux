# SPDX-License-Identifier: GPL-2.0-only
"""Real source reuse and preparation with only the phone connection replaced."""

from __future__ import annotations

import hashlib
import json
import shutil
import subprocess
from typing import TYPE_CHECKING, Any

from fplinux_cli.runtime import boot, emmc_files, workflow
from fplinux_cli.runtime.bundle import ValidatedBundle
from fplinux_cli.runtime.events import RunLog

from tests import ROOT
from tests.fixtures.portable_archive import capsule_files

if TYPE_CHECKING:
    from collections.abc import Callable
    from pathlib import Path

    import pytest


def test_first_emmc_preparation_reads_once_and_next_launch_reuses_originals(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """The actual publisher/cache reprepare saved files after a parser change, without USB."""
    shutil.copyfile(ROOT / "tests/fixtures/original-files/audio_parser.py", tmp_path / "parser.py")
    data = tmp_path / "runtime"
    store = tmp_path / "store"
    data.mkdir()
    store.mkdir()
    runtime: dict[str, Any] = {
        "target": "example",
        "identity": {"target": {"compatible": "test,phone"}},
        "host_tools": {},
        "nand": None,
        "emmc": {
            "raw_device": "/dev/mmcblk1",
            "files": [
                {
                    "name": "audio.conf",
                    "locations": [["system", "/etc/audio.conf"]],
                    "max_bytes": 64,
                }
            ],
            "acquisition_recipe": "a" * 64,
        },
        "device_data": {
            "parser": None,
            "originals_parser": "parser.py",
            "ecc_module": None,
            "groups": {"audio-profile": [{"source": "audio.bin", "destination": "audio.bin"}]},
            "required_groups": ["audio-profile"],
            "preparation_recipe": "b" * 64,
            "audio_policy": {
                "speaker_vibration": False,
                "speaker_from_headphones": False,
                "headfree_headphones_only": False,
            },
        },
    }
    admitted = ValidatedBundle(tmp_path, runtime, "a" * 64, "b" * 64, None)
    reads: list[str] = []
    boots: list[dict[str, bytes]] = []

    class Phone:
        """Only model clock, progress and the mount table of a RAM reader session."""

        def run_remote(
            self, _session: dict[str, Any], command: str, **_kwargs: object
        ) -> subprocess.CompletedProcess[str]:
            if command.startswith("fplinux-clock "):
                output = "system=1700000000 rtc=written\n"
            elif command.startswith("cat /proc/mounts"):
                output = (
                    "tmpfs / tmpfs rw 0 0\n\nFPLINUX_SWAPS\nFilename\tType\tSize\tUsed\tPriority\n"
                )
            else:
                assert command.startswith("/usr/libexec/fplinux/preparation ")
                output = ""
            return subprocess.CompletedProcess(command, 0, stdout=output, stderr="")

    phone = Phone()

    def start(
        _admitted: ValidatedBundle,
        _emit: Callable[[str], None],
        *,
        device_data: Path,
        **_kwargs: object,
    ) -> tuple[Phone, dict[str, Any]]:
        boots.append(capsule_files(device_data.read_bytes()))
        return phone, {}

    def read(
        _transport: object, _session: object, device: str, _selections: object
    ) -> dict[str, bytes]:
        reads.append(device)
        return {"audio.conf": b"source settings"}

    monkeypatch.setattr(boot, "run_bundle", start)
    monkeypatch.setattr(boot, "reconnect_live_bundle", lambda *_args, **_kwargs: None)
    monkeypatch.setattr(emmc_files, "read_originals", read)
    logs = tmp_path / "logs"
    logs.mkdir()
    log = RunLog(logs, protect=lambda _path: None)
    try:
        used_reader = workflow._prepare_phone_data(  # noqa: SLF001 -- actual orchestration boundary.
            admitted,
            data,
            log,
            lambda _stage: None,
            store=store,
            from_dump=None,
            replace=False,
            loader_trace=False,
        )
        assert used_reader
        assert reads == ["/dev/mmcblk1"]
        assert boots[0]["etc/fplinux/preparation"] == b"1\n"
        assert (
            store / "phone/groups/audio-profile/audio.bin"
        ).read_bytes() == b"encoded:source settings"
        assert not (data / "reader").exists()
        destination = store / "phone"
        receipt = destination / "receipt.json"
        initial_receipt = receipt.read_bytes()
        initial_inode = destination.stat().st_ino

        assert not workflow._prepare_phone_data(  # noqa: SLF001 -- unchanged preparation reuses the publication.
            admitted,
            data,
            log,
            lambda _stage: None,
            store=store,
            from_dump=None,
            replace=False,
            loader_trace=False,
        )
        assert destination.stat().st_ino == initial_inode
        assert receipt.read_bytes() == initial_receipt
        assert (destination / "groups/audio-profile/audio.bin").read_bytes() == (
            b"encoded:source settings"
        )

        shutil.copyfile(
            ROOT / "tests/small/runtime/fixtures/emmc-preparation/refreshed_audio_parser.py",
            tmp_path / "refreshed_parser.py",
        )
        runtime["device_data"]["originals_parser"] = "refreshed_parser.py"
        runtime["device_data"]["preparation_recipe"] = "c" * 64
        assert not workflow._prepare_phone_data(  # noqa: SLF001 -- changed preparation retains the acquisition.
            admitted,
            data,
            log,
            lambda _stage: None,
            store=store,
            from_dump=None,
            replace=False,
            loader_trace=False,
        )
        assert (destination / "groups/audio-profile/audio.bin").read_bytes() == (
            b"refreshed:source settings"
        )
        refreshed_receipt = json.loads(receipt.read_bytes())
        previous_receipt = json.loads(initial_receipt)
        assert refreshed_receipt["preparation_recipe"] != previous_receipt["preparation_recipe"]
        assert refreshed_receipt["source"] == previous_receipt["source"]
        assert refreshed_receipt["groups"][0]["prepared"] == [
            {
                "source": "audio.bin",
                "destination": "audio.bin",
                "size": 25,
                "sha256": hashlib.sha256(b"refreshed:source settings").hexdigest(),
            }
        ]
        assert reads == ["/dev/mmcblk1"]
        assert len(boots) == 1
        assert (store / "originals/audio.conf").read_bytes() == b"source settings"
        assert not (data / "reader").exists()
    finally:
        log.close("success")
