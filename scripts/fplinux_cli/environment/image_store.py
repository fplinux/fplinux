# SPDX-License-Identifier: GPL-2.0-only
"""Inspect and publish exact images in the project-local Kern store."""

from __future__ import annotations

import json
import os
import re
import secrets
import subprocess
from typing import TYPE_CHECKING, Any

from fplinux_cli.common import ROOT, fail

from .image_state import ImageState, ImageStateError, publish_image_state
from .images import (
    container_base_image_reference,
    container_image_recipe_digest,
    container_image_reference,
)
from .kern import KERN_PROBE_TIMEOUT, kern_box_name, kern_environment

if TYPE_CHECKING:
    from collections.abc import Callable


_SHA256 = re.compile(r"[0-9a-f]{64}\Z")


def image_references(kern: str) -> frozenset[str]:
    """Return the exact images in this checkout's isolated Kern store."""
    try:
        result = subprocess.run(
            [kern, "images", "--json"],
            cwd=ROOT,
            env=kern_environment(),
            capture_output=True,
            text=True,
            check=False,
            timeout=KERN_PROBE_TIMEOUT,
        )
    except subprocess.TimeoutExpired:
        fail(f"Kern image inventory timed out after {KERN_PROBE_TIMEOUT}s")
    if result.returncode:
        fail(result.stderr.strip() or "Kern image inventory failed")
    try:
        payload = json.loads(result.stdout)
    except json.JSONDecodeError:
        fail("Kern image inventory is not valid JSON")
    if not isinstance(payload, list):
        fail("Kern image inventory root is invalid")
    references: set[str] = set()
    for entry in payload:
        reference = entry.get("image") if isinstance(entry, dict) else None
        if not isinstance(reference, str) or not reference:
            fail("Kern image inventory entry is invalid")
        references.add(reference)
    return frozenset(references)


def remove_images(kern: str, references: set[str]) -> None:
    """Remove exact provider-owned image references, never the whole Kern store."""
    if not references:
        return
    try:
        result = subprocess.run(
            [kern, "rmi", *sorted(references)],
            cwd=ROOT,
            env=kern_environment(),
            capture_output=True,
            text=True,
            check=False,
            timeout=KERN_PROBE_TIMEOUT,
        )
    except subprocess.TimeoutExpired:
        fail(f"Kern image removal timed out after {KERN_PROBE_TIMEOUT}s")
    if result.returncode:
        fail(result.stderr.strip() or "Kern image removal failed")


def prune_build_history(kern: str) -> None:
    """Remove provider build records after FPLinux has retained its own setup logs."""
    try:
        result = subprocess.run(
            [kern, "build", "prune", "--keep", "0"],
            cwd=ROOT,
            env=kern_environment(),
            capture_output=True,
            text=True,
            check=False,
            timeout=KERN_PROBE_TIMEOUT,
        )
    except subprocess.TimeoutExpired:
        fail(f"Kern build-history pruning timed out after {KERN_PROBE_TIMEOUT}s")
    if result.returncode:
        fail(result.stderr.strip() or "Kern build-history pruning failed")


def discard_obsolete_images(
    kern: str,
    lock: dict[str, Any],
    image_recipe: str,
) -> None:
    """Keep only the exact current FPLinux base and build images."""
    keep = {
        container_base_image_reference(lock),
        container_image_reference(lock, image_recipe),
    }
    prefixes = (
        f"{lock['oci']['base_repository']}:",
        f"{lock['oci']['repository']}:",
    )
    stale = {
        reference
        for reference in image_references(kern)
        if reference not in keep and reference.startswith(prefixes)
    }
    remove_images(kern, stale)


def discard_transient_images(kern: str, lock: dict[str, Any]) -> None:
    """Remove only abandoned FPLinux staging and backup tags from an earlier invocation."""
    prefixes = (
        f"{lock['oci']['base_repository']}:",
        f"{lock['oci']['repository']}:",
    )
    stale = {
        reference
        for reference in image_references(kern)
        if reference.startswith(prefixes) and ("-staging-" in reference or "-backup-" in reference)
    }
    remove_images(kern, stale)


def temporary_image_reference(image: str, role: str) -> str:
    """Return one invocation-owned staging or backup tag beside a final image tag."""
    return f"{image}-{role}-{os.getpid()}-{secrets.token_hex(3)}"


def tag_image(kern: str, source: str, destination: str) -> None:
    """Apply one bounded provider tag operation."""
    try:
        result = subprocess.run(
            # A flat image copy must retain ownership and set-id permissions.
            ["unshare", "--map-auto", "--map-root-user", "--", kern, "tag", source, destination],
            cwd=ROOT,
            env=kern_environment(),
            capture_output=True,
            text=True,
            check=False,
            timeout=KERN_PROBE_TIMEOUT,
        )
    except subprocess.TimeoutExpired:
        fail(f"Kern image publication timed out after {KERN_PROBE_TIMEOUT}s")
    if result.returncode:
        fail(result.stderr.strip() or "Kern image publication failed")


def publish_staged_image(
    kern: str,
    staging: str,
    destination: str,
    validate: Callable[[str], bool],
) -> None:
    """Replace one consumer tag while retaining a restorable last-good image."""
    existing = destination in image_references(kern)
    backup = temporary_image_reference(destination, "backup") if existing else None
    if backup is not None:
        tag_image(kern, destination, backup)
    try:
        tag_image(kern, staging, destination)
        if not validate(destination):
            fail("published Kern image failed its exact validation")
    except BaseException:
        if backup is not None:
            tag_image(kern, backup, destination)
        else:
            current = image_references(kern)
            remove_images(kern, {destination} & set(current))
        raise
    finally:
        current = image_references(kern)
        disposable = {staging}
        if backup is not None:
            disposable.add(backup)
        remove_images(kern, disposable & set(current))


def image_metadata(kern: str, image: str) -> tuple[str, str, str] | None:
    """Require the declared recipe, generation and freshly checked installed content."""
    try:
        result = subprocess.run(
            [
                kern,
                "box",
                kern_box_name("image-probe"),
                "--image",
                image,
                "--pull",
                "never",
                "--read-only",
                "--network",
                "none",
                "--quiet",
                "--",
                "sh",
                "-ceu",
                (
                    "cat /etc/fplinux-image-state; "
                    "python3 -B /usr/local/libexec/fplinux-image-content.py"
                ),
            ],
            cwd=ROOT,
            env=kern_environment(),
            capture_output=True,
            text=True,
            check=False,
            timeout=KERN_PROBE_TIMEOUT,
        )
    except subprocess.TimeoutExpired:
        fail(f"Kern image lookup timed out after {KERN_PROBE_TIMEOUT}s")
    lines = result.stdout.splitlines()
    if result.returncode != 0 or len(lines) != 4:
        return None
    recipe, generation, content, observed_content = lines
    if _SHA256.fullmatch(recipe) is None:
        return None
    if _SHA256.fullmatch(generation) is None:
        return None
    if _SHA256.fullmatch(content) is None or observed_content != content:
        return None
    return recipe, generation, content


def image_generation(kern: str, image: str) -> str | None:
    """Return the exact build generation embedded in one project-built Kern image."""
    metadata = image_metadata(kern, image)
    return None if metadata is None else metadata[1]


def current_image_state(
    kern: str,
    image: str,
    image_recipe: str | None = None,
) -> ImageState | None:
    """Read one valid recipe, generation and content identity with a single Kern probe."""
    metadata = image_metadata(kern, image)
    if metadata is None:
        return None
    if image_recipe is None:
        image_recipe = container_image_recipe_digest()
    recipe, generation, content = metadata
    if recipe != image_recipe:
        return None
    try:
        return ImageState(recipe, generation, content)
    except ImageStateError:
        return None


def publish_current_image_state(
    kern: str,
    image: str,
    image_recipe: str,
    *,
    state: ImageState | None = None,
) -> ImageState:
    """Persist the marker of one image already checked against its static recipe."""
    if state is None:
        state = current_image_state(kern, image, image_recipe)
    if state is None or state.container_image_recipe != image_recipe:
        fail("current build image has no valid generation")
    try:
        publish_image_state(ROOT / ".cache", state)
    except ImageStateError as error:
        fail(f"could not publish host image state: {error}")
    return state


def base_image_ready(kern: str, lock: dict[str, Any], image: str | None = None) -> bool:
    """Require the local base tag to expose the exact locked release and rootfs marker."""
    oci = lock["oci"]
    if image is None:
        image = container_base_image_reference(lock)
    try:
        result = subprocess.run(
            [
                kern,
                "box",
                kern_box_name("base-probe"),
                "--image",
                image,
                "--pull",
                "never",
                "--read-only",
                "--network",
                "none",
                "--no-uid-range",
                "--quiet",
                "--",
                "/bin/cat",
                "/etc/alpine-release",
                "/etc/fplinux-base-rootfs-sha256",
            ],
            cwd=ROOT,
            env=kern_environment(),
            capture_output=True,
            text=True,
            check=False,
            timeout=KERN_PROBE_TIMEOUT,
        )
    except subprocess.TimeoutExpired:
        fail(f"Kern base image lookup timed out after {KERN_PROBE_TIMEOUT}s")
    expected = f"{oci['base_release']}\n{oci['base_rootfs_sha256']}\n"
    return result.returncode == 0 and result.stdout == expected
