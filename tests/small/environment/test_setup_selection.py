# SPDX-License-Identifier: GPL-2.0-only
"""Host preparation of selected originals while reusing the shared environment."""

from __future__ import annotations

import base64
import hashlib
import io
import json
from pathlib import Path
from unittest import mock

import pytest
from fplinux_cli import common
from fplinux_cli.dependencies import site_install, workflow
from fplinux_cli.dependencies.inputs import DependencyInput
from fplinux_cli.environment import setup as env_setup
from fplinux_cli.environment.state import EnvironmentState
from fplinux_cli.reporting import run as reporting


def original(name: str, contents: bytes) -> DependencyInput:
    """Pin small input bytes independently of the downloader being exercised."""
    return DependencyInput(
        key=f"source:{name}",
        url=f"https://example.invalid/{name}.tar.gz",
        sha256=hashlib.sha256(contents).hexdigest(),
        size=len(contents),
        destination=f"downloads/sources/{name}/original.tar.gz",
        purpose="source-original",
    )


@pytest.fixture
def ready_checkout(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Path:
    """Replace the unavailable runtime probe with one ready, unchanged generation."""
    monkeypatch.setattr(common, "ROOT", tmp_path)
    monkeypatch.setattr(env_setup, "ROOT", tmp_path)
    monkeypatch.setattr(reporting, "ROOT", tmp_path)
    state = EnvironmentState("a" * 64, "b" * 64, "c" * 64)
    monkeypatch.setattr(env_setup, "load_environment_lock", dict)
    monkeypatch.setattr(env_setup, "install_kern", lambda *_args, **_kwargs: "kern")
    monkeypatch.setattr(env_setup, "environment_recipe_digest", lambda *_args: "a" * 64)
    monkeypatch.setattr(env_setup, "current_environment_state", lambda *_args: state)
    monkeypatch.setattr(env_setup, "install_git_hooks", lambda: None)

    def unexpected_rebuild(*_args: object, **_kwargs: object) -> None:
        pytest.fail("a ready shared environment must not be rebuilt for selected inputs")

    monkeypatch.setattr(env_setup, "_prepare_rootfs", unexpected_rebuild)
    marker = tmp_path / ".cache/environment/active-generation"
    marker.parent.mkdir(parents=True)
    marker.write_bytes(b"shared generation\n")
    return tmp_path


def test_repeated_target_setup_acquires_updated_and_additional_inputs_without_rebuild(
    ready_checkout: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """Ready environment reuse still prepares new target, host and changed original bytes."""
    selected = [original("first", b"first bytes\n")]
    monkeypatch.setattr(env_setup, "target_setup_inputs", lambda *_args, **_kwargs: selected)

    for target, host, item, contents in (
        ("first-phone", "linux", selected[0], b"first bytes\n"),
        ("first-phone", "linux", original("first", b"updated bytes\n"), b"updated bytes\n"),
        ("first-phone", "windows", original("windows", b"Windows bytes\n"), b"Windows bytes\n"),
        ("second-phone", "linux", original("second", b"second bytes\n"), b"second bytes\n"),
    ):
        selected[:] = [item]
        with mock.patch("urllib.request.urlopen", return_value=io.BytesIO(contents)):
            env_setup.prepare_setup(target, host=host)
        assert (ready_checkout / ".cache" / item.destination).read_bytes() == contents

    assert (ready_checkout / ".cache/environment/active-generation").read_bytes() == (
        b"shared generation\n"
    )
    assert (ready_checkout / ".cache/downloads/sources/first/original.tar.gz").read_bytes() == (
        b"updated bytes\n"
    )
    runs = list((ready_checkout / ".cache/logs/setup").glob("*/*/run.json"))
    assert len(runs) == 4
    assert all(json.loads(path.read_text())["status"] == "success" for path in runs)


def test_offline_target_setup_names_its_missing_original_and_preparation_command(
    ready_checkout: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """A local-only setup failure identifies the selected host without attempting a fetch."""
    item = original("missing", b"expected bytes\n")
    monkeypatch.setattr(env_setup, "target_setup_inputs", lambda *_args, **_kwargs: [item])

    with (
        mock.patch("urllib.request.urlopen", side_effect=AssertionError("network")),
        pytest.raises(
            SystemExit,
            match=r"missing.tar.gz.*run ./fplinux setup first-phone --host windows",
        ),
    ):
        env_setup.prepare_setup("first-phone", host="windows", offline=True)

    assert not (ready_checkout / ".cache" / item.destination).exists()
    assert (ready_checkout / ".cache/environment/active-generation").read_bytes() == (
        b"shared generation\n"
    )


@pytest.fixture
def site_checkout(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Path:
    """Declare tiny exact site originals without any Alpine or target declarations."""
    monkeypatch.setattr(common, "ROOT", tmp_path)
    monkeypatch.setattr(env_setup, "ROOT", tmp_path)
    monkeypatch.setattr(reporting, "ROOT", tmp_path)
    fixture = (
        Path(__file__).parents[1] / "dependencies/fixtures/site-inputs/sources.lock.toml.fixture"
    )
    (tmp_path / "sources.lock.toml").write_bytes(fixture.read_bytes())
    (tmp_path / "package.json").write_text('{"engines":{"node":"24.21.0","npm":"12.2.0"}}\n')
    (tmp_path / "site").mkdir()
    (tmp_path / "site/package-lock.json").write_text(
        json.dumps(
            {
                "packages": {
                    "": {"name": "documentation"},
                    "node_modules/example": {
                        "version": "1.0.0",
                        "resolved": "https://example.invalid/example.tgz",
                        "integrity": "sha512-"
                        + base64.b64encode(hashlib.sha512(b"abc").digest()).decode(),
                    },
                }
            }
        )
    )
    return tmp_path


def test_site_setup_acquires_exact_originals_and_installs_without_alpine(
    site_checkout: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """The site consumer receives saved runtime and registry bytes in a site-only checkout."""

    def install_saved_site() -> None:
        node = site_checkout / ".cache/downloads/sources/site-node/node.tar.xz"
        npm = site_checkout / ".cache/downloads/sources/npm/npm.tgz"
        assert node.read_bytes() == b"abc"
        assert npm.read_bytes() == b"abc"
        registry = list((site_checkout / ".cache/downloads/site/npm").iterdir())
        assert len(registry) == 1
        assert registry[0].read_bytes() == b"abc"
        (site_checkout / "site/installed").write_bytes(b"installed from preserved originals\n")

    monkeypatch.setattr(site_install, "install_site", install_saved_site)
    with (
        mock.patch.object(env_setup, "setup", side_effect=AssertionError("Alpine setup")),
        mock.patch(
            "urllib.request.urlopen", side_effect=lambda *_args, **_kwargs: io.BytesIO(b"abc")
        ),
    ):
        env_setup.prepare_setup(site=True)

    assert (
        site_checkout / "site/installed"
    ).read_bytes() == b"installed from preserved originals\n"
    assert not (site_checkout / ".cache/environment").exists()
    assert not (site_checkout / ".cache/dependencies").exists()


def test_all_setup_acquires_complete_inventory_and_prepares_both_consumers(
    ready_checkout: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """Complete preparation uses the snapshot's existing inventory without creating an export."""
    inputs = [original("linux", b"abc"), original("windows", b"abc"), original("site", b"abc")]
    monkeypatch.setattr(workflow, "checkout_declarations", lambda: (inputs, {}))

    def install_saved_site() -> None:
        contents = [(ready_checkout / ".cache" / item.destination).read_bytes() for item in inputs]
        assert contents == [b"abc", b"abc", b"abc"]
        (ready_checkout / "site-installed").write_text("installed\n")

    monkeypatch.setattr(site_install, "install_site", install_saved_site)
    with mock.patch(
        "urllib.request.urlopen", side_effect=lambda *_args, **_kwargs: io.BytesIO(b"abc")
    ):
        env_setup.prepare_setup(all_inputs=True, offline=False)

    assert (ready_checkout / "site-installed").read_text() == "installed\n"
    assert (ready_checkout / ".cache/environment/active-generation").read_bytes() == (
        b"shared generation\n"
    )
    assert not (ready_checkout / ".cache/dependencies").exists()
