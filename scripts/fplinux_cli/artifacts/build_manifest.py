# SPDX-License-Identifier: GPL-2.0-only
"""Define the build manifest shared by bundle publication and delivered runtimes."""

BUILD_MANIFEST_FIELDS = frozenset(
    {
        "rootfs_receipt",
        "boot_artifacts",
        "environment_recipe",
        "environment_content",
        "apk_signing_key",
        "device_identity",
        "files",
        "generation",
        "kbuild_receipt",
        "kernel_artifacts",
        "linux_recipe",
        "profile",
        "release_id",
        "source_commit",
        "source_snapshot",
        "build_type",
        "target",
        "workspace_digest",
    }
)
