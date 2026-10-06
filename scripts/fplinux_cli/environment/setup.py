# SPDX-License-Identifier: GPL-2.0-only
"""Prepare the pinned base image and publish the exact build environment."""

from __future__ import annotations

import secrets
import shutil
import tarfile
import tempfile
from functools import partial
from pathlib import Path
from typing import TYPE_CHECKING, Any

from fplinux_cli.common import ROOT, alpine_tar_filter, fail
from fplinux_cli.dependencies.inputs import environment_inputs
from fplinux_cli.reporting.run import RunReporter

from .downloads import download_locked_file
from .git_hooks import install_git_hooks
from .image_store import (
    base_image_ready,
    current_image_state,
    discard_obsolete_images,
    discard_transient_images,
    image_metadata,
    prune_build_history,
    publish_current_image_state,
    publish_staged_image,
    temporary_image_reference,
)
from .images import (
    container_base_image_reference,
    container_image_build_arguments,
    container_image_recipe_digest,
    container_image_reference,
    load_container_lock,
)
from .kern import ensure_project_directory, install_kern, kern_environment

if TYPE_CHECKING:
    from .image_state import ImageState

_CONTAINER_SETUP_TIMEOUT = 2 * 60 * 60
_alpine_tar_filter = partial(alpine_tar_filter, on_error=fail)


def _build_base_image(
    kern: str, reporter: RunReporter, lock: dict[str, Any], *, offline: bool = False
) -> None:
    """Build one local Kern base from the exact official Alpine minirootfs archive."""
    oci = lock["oci"]
    image = container_base_image_reference(lock)
    staging_image = temporary_image_reference(image, "staging")
    archive = download_locked_file(
        oci["base_rootfs_url"],
        oci["base_rootfs_sha256"],
        ROOT / ".cache/downloads/kern/alpine-minirootfs.tar.gz",
        offline=offline,
    )
    temporary_parent = ensure_project_directory(ROOT / ".cache/kern")
    with tempfile.TemporaryDirectory(dir=temporary_parent, prefix="base-build-") as temporary:
        context = Path(temporary)
        rootfs = context / "rootfs"
        rootfs.mkdir()
        with tarfile.open(archive, "r:gz") as bundle:
            bundle.extractall(  # noqa: S202 -- every member passes the data-derived filter above.
                rootfs,
                filter=_alpine_tar_filter,
            )
        marker = rootfs / "etc/fplinux-base-rootfs-sha256"
        marker.write_text(f"{oci['base_rootfs_sha256']}\n", encoding="utf-8")
        recipe = context / "Containerfile"
        recipe.write_text("FROM scratch\nCOPY rootfs/ /\n", encoding="utf-8")
        with reporter.stage("container-base") as stage:
            stage.run(
                [
                    kern,
                    "build",
                    "-t",
                    staging_image,
                    "-f",
                    str(recipe),
                    str(context),
                ],
                cwd=ROOT,
                env=kern_environment(),
                timeout=_CONTAINER_SETUP_TIMEOUT,
            )
    if not base_image_ready(kern, lock, staging_image):
        fail("Kern base build completed without publishing the exact locked rootfs")
    publish_staged_image(
        kern,
        staging_image,
        image,
        lambda candidate: base_image_ready(kern, lock, candidate),
    )


def _stage_container_context(context: Path, *, offline: bool) -> None:
    """Stage the recipe and exact saved inputs without copying unrelated cache files."""
    for relative in (
        ".kernignore",
        "Containerfile",
        "scripts/fplinux_cli/environment/image_content.py",
        "package.json",
        "package-lock.json",
        "alpine/aports/fplinux-libtsm/0001-xterm-function-keys.patch",
    ):
        source = ROOT / relative
        if source.is_symlink() or not source.is_file():
            fail(f"container image input is missing or invalid: {source}")
        destination = context / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)
    input_directory = context / "inputs"
    input_directory.mkdir()
    for item in environment_inputs(ROOT):
        if item.purpose in {"container-base", "kern-runtime"}:
            continue
        expected = item.checksum or item.sha256
        if expected is None:
            fail(f"environment input has no declared checksum: {item.key}")
        source = download_locked_file(
            item.url,
            expected,
            ROOT / ".cache" / item.destination,
            offline=offline,
            algorithm=item.algorithm,
            size=item.size,
        )
        if item.purpose == "npm-package":
            relative_input = Path("npm") / source.name
        elif item.purpose == "container-source":
            relative_input = Path("sources") / Path(item.destination).relative_to(
                "downloads/environment"
            )
        else:
            relative_input = Path(item.destination).relative_to("downloads/environment")
        destination = input_directory / relative_input
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)


def setup(
    *,
    force: bool = False,
    offline: bool = False,
    reporter: RunReporter | None = None,
    lock: dict[str, Any] | None = None,
    image_recipe: str | None = None,
) -> ImageState:
    own_reporter = reporter is None
    if reporter is None:
        reporter = RunReporter.create("setup", target=None, verbose=False)
    if lock is None:
        lock = load_container_lock()
    kern = install_kern(lock, offline=offline)
    prune_build_history(kern)
    current_recipe = container_image_recipe_digest(lock)
    if image_recipe is not None and image_recipe != current_recipe:
        fail("container image inputs changed before setup")
    image_recipe = current_recipe
    image = container_image_reference(lock, image_recipe)
    discard_transient_images(kern, lock)
    current_state = current_image_state(kern, image, image_recipe)
    if current_state is not None and not force:
        state = publish_current_image_state(
            kern,
            image,
            image_recipe,
            state=current_state,
        )
        discard_obsolete_images(kern, lock, image_recipe)
        install_git_hooks()
        print(f"Build image is ready: {image}")
        if own_reporter:
            reporter.finish()
        return state

    if not base_image_ready(kern, lock):
        _build_base_image(kern, reporter, lock, offline=offline)

    generation = secrets.token_hex(32)
    staging_image = temporary_image_reference(image, "staging")
    command = [
        kern,
        "build",
        "-t",
        staging_image,
        *container_image_build_arguments(lock),
        "--build-arg",
        f"FPLINUX_IMAGE_RECIPE={image_recipe}",
        "--build-arg",
        f"FPLINUX_IMAGE_GENERATION={generation}",
    ]
    command.append(".")
    temporary_parent = ensure_project_directory(ROOT / ".cache/kern")
    with tempfile.TemporaryDirectory(dir=temporary_parent, prefix="image-build-") as temporary:
        context = Path(temporary)
        with reporter.stage("container-inputs"):
            _stage_container_context(context, offline=offline)
        with reporter.stage("container-setup") as stage:
            stage.run(
                command,
                cwd=context,
                env=kern_environment(),
                timeout=_CONTAINER_SETUP_TIMEOUT,
            )
    if container_image_recipe_digest(lock) != image_recipe:
        fail("container image inputs changed while setup was running")
    metadata = image_metadata(kern, staging_image)
    if metadata is None or metadata[:2] != (image_recipe, generation):
        fail("container setup completed without publishing the exact requested image")
    publish_staged_image(
        kern,
        staging_image,
        image,
        lambda candidate: image_metadata(kern, candidate) == metadata,
    )
    state = publish_current_image_state(kern, image, image_recipe)
    discard_obsolete_images(kern, lock, image_recipe)
    prune_build_history(kern)
    install_git_hooks()
    if own_reporter:
        reporter.finish()
    return state
