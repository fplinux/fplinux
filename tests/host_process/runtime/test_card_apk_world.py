# SPDX-License-Identifier: GPL-2.0-only
"""Restore staged card packages with the native APK solver and independent APKs."""

from __future__ import annotations

import hashlib
import shutil
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING

import pytest

from tests.apk_support import ApkIdentity, signed_apk_tree
from tests.process import run_process

if TYPE_CHECKING:
    from subprocess import CompletedProcess

RESTORE = Path(__file__).resolve().parents[3] / "scripts/fplinux_cli/runtime/card-apk-world.sh"
pytestmark = pytest.mark.skipif(
    not all(shutil.which(command) for command in ("apk", "openssl")),
    reason="native apk and openssl are required for the staged APK transaction",
)


def _run(*arguments: str | Path, check: bool = True) -> CompletedProcess[str]:
    return run_process(
        [str(value) for value in arguments],
        name=str(arguments[0]),
        timeout=15,
        check=check,
    )


def _apk(root: Path, *arguments: str | Path) -> CompletedProcess[str]:
    # The test roots are temporary and APK intentionally sees an in-memory filesystem.
    return _run(
        "apk",
        "--root",
        root,
        "--no-network",
        "--no-cache",
        "--allow-untrusted",
        "--force-non-repository",
        "--repositories-file",
        "/dev/null",
        *arguments,
    )


def _world(root: Path) -> Path:
    return root / "etc/apk/world"


def _installed(root: Path) -> Path:
    return root / "lib/apk/db/installed"


def _index(directory: Path, key: Path) -> Path:
    index = directory / "Packages.adb"
    _run(
        "apk",
        "--allow-untrusted",
        "mkndx",
        "--compression",
        "deflate",
        "--sign-key",
        key,
        "--pkgname-spec",
        "${name}.apk",
        "--output",
        index,
        *sorted(directory.glob("*.apk")),
    )
    return index


@dataclass(frozen=True)
class CardPackages:
    """Own two isolated roots, two releases and an unrelated local APK."""

    active: Path
    staged: Path
    admitted: Path
    previous: Path
    foreign: Path
    release_key: Path

    def select(self, action: str, *, root: Path | None = None) -> CompletedProcess[str]:
        """Run the deployed entry point against the active and staged slots."""
        return _run(
            "sh", RESTORE, action, self.active, root or self.staged, self.admitted, check=False
        )


@pytest.fixture
def card_packages(tmp_path: Path) -> CardPackages:
    key = tmp_path / "fixture.rsa"
    release_key = tmp_path / "release.rsa"
    _run("openssl", "genrsa", "-out", key, "2048")
    _run("openssl", "genrsa", "-out", release_key, "2048")
    active = tmp_path / "system/a"
    staged = tmp_path / "system/.update/pair/root"
    admitted = tmp_path / "system/.update/apks"
    previous = tmp_path / "previous"
    payload = tmp_path / "payload"
    for directory in (admitted, previous, payload):
        directory.mkdir(parents=True)
    _run(
        "openssl", "rsa", "-in", release_key, "-pubout", "-out", admitted / "fplinux-build.rsa.pub"
    )

    def package(directory: Path, identity: ApkIdentity, *, flat: bool = True) -> Path:
        share = payload / f"{identity.name}-{identity.version}" / "usr/share/fixture"
        share.mkdir(parents=True, exist_ok=True)
        (share / identity.name).write_text(identity.version)
        result = signed_apk_tree(directory, identity, share.parents[2], private_key=key)
        return result.rename(directory / f"{identity.name}.apk") if flat else result

    for directory, version, root in (
        (previous, "1.0-r0", active / "root"),
        (admitted, "2.0-r0", staged),
    ):
        base = package(
            tmp_path,
            ApkIdentity("fixture-base", "armv7", version),
            flat=False,
        )
        (root / "proc/self").mkdir(parents=True)
        (root / "dev").mkdir()
        (root / "dev/null").touch()
        keys = root / "etc/apk/keys"
        keys.mkdir(parents=True)
        _run("openssl", "rsa", "-in", key, "-pubout", "-out", keys / "sysroot-test.rsa.pub")
        _apk(root, "--arch", "armv7", "add", "--initdb", base)
        shutil.copyfile(_world(root), root.parent / "base-world")
        for name, dependencies in (
            ("fixture-app", (f"fixture-runtime={version}",)),
            ("fixture-runtime", (f"fixture-base={version}",)),
            ("fixture-removed", ()),
            ("fixture-unused", ()),
        ):
            package(directory, ApkIdentity(name, "armv7", version, depends=dependencies))

    # A package in the new ZIP may provide a missing name or version.
    package(
        admitted,
        ApkIdentity(
            "fixture-alias", "armv7", provides=("fixture-foreign=1.0-r0", "fixture-app=1.0-r0")
        ),
    )
    foreign = package(tmp_path, ApkIdentity("fixture-foreign", "armv7"))
    _index(previous, release_key)
    _index(admitted, release_key)
    return CardPackages(active, staged, admitted, previous, foreign, release_key)


@pytest.mark.parametrize("selection", [None, "fixture-app>=1.0-r0"], ids=["local-apk", "range"])
def test_restore_selection_dependencies_and_removal(
    card_packages: CardPackages, selection: str | None
) -> None:
    active_root = card_packages.active / "root"
    _apk(
        active_root,
        "--repository",
        card_packages.previous / "Packages.adb",
        "add",
        selection or card_packages.previous / "fixture-app.apk",
        card_packages.previous / "fixture-removed.apk",
    )
    _apk(active_root, "del", "fixture-removed")
    active_world = _world(active_root).read_bytes()
    active_installed = _installed(active_root).read_bytes()
    if selection is None:
        assert b"fixture-app><" in active_world
    else:
        assert selection.encode() in active_world
    assert b"fixture-runtime" not in active_world
    assert b"fixture-removed" not in active_world

    restored = card_packages.select("restore")

    assert restored.returncode == 0, restored.stdout + restored.stderr
    assert _world(active_root).read_bytes() == active_world
    assert _installed(active_root).read_bytes() == active_installed
    assert set(_world(card_packages.staged).read_text().splitlines()) == {
        (card_packages.staged.parent / "base-world").read_text().strip(),
        selection or "fixture-app",
    }
    assert sorted(path.name for path in (card_packages.staged / "etc/apk/keys").iterdir()) == [
        "sysroot-test.rsa.pub"
    ]
    staged_payload = card_packages.staged / "usr/share/fixture"
    assert {entry.name: entry.read_text() for entry in staged_payload.iterdir()} == {
        "fixture-base": "2.0-r0",
        "fixture-runtime": "2.0-r0",
        "fixture-app": "2.0-r0",
    }


@pytest.mark.parametrize("conflicting", [False, True], ids=["compatible", "conflicting-base"])
def test_preflight_resolves_local_apk_on_read_only_database(
    card_packages: CardPackages, tmp_path: Path, *, conflicting: bool
) -> None:
    active = card_packages.active / "root"
    _apk(
        active,
        "--repository",
        card_packages.previous / "Packages.adb",
        "add",
        card_packages.previous / "fixture-app.apk",
    )
    assert b"fixture-app><" in _world(active).read_bytes()
    if conflicting:
        payload = tmp_path / "conflicting-payload"
        payload.mkdir()
        archive = signed_apk_tree(
            tmp_path,
            ApkIdentity("fixture-app", "armv7", "2.0-r0", depends=("!fixture-base",)),
            payload,
            private_key=tmp_path / "fixture.rsa",
        )
        archive.replace(card_packages.admitted / "fixture-app.apk")
        _index(card_packages.admitted, card_packages.release_key)

    preflight = tmp_path / "preflight-root"
    files = {
        "etc/apk/arch",
        "etc/apk/world",
        "etc/apk/keys/sysroot-test.rsa.pub",
        "lib/apk/db/installed",
    }
    for name in files:
        destination = preflight / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(card_packages.staged / name, destination)
    active_state = (_world(active).read_bytes(), _installed(active).read_bytes())
    preflight_state = {name: (preflight / name).read_bytes() for name in files}

    result = card_packages.select("preflight", root=preflight)

    if conflicting:
        assert result.returncode == 1, result.stdout + result.stderr
        assert "cannot resolve APK selection: fixture-app" in result.stderr
        assert "fixture-base" in result.stderr
    else:
        assert result.returncode == 0, result.stdout + result.stderr
    assert (_world(active).read_bytes(), _installed(active).read_bytes()) == active_state
    assert {name: (preflight / name).read_bytes() for name in files} == preflight_state
    actual_files = {
        path.relative_to(preflight).as_posix() for path in preflight.rglob("*") if path.is_file()
    }
    assert actual_files == files


@pytest.mark.parametrize("action", ["preflight", "restore"])
@pytest.mark.parametrize(
    ("scenario", "rejected"),
    [
        ("third-party", "fixture-foreign"),
        ("pinned-version", "fixture-app=1.0-r0"),
        ("excluded-dependency", "fixture-runtime"),
        ("wrong-signing-key", "cannot verify the release APK index"),
    ],
)
def test_restore_rejects_unavailable_selections_without_installing(
    card_packages: CardPackages, scenario: str, rejected: str, action: str
) -> None:
    active_root = card_packages.active / "root"
    if scenario == "third-party":
        _apk(active_root, "add", card_packages.foreign)
    elif scenario == "pinned-version":
        _apk(
            active_root,
            "--repository",
            card_packages.previous / "Packages.adb",
            "add",
            "fixture-app=1.0-r0",
        )
    else:
        _apk(
            active_root,
            "--repository",
            card_packages.previous / "Packages.adb",
            "add",
            card_packages.previous / "fixture-app.apk",
        )
    if scenario == "excluded-dependency":
        excluded = card_packages.previous.parent / "excluded"
        excluded.mkdir()
        (card_packages.admitted / "fixture-runtime.apk").rename(excluded / "fixture-runtime.apk")
        excluded_index = _index(excluded, card_packages.release_key)
        _index(card_packages.admitted, card_packages.release_key)
        extra_config = card_packages.staged / "etc/apk/repositories.d"
        extra_config.mkdir()
        (extra_config / "external.list").write_text(f"{excluded_index}\n")
    if scenario == "wrong-signing-key":
        shutil.copyfile(
            card_packages.staged / "etc/apk/keys/sysroot-test.rsa.pub",
            card_packages.admitted / "fplinux-build.rsa.pub",
        )
    active_world = _world(active_root).read_bytes()
    active_installed = _installed(active_root).read_bytes()
    staged_world = _world(card_packages.staged).read_bytes()
    staged_installed = _installed(card_packages.staged).read_bytes()

    restored = card_packages.select(action)

    assert restored.returncode != 0
    assert rejected in restored.stderr
    assert _world(active_root).read_bytes() == active_world
    assert _installed(active_root).read_bytes() == active_installed
    assert _world(card_packages.staged).read_bytes() == staged_world
    assert _installed(card_packages.staged).read_bytes() == staged_installed


@pytest.mark.skipif(shutil.which("busybox") is None, reason="the update script needs BusyBox")
@pytest.mark.parametrize("board_intact", [True, False], ids=["intact-board", "modified-board"])
def test_publish_verifies_the_board_before_restoring_the_selected_apk(
    card_packages: CardPackages, tmp_path: Path, *, board_intact: bool
) -> None:
    target = "nokia-ta1618"
    old_build = "111111111111"
    new_build = "222222222222"
    active = card_packages.active
    staged = card_packages.staged
    for pair, build in ((active, old_build), (staged.parent, new_build)):
        (pair / "root/etc/os-release").write_text(f"BUILD_ID={build}\n", encoding="ascii")
        (pair / "FPLINUX.ITB").write_bytes(f"FIT for {build}\n".encode())
    (tmp_path / "system/target").write_text(f"{target}\n", encoding="ascii")
    selector = tmp_path / "system/active"
    selector.write_bytes(b"a\n")
    _run("sh", RESTORE.with_name("card-slots.sh"), tmp_path, target, "confirm", old_build)
    _apk(
        active / "root",
        "--repository",
        card_packages.previous / "Packages.adb",
        "add",
        card_packages.previous / "fixture-app.apk",
    )
    original_world = _world(active / "root").read_bytes()
    original_database = _installed(active / "root").read_bytes()
    board_file = staged / "etc/board.conf"
    board_file.write_bytes(b"brightness=8\n")
    staged_world = _world(staged).read_bytes()
    staged_database = _installed(staged).read_bytes()
    board_records = "".join(
        f"{hashlib.sha256(path.read_bytes()).hexdigest()}  ./{path.relative_to(staged)}\n"
        for path in (_world(staged), _installed(staged), board_file)
    ).encode("ascii")
    (staged.parent.parent / "board.sha256").write_bytes(board_records)
    if not board_intact:
        board_file.write_bytes(b"brightness=4\n")

    published = _run(
        "sh",
        RESTORE.with_name("update-card.sh"),
        "publish",
        tmp_path,
        target,
        new_build,
        hashlib.sha256(b"FIT for 222222222222\n").hexdigest(),
        hashlib.sha256(board_records).hexdigest(),
        check=False,
    )

    assert _world(active / "root").read_bytes() == original_world
    assert _installed(active / "root").read_bytes() == original_database
    selected = tmp_path / "system/b"
    if not board_intact:
        assert published.returncode == 1
        assert "prepared board files failed verification" in published.stderr
        assert selector.read_bytes() == b"a\n"
        assert not selected.exists()
        assert _world(staged).read_bytes() == staged_world
        assert _installed(staged).read_bytes() == staged_database
        return
    assert published.returncode == 0, published.stderr
    assert selector.read_bytes() == b"b\n"
    assert (selected / "root/etc/board.conf").read_bytes() == b"brightness=8\n"
    assert (selected / "base-world").read_bytes() == staged_world
    assert b"fixture-app\n" in _world(selected / "root").read_bytes()
    assert (selected / "root/usr/share/fixture/fixture-app").read_text() == "2.0-r0"
