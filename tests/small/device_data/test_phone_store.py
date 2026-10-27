# SPDX-License-Identifier: GPL-2.0-only
"""Persistent phone paths and their shared preparation lock."""

from __future__ import annotations

from typing import TYPE_CHECKING, Any

import pytest
from fplinux_cli.device_data import inputs, store
from fplinux_cli.device_data.formats import AudioProfileContext
from fplinux_cli.device_data.original_files import save_original_files
from fplinux_cli.device_data.prepare import DeviceDataSpec, DeviceDataTools, prepare_from_originals
from fplinux_cli.runtime import boot, workflow
from fplinux_cli.runtime.bundle import ValidatedBundle

from tests import ROOT

if TYPE_CHECKING:
    from pathlib import Path


@pytest.fixture
def isolated_user_paths(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Path:
    """Keep every supported data-home environment variable inside one owned tree."""
    monkeypatch.setenv("HOME", str(tmp_path / "home"))
    monkeypatch.setenv("XDG_DATA_HOME", str(tmp_path / "data"))
    monkeypatch.setenv("LOCALAPPDATA", str(tmp_path / "local"))
    return tmp_path


@pytest.mark.parametrize("platform", ["linux", "win32"])
def test_store_path_uses_the_host_user_directory_independently_of_current_directory(
    isolated_user_paths: Path, monkeypatch: pytest.MonkeyPatch, platform: str
) -> None:
    """Different source and release directories select one stable per-target store."""
    monkeypatch.setattr("sys.platform", platform)
    first = store.phone_store_directory("demo-phone")
    checkout = isolated_user_paths / "checkout"
    checkout.mkdir()
    monkeypatch.chdir(checkout)
    second = store.phone_store_directory("demo-phone")

    expected = isolated_user_paths / (
        "local/FPLinux/phones/demo-phone"
        if platform == "win32"
        else "data/fplinux/phones/demo-phone"
    )
    assert first == second == expected
    assert not expected.exists()


def test_linux_store_defaults_to_the_home_data_directory(
    isolated_user_paths: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """An unset or empty XDG data home uses the normal persistent user-data directory."""
    monkeypatch.setattr("sys.platform", "linux")
    monkeypatch.delenv("XDG_DATA_HOME")
    assert store.phone_store_directory("phone") == (
        isolated_user_paths / "home/.local/share/fplinux/phones/phone"
    )
    monkeypatch.setenv("XDG_DATA_HOME", "")
    assert store.phone_store_directory("phone") == (
        isolated_user_paths / "home/.local/share/fplinux/phones/phone"
    )


def test_windows_store_requires_the_user_local_application_directory(
    isolated_user_paths: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """A missing Windows user-data root cannot silently select another storage location."""
    del isolated_user_paths
    monkeypatch.setattr("sys.platform", "win32")
    monkeypatch.delenv("LOCALAPPDATA")
    with pytest.raises(SystemExit, match="LOCALAPPDATA is required"):
        store.phone_store_directory("phone")


def test_explicit_directory_selects_another_phone_without_changing_the_default(
    isolated_user_paths: Path,
) -> None:
    """A same-model phone can own a separate store without changing other launches."""
    other = isolated_user_paths / "other-phone"
    assert store.phone_store_directory("phone", root=other) == other
    assert (
        store.phone_store_directory("phone") == isolated_user_paths / "data/fplinux/phones/phone"
    )
    assert not other.exists()


def test_private_store_lock_is_stable_and_released_after_an_error(
    isolated_user_paths: Path,
) -> None:
    """A failed write releases the lock without replacing or removing its inode."""
    directory = isolated_user_paths / "data/fplinux/phones/phone"

    def write_and_fail() -> None:
        with store.locked_phone_store("phone") as selected:
            assert selected == directory
            with (
                pytest.raises(SystemExit, match="data is being updated"),
                store.locked_phone_store("phone"),
            ):
                pytest.fail("a second writer must not enter")
            message = "controlled write failure"
            raise ValueError(message)

    with pytest.raises(ValueError, match="controlled write failure"):
        write_and_fail()
    before = (directory / ".store.lock").stat().st_ino
    with store.locked_phone_store("phone"):
        assert (directory / ".store.lock").stat().st_ino == before
    assert directory.stat().st_mode & 0o777 == 0o700


@pytest.mark.parametrize("explicit", [False, True], ids=["default-store", "explicit-phone"])
def test_build_capture_rejects_a_store_writer_and_then_reads_the_complete_group(
    isolated_user_paths: Path, *, explicit: bool
) -> None:
    """Build input capture shares the publication lock and returns fixed admitted bytes."""
    directory = isolated_user_paths / ("other-phone" if explicit else "data/fplinux/phones/phone")
    root = directory if explicit else None
    group = directory / "phone/groups/bluetooth"
    group.mkdir(parents=True)
    (group / "controller.bin").write_bytes(b"radio")
    groups = {
        "bluetooth": [{"source": "controller.bin", "destination": "chip/radio.bin", "size": 5}]
    }
    with (
        store.locked_phone_store("phone", root=root),
        pytest.raises(SystemExit, match="data is being updated"),
    ):
        inputs.capture_external_device_data("phone", groups, root=root)
    captured = inputs.capture_external_device_data("phone", groups, root=root)
    (group / "controller.bin").write_bytes(b"later")
    assert captured["bluetooth"][0].contents == b"radio"


def test_launcher_and_store_locks_can_have_independent_lifetimes(
    isolated_user_paths: Path,
    monkeypatch: pytest.MonkeyPatch,
    capsys: pytest.CaptureFixture[str],
) -> None:
    """The real launcher admits snapshots during its controlled authenticated shell."""
    groups: dict[str, list[dict[str, Any]]] = {
        "audio-profile": [{"source": "audio.bin", "destination": "audio.bin", "size": 23}]
    }
    audio_policy = {
        "speaker_vibration": False,
        "speaker_from_headphones": False,
        "headfree_headphones_only": False,
    }
    parser = "tests/fixtures/original-files/audio_parser.py"
    spec = DeviceDataSpec(
        target="phone",
        parser=None,
        originals_parser=ROOT / parser,
        groups=groups,
        audio=AudioProfileContext(b"test,phone", **audio_policy),
        preparation_recipe="b" * 64,
    )
    with store.locked_phone_store("phone") as directory:
        manifest = save_original_files(
            "phone",
            {"audio.conf": b"source settings"},
            directory / "originals",
            acquisition_recipe="a" * 64,
        )
        prepare_from_originals(spec, manifest, directory / "phone", DeviceDataTools())
    runtime: dict[str, Any] = {
        "target": "phone",
        "profile": None,
        "build_type": "release",
        "identity": {"target": {"display_name": "Test phone", "compatible": "test,phone"}},
        "host_tools": {},
        "nand": None,
        "emmc": {
            "acquisition_recipe": "a" * 64,
            "files": [
                {
                    "name": "audio.conf",
                    "locations": [["system", "/etc/audio.conf"]],
                    "max_bytes": 64,
                }
            ],
        },
        "device_data": {
            "parser": None,
            "originals_parser": parser,
            "ecc_module": None,
            "groups": groups,
            "required_groups": ["audio-profile"],
            "preparation_recipe": "b" * 64,
            "audio_policy": audio_policy,
        },
    }
    admitted = ValidatedBundle(ROOT, runtime, "a" * 64, "b" * 64, None)
    data = isolated_user_paths / "runtime/phone"
    logs = isolated_user_paths / "logs"
    sessions: list[dict[str, Any]] = []

    class Phone:
        """Run assertions at the interactive endpoint while real host locks are active."""

        def run_shell(self, session: dict[str, Any]) -> int:
            """Capture prepared bytes while this terminal retains its launcher lock."""
            sessions.append(session)
            assert sessions == [{"status": "ready"}]
            captured = inputs.capture_external_device_data("phone", groups)
            assert captured["audio-profile"][0].contents == b"encoded:source settings"
            assert workflow.run_phone(ROOT, data, logs, reconnect=True) == 1
            assert "Another launcher is already open" in capsys.readouterr().err
            return 0

    def connect(
        selected: ValidatedBundle,
        *,
        session_root: Path,
        prepared_data_sha256: str | None,
        required: bool = False,
    ) -> tuple[Phone, dict[str, Any]]:
        assert selected is admitted
        assert session_root == data / "session"
        assert prepared_data_sha256 is not None
        assert required
        return Phone(), {"status": "ready"}

    monkeypatch.setattr(workflow, "admit_bundle", lambda _bundle: admitted)
    monkeypatch.setattr(workflow, "host_preflight", lambda: None)
    monkeypatch.setattr(boot, "reconnect_live_bundle", connect)

    assert workflow.run_phone(ROOT, data, logs, reconnect=True) == 0
    assert sessions == [{"status": "ready"}]
