# SPDX-License-Identifier: GPL-2.0-only
"""Prepare and publish the exact locked Alpine build-environment rootfs."""

from __future__ import annotations

import json
import os
import secrets
import shutil
import sys
import tempfile
from pathlib import Path
from typing import TYPE_CHECKING, Any

from fplinux_cli.alpine.aports import materialize_aport_sources
from fplinux_cli.alpine.sources import font_source_manifest
from fplinux_cli.build import DEFAULT_BUILD_MEMORY_MIB
from fplinux_cli.build.environment import SOURCE_DATE_EPOCH
from fplinux_cli.build.host_tools.host import extract_host_members
from fplinux_cli.common import (
    ROOT,
    canonical_json_bytes,
    error_message,
    fail,
    load_toml,
    read_json_object,
    replace_file_atomically,
    sha256_file,
)
from fplinux_cli.data_paths import data_root
from fplinux_cli.dependencies.inputs import (
    acquire_inputs,
    build_source_inputs,
    deduplicate_inputs,
    environment_inputs,
    host_inputs,
    report_input_download,
    terminus_font_environment_inputs,
    ui_font_environment_inputs,
    ui_icon_environment_inputs,
)
from fplinux_cli.manifests.identity import BUILD_TYPES
from fplinux_cli.manifests.paths import declared_boot_profiles, target_directory
from fplinux_cli.manifests.platforms import load_platform
from fplinux_cli.manifests.targets import load_target
from fplinux_cli.reporting.run import RunReporter, current_stage

from .definition import (
    ENVIRONMENT_RECIPE_INPUTS,
    TOOLCHAIN_RECIPE_DIR,
    TOOLCHAIN_RECIPE_INPUTS,
    environment_recipe_digest,
    file_recipe_digest,
    load_environment_lock,
    spreadtrum_flash_environment_source,
)
from .git_hooks import install_git_hooks
from .kern import (
    ensure_project_directory,
    install_kern,
    kern_box_name,
    kern_environment,
    report_kern_memory_failure,
)
from .runtime import (
    current_environment_state,
    environment_arguments,
    probe_environment_state,
    publish_environment,
    remove_environment_rootfs,
)

if TYPE_CHECKING:
    from fplinux_cli.dependencies.inputs import DependencyInput
    from fplinux_cli.dependencies.site_inputs import SiteConsumer
    from fplinux_cli.reporting.run import Stage

    from .state import EnvironmentState

_ENVIRONMENT_SETUP_TIMEOUT = 2 * 60 * 60
_TOOLCHAIN_SETUP_TIMEOUT = 12 * 60 * 60
_TOOLCHAIN_MEMORY_MIB = 10 * 1024
_TOOLCHAIN_SOURCES = (
    "source:llvm_project",
    "source:spirv_headers",
    "source:spirv_tools",
    "source:spirv_llvm_translator",
    "source:phoneme_host_musl",
    "source:phoneme_host_libstdcpp",
)
_TOOLCHAIN_OUTPUTS = ("toolchain.apk", "fplinux-toolchain.rsa.pub")


def target_setup_inputs(target: str, *, host: str = "linux") -> list[DependencyInput]:
    """Select every supported build variant and the chosen archive host's originals."""
    declaration = load_toml(target_directory(target) / "target.toml")
    platform = load_platform(declaration["platform"])
    inputs = host_inputs(ROOT, host)
    for profile in declared_boot_profiles(declaration):
        for build_type in BUILD_TYPES:
            config = load_target(target, profile, build_type=build_type)
            inputs.extend(build_source_inputs(ROOT, platform, config))
    return deduplicate_inputs(inputs)


def _acquire_setup_inputs(
    inputs: list[DependencyInput], cache: Path, *, offline: bool, setup_command: str
) -> dict[str, Path]:
    downloaded = 0

    def report(input_file: DependencyInput, position: int, total: int) -> None:
        nonlocal downloaded
        downloaded += 1
        report_input_download(input_file, position, total, label="setup")

    paths = acquire_inputs(
        inputs, cache, offline=offline, setup_command=setup_command, on_download=report
    )
    summary = (
        f"setup: inputs: {len(inputs) - downloaded} cached, {downloaded} downloaded, 0 missing"
    )
    stage = current_stage()
    if stage is not None:
        stage.write((summary + "\n").encode())
    print(summary, file=sys.stderr, flush=True)
    return paths


def prepare_setup(  # noqa: PLR0913 -- public preparation selectors and environment policy stay explicit.
    target: str | None = None,
    *,
    host: str = "linux",
    site: bool = False,
    all_inputs: bool = False,
    force: bool = False,
    offline: bool = False,
) -> None:
    """Prepare selected originals and their current local consumers without an export."""
    cache = data_root(ROOT)
    reporter = RunReporter.create("setup", target=target, verbose=False, selected_data_root=cache)
    site_consumer: SiteConsumer | None = None
    if site or all_inputs:
        if all_inputs:
            from fplinux_cli.dependencies.workflow import (  # noqa: PLC0415 -- complete inventory only.
                checkout_declarations,
            )

            with reporter.stage("setup-selection"):
                inputs, context = checkout_declarations()
                site_consumer = context["site"]["consumer"]
            setup_command = "./fplinux setup --all"
        else:
            from fplinux_cli.dependencies.site_inputs import (  # noqa: PLC0415 -- site-only selection.
                site_dependency_selection,
            )

            with reporter.stage("setup-selection"):
                inputs, site_context = site_dependency_selection(ROOT)
                site_consumer = site_context["consumer"]
            setup_command = "./fplinux setup --site"
    elif target is not None:
        with reporter.stage("setup-selection"):
            inputs = target_setup_inputs(target, host=host)
        setup_command = f"./fplinux setup {target} --host {host}"
    else:
        inputs = []
        setup_command = "./fplinux setup"
    originals = {}
    if inputs:
        with reporter.stage("setup-inputs"):
            originals = _acquire_setup_inputs(
                inputs, cache, offline=offline, setup_command=setup_command
            )
    if not site:
        setup(force=force, offline=offline, reporter=reporter, cache=cache)
    if site_consumer is not None:
        from fplinux_cli.dependencies.site_install import (  # noqa: PLC0415 -- documentation consumer only.
            install_site,
        )

        with reporter.stage("site-install"):
            install_site(ROOT, cache, inputs, originals, site_consumer)
    reporter.finish()


def _stage_environment_inputs(context: Path, *, offline: bool, cache: Path | None = None) -> None:
    """Capture the exact recipe and verified downloads without unrelated cache files."""
    selected_root = data_root(ROOT) if cache is None else cache
    for relative in ENVIRONMENT_RECIPE_INPUTS:
        source = ROOT / relative
        if source.is_symlink() or not source.is_file():
            fail(f"environment recipe input is missing or invalid: {source}")
        destination = context / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)
    input_directory = context / "inputs"
    input_directory.mkdir()
    ui_fonts = {item.key for item in ui_font_environment_inputs(ROOT)}
    terminal_fonts = {item.key for item in terminus_font_environment_inputs(ROOT)}
    ui_icons = {item.key for item in ui_icon_environment_inputs(ROOT)}
    inputs = environment_inputs(ROOT)
    originals = _acquire_setup_inputs(
        [item for item in inputs if item.key != "source:kern"],
        selected_root,
        offline=offline,
        setup_command="./fplinux setup --all",
    )
    for item in inputs:
        if item.key == "source:kern":
            continue
        source = originals[item.key]
        if item.key == "source:spreadtrum_flash":
            projection = spreadtrum_flash_environment_source(ROOT)
            extract_host_members(
                source,
                f"spreadtrum_flash-{projection['commit']}/",
                projection["members"],
                projection["files"],
                context / "quality/spreadtrum-flash",
            )
            continue
        if item.key in ui_fonts:
            destination = context / "quality/ui-fonts" / source.name
            destination.parent.mkdir(parents=True, exist_ok=True)
            os.link(source, destination)
            continue
        if item.key in terminal_fonts:
            destination = context / "quality/terminus" / source.name
            destination.parent.mkdir(parents=True, exist_ok=True)
            os.link(source, destination)
            continue
        if item.key in ui_icons:
            destination = context / "quality/ui-icons" / source.name
            destination.parent.mkdir(parents=True, exist_ok=True)
            os.link(source, destination)
            continue
        destination = (
            context / "npm" / source.name
            if item.purpose == "npm-package"
            else input_directory / item.destination
        )
        destination.parent.mkdir(parents=True, exist_ok=True)
        if not destination.exists():
            os.link(source, destination)
    (context / "quality/ui-fonts/fplinux-font-sources.json").write_text(
        json.dumps(font_source_manifest(ROOT)) + "\n"
    )
    materialize_aport_sources(
        "fplinux-font-terminus", ROOT, context / "quality/terminus-aport", cache=selected_root
    )
    (context / "environment-inputs.json").write_text(json.dumps(load_environment_lock()) + "\n")


def _extract_environment_rootfs(base: Path, rootfs: Path, *, stage: Stage, cache: Path) -> None:
    environment = kern_environment(cache=cache)
    environment["PYTHONPATH"] = str(ROOT / "scripts")
    stage.run(
        [
            "unshare",
            "--map-auto",
            "--map-root-user",
            "--",
            sys.executable,
            "-m",
            "fplinux_cli.environment.rootfs",
            str(base),
            str(rootfs),
        ],
        cwd=ROOT,
        env=environment,
        timeout=_ENVIRONMENT_SETUP_TIMEOUT,
    )


def _environment_box(
    kern: str, name: str, *, rootfs: Path, context: Path, memory_mib: int
) -> list[str]:
    return [
        kern,
        "box",
        kern_box_name(name),
        *environment_arguments(rootfs),
        "--bind-rootfs",
        "--uid-range",
        "--memory",
        f"{memory_mib}m",
        "--network",
        "none",
        "--volume",
        f"{context}:/recipe:ro",
        "--volume",
        f"{context / 'inputs'}:/inputs:ro",
        "--quiet",
    ]


def _toolchain_output_hashes(directory: Path) -> dict[str, str] | None:
    keys = [directory / name for name in _TOOLCHAIN_OUTPUTS]
    if any(not path.is_file() or path.is_symlink() for path in keys):
        return None
    return {path.name: sha256_file(path) for path in keys}


def _prepare_toolchain(
    kern: str, *, context: Path, lock: dict[str, Any], reporter: RunReporter, cache: Path
) -> Path:
    base = next(record for record in lock["input"] if record["purpose"] == "environment-base")
    records = {record["key"]: record for record in lock["input"]}
    packages = [records[key] for key in lock["selection"]["bootstrap_apks"]]
    declared_sources = {
        key for key, entry in records.items() if entry["purpose"] == "environment-toolchain-source"
    }
    if set(_TOOLCHAIN_SOURCES) != declared_sources:
        fail("environment lock must select the six LLVM toolchain source inputs")
    sources = [records[key] for key in _TOOLCHAIN_SOURCES]
    recipe = file_recipe_digest(
        [context / path for path in TOOLCHAIN_RECIPE_INPUTS],
        prefix=canonical_json_bytes([lock["kern"], base, packages, sources, SOURCE_DATE_EPOCH]),
        source_root=context,
    )
    cached = ensure_project_directory(cache / "environment/toolchain")
    output_receipt = cached / "receipt.json"
    with reporter.stage("environment-toolchain") as stage:
        receipt = read_json_object(output_receipt)
        if receipt is not None and receipt.get("recipe") == recipe:
            hashes = _toolchain_output_hashes(cached)
            if hashes is not None and receipt.get("outputs") == hashes:
                return cached

        workspace = context / "toolchain-work"
        workspace.mkdir()
        bootstrap = Path(tempfile.mkdtemp(dir=cached.parent, prefix="candidate-"))
        try:
            _extract_environment_rootfs(
                context / "inputs" / base["destination"], bootstrap, stage=stage, cache=cache
            )
            stage.run(
                [
                    *_environment_box(
                        kern,
                        "environment-toolchain",
                        rootfs=bootstrap,
                        context=context,
                        memory_mib=_TOOLCHAIN_MEMORY_MIB,
                    ),
                    "--env",
                    f"SOURCE_DATE_EPOCH={SOURCE_DATE_EPOCH}",
                    "--volume",
                    f"{workspace}:/work",
                    "--",
                    "sh",
                    f"/recipe/{TOOLCHAIN_RECIPE_DIR}/prepare.sh",
                    *(f"/inputs/{item['destination']}" for item in [*sources, *packages]),
                ],
                cwd=ROOT,
                env=kern_environment(cache=cache),
                timeout=_TOOLCHAIN_SETUP_TIMEOUT,
            )
        except BaseException:
            report_kern_memory_failure(stage, memory_mib=_TOOLCHAIN_MEMORY_MIB, advice="")
            raise
        finally:
            remove_environment_rootfs(bootstrap, cache=cache)

        output = workspace / "out"
        hashes = _toolchain_output_hashes(output)
        if hashes is None:
            fail("LLVM toolchain did not produce the signed package and public key")
        for name in _TOOLCHAIN_OUTPUTS:
            (output / name).replace(cached / name)
        receipt_bytes = canonical_json_bytes({"recipe": recipe, "outputs": hashes})
        replace_file_atomically(output_receipt, receipt_bytes, 0o600, sync=False)
        return cached


def _prepare_rootfs(  # noqa: PLR0913 -- captured inputs, receipt markers and reporting are distinct.
    kern: str,
    candidate: Path,
    *,
    context: Path,
    toolchain: Path,
    lock: dict[str, Any],
    recipe: str,
    generation: str,
    reporter: RunReporter,
    cache: Path,
) -> None:
    base = next(record for record in lock["input"] if record["purpose"] == "environment-base")
    with reporter.stage("environment-base") as stage:
        _extract_environment_rootfs(
            context / "inputs" / base["destination"], candidate, stage=stage, cache=cache
        )
    packages = [
        str(Path("/inputs") / record["destination"])
        for record in lock["input"]
        if record["purpose"] == "environment-package"
    ]
    with reporter.stage("environment-install") as stage:
        stage.run(
            [
                *_environment_box(
                    kern,
                    "environment-install",
                    rootfs=candidate,
                    context=context,
                    memory_mib=DEFAULT_BUILD_MEMORY_MIB,
                ),
                "--volume",
                f"{toolchain}:/toolchain:ro",
                "--",
                "sh",
                "/recipe/scripts/fplinux_cli/environment/install/prepare.sh",
                "/recipe/environment-inputs.json",
                "/inputs",
                "/recipe",
                recipe,
                generation,
                *packages,
            ],
            cwd=ROOT,
            env=kern_environment(cache=cache),
            timeout=_ENVIRONMENT_SETUP_TIMEOUT,
        )


def setup(
    *,
    force: bool = False,
    offline: bool = False,
    reporter: RunReporter,
    cache: Path | None = None,
) -> EnvironmentState:
    """Reuse an exact environment or prepare a replacement while retaining the old tree."""
    lock = load_environment_lock()
    selected_root = data_root(ROOT) if cache is None else cache

    def report_kern() -> None:
        kern_input = next(item for item in environment_inputs(ROOT) if item.key == "source:kern")
        report_input_download(kern_input, 1, 1, label="setup")

    with reporter.stage("kern"):
        kern = install_kern(lock, offline=offline, cache=selected_root, on_download=report_kern)
    environment_recipe = environment_recipe_digest(lock)
    current = current_environment_state(kern, environment_recipe, cache=selected_root)
    if current is not None and not force:
        install_git_hooks()
        print("Build environment is ready")
        return current

    directory = ensure_project_directory(selected_root / "environment")
    candidate = Path(tempfile.mkdtemp(dir=directory, prefix="candidate-"))
    generation = secrets.token_hex(32)
    failed = False
    try:
        with tempfile.TemporaryDirectory(dir=directory, prefix="recipe-") as temporary:
            context = Path(temporary)
            with reporter.stage("environment-inputs"):
                _stage_environment_inputs(context, offline=offline, cache=selected_root)
                if read_json_object(context / "environment-inputs.json") != lock:
                    fail("environment lock changed while staging its installation inputs")
            toolchain = _prepare_toolchain(
                kern, context=context, lock=lock, reporter=reporter, cache=selected_root
            )
            _prepare_rootfs(
                kern,
                candidate,
                context=context,
                toolchain=toolchain,
                lock=lock,
                recipe=environment_recipe,
                generation=generation,
                reporter=reporter,
                cache=selected_root,
            )
        if environment_recipe_digest() != environment_recipe:
            fail("environment inputs changed while setup was running")
        state = probe_environment_state(kern, candidate, environment_recipe, cache=selected_root)
        if state is None or state.generation != generation:
            fail("environment preparation did not publish the exact requested generation")
        state = publish_environment(kern, candidate, state, cache=selected_root)
    except BaseException:
        failed = True
        raise
    finally:
        if candidate.exists():
            try:
                remove_environment_rootfs(candidate, cache=selected_root)
            except BaseException as error:
                if not failed:
                    raise
                print(
                    error_message(f"could not remove failed environment candidate: {error}"),
                    file=sys.stderr,
                )
    install_git_hooks()
    return state
