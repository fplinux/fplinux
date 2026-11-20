# SPDX-License-Identifier: GPL-2.0-only
"""Exercise selection and assembly through the delivered archive's Python entry point."""

from __future__ import annotations

import hashlib
import json
import subprocess
import sys
from pathlib import Path
from typing import Any

import pytest

from tests.process import python_environment

_PEER = Path(__file__).with_name("fixtures") / "release-catalog/archive_peer.py"


def _digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _write_object(archive: Path, data: bytes) -> str:
    member = f"objects/{_digest(data)}"
    (archive / member).write_bytes(data)
    return member


@pytest.fixture
def archive(tmp_path: Path) -> Path:
    """Create two fixed model choices which share an image segment and host script."""
    archive = tmp_path / "Extracted release"
    (archive / "objects").mkdir(parents=True)
    (archive / "tools").mkdir()
    (archive / "tools/phone-helper").write_bytes(b"helper")
    suffix = _write_object(archive, b" shared")
    targets = {}
    for target, name, image in (("jade", "Jade Phone", b"Jade"), ("opal", "Opal Phone", b"Opal")):
        runtime = json.dumps({"release_id": "release", "target": target}).encode()
        contents = {
            "ramboot.bin": image + b" shared",
            "runtime-manifest.json": runtime,
            "tools/phone-helper": b"helper",
        }
        if target == "jade":
            contents["card/layout.json"] = b"Selected card layout"
        records = {
            relative: {
                "size": len(data),
                "sha256": _digest(data),
                "mode": 493 if relative == "tools/phone-helper" else 420,
            }
            for relative, data in contents.items()
        }
        build = json.dumps({"release_id": "release", "target": target, "files": records}).encode()
        targets[target] = {
            "name": name,
            "files": {
                "ramboot.bin": [_write_object(archive, image), suffix],
                "runtime-manifest.json": [_write_object(archive, runtime)],
                "tools/phone-helper": ["tools/phone-helper"],
                "build-manifest.json": [_write_object(archive, build)],
                "SHA256SUMS": [_write_object(archive, b"original checksum list\n")],
                **(
                    {"card/layout.json": [_write_object(archive, contents["card/layout.json"])]}
                    if target == "jade"
                    else {}
                ),
            },
            "executables": ["tools/phone-helper"],
        }
    stage0 = b"Jade microSD Stage0"
    runtime = b'{"release_id":"release","target":"jade","profile":"microsd-uboot"}'
    sd_contents = {"ramboot.bin": stage0, "runtime-manifest.json": runtime}
    sd_build = json.dumps(
        {
            "target": "jade",
            "release_id": "release",
            "profile": "microsd-uboot",
            "files": {
                name: {"size": len(contents), "sha256": _digest(contents), "mode": 420}
                for name, contents in sd_contents.items()
            },
        }
    ).encode()
    targets["jade"]["microsd"] = {
        "files": {
            "ramboot.bin": [_write_object(archive, stage0)],
            "runtime-manifest.json": [_write_object(archive, runtime)],
            "build-manifest.json": [_write_object(archive, sd_build)],
            "SHA256SUMS": [_write_object(archive, b"original SD checksums\n")],
        },
        "executables": [],
    }
    (archive / "targets.json").write_text(
        json.dumps({"release_id": "release", "targets": targets}), encoding="utf-8"
    )
    return archive


def _launch(
    archive: Path, *argv: str, terminal: str = "", action_status: int = 23
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, "-B", str(_PEER), str(archive), *argv],
        cwd=archive.parent,
        env={
            **python_environment(),
            "FPLINUX_DATA_DIR": "runtime data",
            "FPLINUX_TEST_ACTION_STATUS": str(action_status),
        },
        input=terminal,
        capture_output=True,
        check=False,
        text=True,
        timeout=15,
    )


def _record(result: subprocess.CompletedProcess[str]) -> dict[str, Any]:
    marker = "RUNTIME ACTION "
    line = next(line for line in result.stdout.splitlines() if marker in line)
    action: dict[str, Any] = json.loads(line.partition(marker)[2])
    return action


def test_menu_selects_one_model_and_uses_its_exact_bundle_and_runtime_status(
    archive: Path,
) -> None:
    result = _launch(archive, terminal="unknown model\n2\n")

    assert result.returncode == 23, (result.stdout, result.stderr)
    assert "1. Jade Phone (jade)" in result.stdout
    assert "2. Opal Phone (opal)" in result.stdout
    assert "Enter a model number or its target ID." in result.stdout
    action = _record(result)
    assert action["target"] == "opal"
    bundle = Path(action["bundle"])
    managed = archive / "runtime data"
    assert bundle.is_relative_to(managed)
    assert list(bundle.parent.iterdir()) == [bundle]
    assert (bundle / "ramboot.bin").read_bytes() == b"Opal shared"
    assert (bundle / "SHA256SUMS").read_bytes() == b"original checksum list\n"
    assert (bundle / "tools/phone-helper").stat().st_mode & 0o111 == 0o111
    data = Path(action["data"])
    assert data.is_relative_to(managed)
    assert data.name == "opal"
    assert action["logs"] == str(data / "logs")

    next_result = _launch(archive, "verify", "--target", "opal")

    assert next_result.returncode == 23, (next_result.stdout, next_result.stderr)
    next_action = _record(next_result)
    assert next_action["verify_only"] is True
    assert next_action["data"] == action["data"]
    assert next_action["bundle"] == action["bundle"]


def test_explicit_console_uses_cwd_relative_runtime_overrides_without_reading_menu(
    archive: Path,
) -> None:
    result = _launch(
        archive,
        "console",
        "--target=jade",
        "--exec=printf 'x --target opal y'",
        "--allow-other-build",
        "--data-root",
        "alternate state",
        "--log-root",
        "alternate logs",
    )

    assert result.returncode == 23, (result.stdout, result.stderr)
    assert "Phone model:" not in result.stdout
    action = _record(result)
    assert action["target"] == "jade"
    assert action["data"] == str(archive.parent / "alternate state")
    assert action["logs"] == str(archive.parent / "alternate logs")
    assert action["exec_command"] == "printf 'x --target opal y'"
    assert action["allow_other_build"] is True


def test_cached_bundle_rejects_changed_shared_host_file(archive: Path) -> None:
    assert _launch(archive, "run", "--target", "jade").returncode == 23
    (archive / "tools/phone-helper").write_bytes(b"changed extracted helper")

    result = _launch(archive, "run", "--target", "jade")

    assert result.returncode != 23
    assert "tools/phone-helper SHA256 differs" in result.stderr
    assert "RUNTIME ACTION" not in result.stdout


def test_supported_model_menu_selects_card_writer_with_interactive_consent(archive: Path) -> None:
    result = _launch(archive, terminal="1\n2\n")

    assert result.returncode == 23, (result.stdout, result.stderr)
    assert "Install on microSD" in result.stdout
    action = _record(result)
    assert action["target"] == "jade"
    assert action["inspect_only"] is False
    assert action["erase_card"] is None
    assert "confirm_card_erase" in action["confirm"]


def test_only_successful_menu_install_prompts_to_close(archive: Path) -> None:
    menu = _launch(archive, terminal="1\n2\n\n", action_status=0)
    command = _launch(
        archive, "card", "install", "--target", "jade", "--erase-card", "a" * 32, action_status=0
    )

    assert menu.returncode == command.returncode == 0
    assert "Press Enter to close the launcher." in menu.stdout
    assert "Press Enter" not in command.stdout


def test_card_start_and_verify_select_stage0_and_leave_default_ram_workflow(archive: Path) -> None:
    menu = _launch(archive, terminal="1\n3\n")
    boot = _launch(archive, "run", "--target", "jade", "--boot", "microsd")
    console = _launch(archive, "console", "--target", "jade", "--boot", "microsd")
    verify = _launch(archive, "verify", "--target", "jade", "--boot", "microsd")
    ram = _launch(archive, "run", "--target", "jade")

    for result in (menu, boot, console, verify, ram):
        assert result.returncode == 23, (result.stdout, result.stderr)
    assert "Start from microSD" in menu.stdout
    action = _record(menu)
    assert action == _record(boot)
    assert action["profile"] == "microsd-uboot"
    assert _record(console)["bundle"] == action["bundle"]
    assert _record(verify)["bundle"] == action["bundle"]
    assert _record(verify)["verify_only"] is True
    bundle = Path(action["bundle"])
    assert (bundle / "ramboot.bin").read_bytes() == b"Jade microSD Stage0"
    assert not (bundle / "card").exists()
    assert _record(ram)["bundle"] != action["bundle"]
    assert _record(ram)["profile"] is None
    assert _record(ram)["data"] == action["data"]


def test_card_start_rejects_ram_build_recipe_for_the_same_phone(archive: Path) -> None:
    path = archive / "targets.json"
    catalog = json.loads(path.read_bytes())
    board = catalog["targets"]["jade"]
    board["microsd"] = {"files": board["files"], "executables": board["executables"]}
    path.write_text(json.dumps(catalog), encoding="utf-8")

    result = _launch(archive, "run", "--target", "jade", "--boot", "microsd")

    assert result.returncode == 1
    assert "phone model jade and boot profile" in result.stderr
    assert not (archive / "runtime data").exists()


def test_explicit_card_commands_require_a_supported_model_and_install_consent(
    archive: Path,
) -> None:
    missing = _launch(archive, "card", "install", "--target", "jade")
    unsupported = _launch(archive, "card", "inspect", "--target", "opal")

    assert missing.returncode == 2
    assert "requires --erase-card CID" in missing.stderr
    assert unsupported.returncode == 2
    assert "invalid choice" in unsupported.stderr
    assert not (archive / "runtime data").exists()

    inspected = _launch(archive, "card", "inspect", "--target", "jade")
    assert inspected.returncode == 23
    assert f"SD card {'c' * 32} (6 MiB) at /dev/mmcblk0" in inspected.stdout
    installed = _launch(archive, "card", "install", "--target", "jade", "--erase-card", "a" * 32)
    assert installed.returncode == 23
    assert _record(installed)["erase_card"] == "a" * 32
    assert _record(installed)["confirm"] is None


def test_card_inspect_materializes_ram_without_the_system_card_objects(archive: Path) -> None:
    catalog = json.loads((archive / "targets.json").read_bytes())
    files = catalog["targets"]["jade"]["files"]
    card_objects = {}
    for relative, parts in files.items():
        if relative.startswith("card/"):
            for part in parts:
                path = archive / part
                card_objects[path] = path.read_bytes()
                path.unlink()

    inspected = _launch(archive, "card", "inspect", "--target", "jade")

    assert inspected.returncode == 23, (inspected.stdout, inspected.stderr)
    assert f"SD card {'c' * 32} (6 MiB) at /dev/mmcblk0" in inspected.stdout
    action = _record(inspected)
    bundle = Path(action["bundle"])
    assert action["target"] == "jade"
    assert action["profile"] is None
    assert (bundle / "ramboot.bin").read_bytes() == b"Jade shared"
    manifest = (archive / files["build-manifest.json"][0]).read_bytes()
    assert (bundle / "build-manifest.json").read_bytes() == manifest
    assert not (bundle / "card").exists()

    installed = _launch(archive, "card", "install", "--target", "jade", "--erase-card", "a" * 32)

    assert installed.returncode == 1
    assert "the extracted release is missing archive member" in installed.stderr
    assert "RUNTIME ACTION" not in installed.stdout

    for path, contents in card_objects.items():
        path.write_bytes(contents)
    retried = _launch(archive, "card", "install", "--target", "jade", "--erase-card", "a" * 32)

    assert retried.returncode == 23, (retried.stdout, retried.stderr)
    install_action = _record(retried)
    assert install_action["data"] == action["data"]
    assert install_action["logs"] == action["logs"]
    install_bundle = Path(install_action["bundle"])
    assert (install_bundle / "build-manifest.json").read_bytes() == manifest
    assert (install_bundle / "card/layout.json").read_bytes() == b"Selected card layout"


def test_prepare_passes_phone_data_and_dump_options_to_the_selected_model(archive: Path) -> None:
    result = _launch(
        archive,
        "prepare",
        "--target",
        "opal",
        "--phone-data",
        "phone originals",
        "--from-dump",
        "backup.bin",
        "--events",
        "events.jsonl",
        "--replace",
        "--loader-trace",
    )

    assert result.returncode == 23, (result.stdout, result.stderr)
    action = _record(result)
    assert action["target"] == "opal"
    assert action["prepare_only"] is True
    assert action["replace"] is True
    assert action["loader_trace"] is True
    assert action["phone_data"] == str(archive.parent / "phone originals")
    assert action["from_dump"] == str(archive.parent / "backup.bin")
    assert action["events"] == "events.jsonl"


@pytest.mark.parametrize(
    ("arguments", "terminal", "status", "output"),
    [
        (("--help",), "", 0, "connect to its USB session"),
        (("nand", "identify", "--help"), "", 0, "--target MODEL"),
        (("run", "--target", "other"), "", 2, "invalid choice: 'other'"),
        (
            ("run", "--target", "opal", "--boot", "microsd"),
            "",
            2,
            "opal has no system microSD boot",
        ),
        (
            ("run", "--target", "jade", "--boot", "microsd", "--from-dump", "backup.bin"),
            "",
            2,
            "uses the phone data already on the card",
        ),
        (("prepare",), "", 2, "--target"),
        ((), "0\n", 0, "0. Exit"),
        ((), "", 2, "No model selected"),
    ],
)
def test_help_invalid_model_and_cancel_leave_managed_data_untouched(
    archive: Path, arguments: tuple[str, ...], terminal: str, status: int, output: str
) -> None:
    result = _launch(archive, *arguments, terminal=terminal)

    assert result.returncode == status, (result.stdout, result.stderr)
    assert output in result.stdout + result.stderr
    assert "RUNTIME ACTION" not in result.stdout
    assert not (archive / "runtime data").exists()


def test_wrong_archive_bytes_fail_before_dispatch_and_restored_input_can_retry(
    archive: Path,
) -> None:
    broken_part = archive / "objects" / _digest(b" shared")
    broken_part.write_bytes(b" wrong!")

    result = _launch(archive, "run", "--target", "opal")

    assert result.returncode == 1, (result.stdout, result.stderr)
    assert "ramboot.bin SHA256 or size differs from the build manifest" in result.stderr
    assert "RUNTIME ACTION" not in result.stdout
    assert not list((archive / "runtime data/runtime/archive-bundles").iterdir())

    broken_part.write_bytes(b" shared")
    retry = _launch(archive, "run", "--target", "opal")

    assert retry.returncode == 23, (retry.stdout, retry.stderr)
    assert (Path(_record(retry)["bundle"]) / "ramboot.bin").read_bytes() == b"Opal shared"


def test_windows_stream_name_is_rejected_before_materializing_archive_members(
    archive: Path,
) -> None:
    path = archive / "targets.json"
    catalog = json.loads(path.read_bytes())
    catalog["targets"]["opal"]["files"]["tools/phone-helper"] = ["tools/phone-helper:other"]
    path.write_text(json.dumps(catalog), encoding="utf-8")

    result = _launch(archive, "run", "--target", "opal")

    assert result.returncode == 1, (result.stdout, result.stderr)
    assert "must be a portable relative path: tools/phone-helper:other" in result.stderr
    assert not (archive / "runtime data").exists()
