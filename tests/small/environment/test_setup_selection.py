# SPDX-License-Identifier: GPL-2.0-only
"""Host preparation of selected originals while reusing the shared environment."""

from __future__ import annotations

import base64
import hashlib
import io
import json
from pathlib import Path
from typing import TYPE_CHECKING
from unittest import mock

import pytest
from fplinux_cli import common
from fplinux_cli.dependencies import inputs as dependency_inputs
from fplinux_cli.dependencies import site_inputs, site_install, workflow
from fplinux_cli.dependencies.inputs import DependencyInput
from fplinux_cli.environment import setup as env_setup
from fplinux_cli.environment.downloads import download_locked_file
from fplinux_cli.environment.state import EnvironmentState
from fplinux_cli.reporting import run as reporting

if TYPE_CHECKING:
    from collections.abc import Mapping, Sequence

    from fplinux_cli.dependencies.site_inputs import SiteConsumer


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
    monkeypatch.setattr(env_setup, "current_environment_state", lambda *_args, **_kwargs: state)
    monkeypatch.setattr(env_setup, "install_git_hooks", lambda: None)

    def unexpected_rebuild(*_args: object, **_kwargs: object) -> None:
        pytest.fail("a ready shared environment must not be rebuilt for selected inputs")

    monkeypatch.setattr(env_setup, "_prepare_rootfs", unexpected_rebuild)
    marker = tmp_path / ".data/environment/active-generation"
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
        assert (ready_checkout / ".data" / item.destination).read_bytes() == contents

    assert (ready_checkout / ".data/environment/active-generation").read_bytes() == (
        b"shared generation\n"
    )
    assert (ready_checkout / ".data/downloads/sources/first/original.tar.gz").read_bytes() == (
        b"updated bytes\n"
    )
    runs = list((ready_checkout / ".data/logs/setup").glob("*/*/run.json"))
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

    assert not (ready_checkout / ".data" / item.destination).exists()
    assert (ready_checkout / ".data/environment/active-generation").read_bytes() == (
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

    def install_saved_site(
        root: Path,
        cache: Path,
        inputs: Sequence[DependencyInput],
        originals: Mapping[str, Path],
        consumer: SiteConsumer,
    ) -> None:
        assert root == site_checkout
        assert cache == site_checkout / ".data"
        assert consumer["node"] == "24.21.0"
        assert consumer["npm"] == "12.2.0"
        assert {item.key for item in inputs} == {
            "source:site-node",
            "source:npm",
            "npm:site:node_modules/example",
        }
        assert {key: path.read_bytes() for key, path in originals.items()} == {
            "source:site-node": b"abc",
            "source:npm": b"abc",
            "npm:site:node_modules/example": b"abc",
        }
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
    assert not (site_checkout / ".data/environment").exists()
    assert not (site_checkout / ".data/dependencies").exists()


@pytest.mark.parametrize("all_inputs", [False, True], ids=["site", "all"])
def test_offline_site_setup_checks_each_saved_original_once(
    site_checkout: Path,
    ready_checkout: Path,
    monkeypatch: pytest.MonkeyPatch,
    *,
    all_inputs: bool,
) -> None:
    """Verification reads each saved archive once before offline npm receives the selection."""
    assert ready_checkout == site_checkout
    monkeypatch.setattr(workflow, "dependency_selection", lambda _root: ([], {}))
    cache = site_checkout / ".data"
    for relative in (
        "downloads/sources/site-node/node.tar.xz",
        "downloads/sources/npm/npm.tgz",
    ):
        path = cache / relative
        path.parent.mkdir(parents=True)
        path.write_bytes(b"abc")
    inputs, _context = site_inputs.site_dependency_selection(site_checkout)
    registry = next(item for item in inputs if item.purpose == "site-npm-package")
    registry_archive = cache / registry.destination
    registry_archive.parent.mkdir(parents=True)
    registry_archive.write_bytes(b"abc")
    environment = site_checkout / "scripts/fplinux_cli/environment"
    environment.mkdir(parents=True)
    for source in ("node_runtime.py", "archives.py", "install.py"):
        (environment / source).write_bytes(b"fixture runtime installer\n")

    def unpack_node(node: Path, npm: Path, runtime: Path) -> None:
        assert node.read_bytes() == b"abc"
        assert npm.read_bytes() == b"abc"
        runtime.mkdir()

    def import_packages(_npm: Path, _cache: Path, archives: Sequence[Path]) -> None:
        assert list(archives) == [registry_archive]

    monkeypatch.setattr(site_install, "install_node_runtime", unpack_node)
    monkeypatch.setattr(site_install, "populate_npm_cache", import_packages)
    runner = mock.Mock()
    runner.run.side_effect = [
        mock.Mock(returncode=0, stdout="v24.21.0\n"),
        mock.Mock(returncode=0, stdout="12.2.0\n"),
        mock.Mock(returncode=0),
    ]
    monkeypatch.setattr(site_install, "subprocess", runner)

    with (
        mock.patch("urllib.request.urlopen", side_effect=AssertionError("network")),
        mock.patch.object(
            dependency_inputs, "download_locked_file", wraps=download_locked_file
        ) as verify,
    ):
        env_setup.prepare_setup(site=not all_inputs, all_inputs=all_inputs, offline=True)

    assert sorted(call.args[2] for call in verify.call_args_list) == sorted(
        (
            cache / "downloads/sources/site-node/node.tar.xz",
            cache / "downloads/sources/npm/npm.tgz",
            registry_archive,
        )
    )
    assert runner.run.call_args.args[0][1:3] == ["ci", "--offline"]
    assert (cache / "site/node").is_dir()


def test_all_setup_acquires_complete_inventory_and_prepares_both_consumers(
    ready_checkout: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """Complete preparation uses the snapshot's existing inventory without creating an export."""
    inputs = [
        original("linux_host_python3", b"abc"),
        original("windows_host_python", b"abc"),
        original("site-node", b"abc"),
    ]
    consumer = {
        "node": "24.21.0",
        "npm": "12.2.0",
        "platform": "linux",
        "arch": "x86_64",
        "libc": "glibc",
    }
    monkeypatch.setattr(
        workflow, "checkout_declarations", lambda: (inputs, {"site": {"consumer": consumer}})
    )

    def install_saved_site(
        root: Path,
        cache: Path,
        inputs: Sequence[DependencyInput],
        originals: Mapping[str, Path],
        selected_consumer: SiteConsumer,
    ) -> None:
        assert root == ready_checkout
        assert cache == ready_checkout / ".data"
        assert selected_consumer == consumer
        assert {item.key for item in inputs} == {
            "source:linux_host_python3",
            "source:windows_host_python",
            "source:site-node",
        }
        assert {key: path.read_bytes() for key, path in originals.items()} == {
            "source:linux_host_python3": b"abc",
            "source:windows_host_python": b"abc",
            "source:site-node": b"abc",
        }
        (ready_checkout / "site-installed").write_text("installed\n")

    monkeypatch.setattr(site_install, "install_site", install_saved_site)
    with mock.patch(
        "urllib.request.urlopen", side_effect=lambda *_args, **_kwargs: io.BytesIO(b"abc")
    ):
        env_setup.prepare_setup(all_inputs=True, offline=False)

    assert (ready_checkout / "site-installed").read_text() == "installed\n"
    assert (ready_checkout / ".data/environment/active-generation").read_bytes() == (
        b"shared generation\n"
    )
    assert not (ready_checkout / ".data/dependencies").exists()
