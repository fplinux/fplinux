# SPDX-License-Identifier: GPL-2.0-only
"""Host component checks for setup's reporting and preparation boundaries."""

from __future__ import annotations

import json
import shutil
from pathlib import Path
from unittest import mock

from fplinux_cli.environment import kern
from fplinux_cli.environment import setup as env_setup
from fplinux_cli.environment.definition import TOOLCHAIN_RECIPE_INPUTS, load_environment_lock
from fplinux_cli.environment.state import EnvironmentState
from fplinux_cli.reporting import run as reporting

from tests import ROOT


def test_ready_setup_finishes_its_reporter(tmp_path: Path) -> None:
    """A prepared-environment hit publishes successful public setup run metadata."""
    expected = EnvironmentState("a" * 64, "b" * 64, "c" * 64)
    with (
        mock.patch.object(env_setup, "ROOT", tmp_path),
        mock.patch.object(kern, "ROOT", tmp_path),
        mock.patch.object(reporting, "ROOT", tmp_path),
        mock.patch.object(env_setup, "install_kern", return_value="kern"),
        mock.patch.object(env_setup, "load_environment_lock", return_value={}),
        mock.patch.object(env_setup, "environment_recipe_digest", return_value="a" * 64),
        mock.patch.object(env_setup, "current_environment_state", return_value=expected),
        mock.patch.object(env_setup, "install_git_hooks"),
    ):
        env_setup.prepare_setup()
    paths = list((tmp_path / ".data/logs/setup").glob("*/run.json"))
    assert len(paths) == 1
    assert json.loads(paths[0].read_text())["status"] == "success"


def test_tool_installation_reserves_eight_gib_in_the_kern_command(tmp_path: Path) -> None:
    """The complete tool installation retains its explicit preparation memory budget."""
    reporter = mock.MagicMock()
    lock = {
        "input": [
            {
                "purpose": "environment-base",
                "destination": "downloads/base.tar.gz",
            },
            {
                "purpose": "environment-package",
                "destination": "downloads/tool.apk",
            },
            {
                "purpose": "environment-bootstrap-package",
                "destination": "downloads/bootstrap-clang.apk",
            },
        ]
    }
    with mock.patch.object(env_setup, "kern_environment", return_value={}):
        env_setup._prepare_rootfs(  # noqa: SLF001 -- inspect the installation execution boundary.
            "kern",
            tmp_path / "candidate",
            context=tmp_path / "recipe",
            toolchain=tmp_path / "generated",
            lock=lock,
            recipe="a" * 64,
            generation="b" * 64,
            reporter=reporter,
            cache=tmp_path / ".data",
        )
    commands = [
        call.args[0]
        for call in reporter.stage.return_value.__enter__.return_value.run.call_args_list
    ]
    installation = next(command for command in commands if command[:2] == ["kern", "box"])
    assert installation[installation.index("--memory") + 1] == "8192m"
    assert f"{tmp_path / 'generated'}:/toolchain:ro" in installation
    assert "/inputs/downloads/tool.apk" in installation
    assert "/inputs/downloads/bootstrap-clang.apk" not in installation


def test_setup_reuses_signed_toolchain_until_a_toolchain_input_changes(tmp_path: Path) -> None:
    """The shared environment's unrelated inputs leave the LLVM package reusable."""
    lock = load_environment_lock()
    contexts = {
        name: tmp_path / name
        for name in ("first", "quality-update", "final-apk", "bootstrap-apk", "script", "original")
    }
    for context in contexts.values():
        for recipe in TOOLCHAIN_RECIPE_INPUTS:
            destination = context / recipe
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / recipe, destination)

    reporter = mock.MagicMock()
    stage = reporter.stage.return_value.__enter__.return_value
    cache = tmp_path / "data"

    def build_toolchain(command: list[str], **_kwargs: object) -> None:
        if command[0] != "kern":
            return
        mount = next(part for part in command if part.endswith(":/work"))
        destination = Path(mount.removesuffix(":/work")) / "out"
        destination.mkdir()
        (destination / "toolchain.apk").write_text(destination.parent.parent.name)
        (destination / "fplinux-toolchain.rsa.pub").write_bytes(b"public key")

    stage.run.side_effect = build_toolchain
    with (
        mock.patch.object(env_setup, "kern_environment", return_value={}),
        mock.patch.object(env_setup, "remove_environment_rootfs"),
    ):

        def package_for(name: str) -> str:
            result = env_setup._prepare_toolchain(  # noqa: SLF001 -- exercise the toolchain boundary.
                "kern", context=contexts[name], lock=lock, reporter=reporter, cache=cache
            )
            return (result / "toolchain.apk").read_text()

        inputs = {record["key"]: record for record in lock["input"]}
        assert package_for("first") == "first"
        inputs["environment:node"]["sha256"] = "a" * 64
        assert package_for("quality-update") == "first"

        final_apk = next(record for key, record in inputs.items() if ":x86_64:7zip-" in key)
        final_apk["sha256"] = "b" * 64
        assert package_for("final-apk") == "first"

        cmake = next(record for key, record in inputs.items() if ":x86_64:cmake-" in key)
        cmake["sha256"] = "c" * 64
        assert package_for("bootstrap-apk") == "bootstrap-apk"

        relative = "scripts/fplinux_cli/environment/install/toolchain/configure-host.sh"
        script = contexts["script"] / relative
        script.write_text(script.read_text().replace("-O2 -DNDEBUG", "-O1 -DNDEBUG"))
        shutil.copyfile(script, contexts["original"] / relative)
        assert package_for("script") == "script"

        inputs["source:llvm_project"]["sha256"] = "d" * 64
        assert package_for("original") == "original"
