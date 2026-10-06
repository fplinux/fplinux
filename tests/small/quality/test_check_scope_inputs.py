# SPDX-License-Identifier: GPL-2.0-only
"""Causal source closures and recipes for selected check scopes."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from fplinux_cli.quality.inputs import check_scope_closure_digest
from fplinux_cli.quality.receipts import (
    CheckReceiptRecipe,
    check_scope_receipt_recipe,
    publish_success_receipt,
    receipt_matches,
)
from fplinux_cli.workspace.capture import WorkspaceFile, WorkspaceSnapshot


class CheckScopeTests(unittest.TestCase):
    """Keep scope recipes bound to the inputs checked by their consumers."""

    def test_registration_edits_revoke_affected_check_scope_receipts(self) -> None:
        """Ownership and shared-source declarations cannot reuse checks of the old inputs."""
        cases = (
            (
                "alpine",
                b'COMMON_PACKAGES = ("fplinux-base",)\n',
                b'COMMON_PACKAGES = ("fplinux-base", "fplinux-cpuclock")\n',
            ),
            (
                "c",
                b'SHARED_APORT_SOURCES = {"consumer-a": ("shared.h",), "consumer-b": ()}\n',
                b'SHARED_APORT_SOURCES = {"consumer-a": (), "consumer-b": ("shared.h",)}\n',
            ),
        )
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary)
            for scope, original, changed in cases:
                with self.subTest(scope=scope):

                    def receipt(scope: str, registration: bytes) -> CheckReceiptRecipe:
                        snapshot = WorkspaceSnapshot(
                            (
                                WorkspaceFile(
                                    "scripts/fplinux_cli/alpine/registration.py",
                                    registration,
                                    0o644,
                                ),
                                WorkspaceFile(
                                    "alpine/aports/fplinux-base/APKBUILD", b"pkgname=base\n", 0o644
                                ),
                                WorkspaceFile(
                                    "alpine/aports/consumer-a/app.c", b"int app;\n", 0o644
                                ),
                                WorkspaceFile(
                                    "scripts/fplinux_cli/quality/kernel/analysis.py",
                                    b"# checker\n",
                                    0o644,
                                ),
                            ),
                            "a" * 64,
                        )
                        return check_scope_receipt_recipe(
                            scope,
                            check_scope_closure_digest(scope, snapshot),
                            image_generation="b" * 64,
                            orchestration_recipe="c" * 64,
                        )

                    publish_success_receipt(cache, receipt(scope, original))
                    self.assertTrue(receipt_matches(cache, receipt(scope, original)))
                    self.assertFalse(receipt_matches(cache, receipt(scope, changed)))

    def test_kernel_receipt_tracks_only_the_selected_build_type_fragment(self) -> None:
        """Editing release policy invalidates release checks while preserving debug reuse."""
        files = (
            WorkspaceFile("targets/phone/target.toml", b'platform = "demo"\n', 0o644),
            WorkspaceFile(
                "platforms/demo/platform.toml",
                b'[linux.build_types]\nrelease = "release.config"\ndebug = "debug.config"\n',
                0o644,
            ),
            WorkspaceFile("release.config", b"CONFIG_LOG_BUF_SHIFT=16\n", 0o644),
            WorkspaceFile("debug.config", b"CONFIG_LOG_BUF_SHIFT=17\n", 0o644),
        )
        before = WorkspaceSnapshot(files, "a" * 64)
        after = WorkspaceSnapshot(
            (
                *files[:2],
                WorkspaceFile("release.config", b"CONFIG_LOG_BUF_SHIFT=15\n", 0o644),
                files[3],
            ),
            "b" * 64,
        )
        self.assertNotEqual(
            check_scope_closure_digest("kernel", before),
            check_scope_closure_digest("kernel", after),
        )
        self.assertEqual(
            check_scope_closure_digest("kernel", before, build_type="debug"),
            check_scope_closure_digest("kernel", after, build_type="debug"),
        )
        release = check_scope_receipt_recipe(
            "python", "a" * 64, image_generation="b" * 64, orchestration_recipe="c" * 64
        )
        debug = check_scope_receipt_recipe(
            "python",
            "a" * 64,
            image_generation="b" * 64,
            orchestration_recipe="c" * 64,
            build_type="debug",
        )
        self.assertEqual(release, debug)

    def test_scope_receipt_misses_after_orchestration_or_generation_changes(self) -> None:
        """Do not reuse one scope across checker or image generation changes."""
        closure = "a" * 64
        orchestration = "b" * 64
        generation = "c" * 64
        first = check_scope_receipt_recipe(
            "python", closure, image_generation=generation, orchestration_recipe=orchestration
        )
        generation_changed = check_scope_receipt_recipe(
            "python", closure, image_generation="d" * 64, orchestration_recipe=orchestration
        )
        orchestration_changed = check_scope_receipt_recipe(
            "python", closure, image_generation=generation, orchestration_recipe="e" * 64
        )
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary)
            publish_success_receipt(cache, first)
            self.assertTrue(receipt_matches(cache, first))
            self.assertFalse(receipt_matches(cache, generation_changed))
            self.assertFalse(receipt_matches(cache, orchestration_changed))

    def test_kernel_receipt_recipe_carries_the_selected_profile(self) -> None:
        """Record a named profile in the kernel recipe and its persisted payload."""
        default = check_scope_receipt_recipe(
            "kernel",
            "a" * 64,
            image_generation="b" * 64,
            orchestration_recipe="c" * 64,
        )
        profile = check_scope_receipt_recipe(
            "kernel",
            "a" * 64,
            image_generation="b" * 64,
            orchestration_recipe="c" * 64,
            profile="microsd-uboot",
        )

        self.assertIsNone(default.profile)
        self.assertEqual(profile.profile, "microsd-uboot")
        self.assertNotEqual(default.payload(), profile.payload())

    def test_readme_does_not_invalidate_c_or_kernel_scope(self) -> None:
        """Keep unrelated documentation outside the two expensive closures."""
        common = (
            WorkspaceFile("README.md", b"first", 0o644),
            WorkspaceFile("alpine/aports/demo-consumer/app.c", b"int app;\n", 0o644),
            WorkspaceFile("targets/phone/kernel/board.c", b"int board;\n", 0o644),
            WorkspaceFile("scripts/fplinux_cli/quality/kernel/analysis.py", b"checker\n", 0o644),
            WorkspaceFile(
                "targets/phone/target.toml",
                b'platform = "phone"\n'
                b"[linux]\n"
                b'copies = [{ source = "kernel/board.c", destination = "board.c" }]\n',
                0o644,
            ),
            WorkspaceFile(
                "platforms/phone/platform.toml",
                b"[linux]\npatches = []\ncopies = []\nappends = []\n",
                0o644,
            ),
        )
        changed = (WorkspaceFile("README.md", b"second", 0o644), *common[1:])
        first = WorkspaceSnapshot(common, "a" * 64)
        second = WorkspaceSnapshot(changed, "b" * 64)
        self.assertEqual(
            check_scope_closure_digest("c", first),
            check_scope_closure_digest("c", second),
        )
        self.assertEqual(
            check_scope_closure_digest("kernel", first),
            check_scope_closure_digest("kernel", second),
        )
        self.assertNotEqual(
            check_scope_closure_digest("source", first),
            check_scope_closure_digest("source", second),
        )

        kernel_changed = WorkspaceSnapshot(
            (
                common[0],
                common[1],
                WorkspaceFile("targets/phone/kernel/board.c", b"int changed;\n", 0o644),
                common[3],
                common[4],
                common[5],
            ),
            "c" * 64,
        )
        self.assertEqual(
            check_scope_closure_digest("c", first),
            check_scope_closure_digest("c", kernel_changed),
        )
        self.assertNotEqual(
            check_scope_closure_digest("kernel", first),
            check_scope_closure_digest("kernel", kernel_changed),
        )

    def test_c_harness_change_updates_the_c_closure(self) -> None:
        """Treat a host C harness as a causal input to the C scope."""
        first = WorkspaceSnapshot(
            (
                WorkspaceFile(
                    "tests/host_tool/harness.c",
                    b"int main(void) { return 0; }\n",
                    0o644,
                ),
            ),
            "a" * 64,
        )
        second = WorkspaceSnapshot(
            (
                WorkspaceFile(
                    "tests/host_tool/harness.c",
                    b"int main(void) { return 1; }\n",
                    0o644,
                ),
            ),
            "b" * 64,
        )

        self.assertNotEqual(
            check_scope_closure_digest("c", first),
            check_scope_closure_digest("c", second),
        )

    def test_kernel_scope_tracks_manifest_sources_without_whole_bootstrap(self) -> None:
        """Track every projected Linux input but ignore bootstrap-only sources."""
        target_manifest = (
            b'platform = "demo"\n'
            b"[linux]\n"
            b'patches = ["kernel/target.patch"]\n'
            b"copies = [\n"
            b'  { source = "kernel/target-copy.c", destination = "target-copy.c" },\n'
            b'  { source = "bootstrap/referenced.h", destination = "referenced.h" },\n'
            b"]\n"
            b"appends = [\n"
            b'  { source = "kernel/target-append", destination = "Makefile" },\n'
            b"]\n"
        )
        platform_manifest = (
            b"[linux]\n"
            b'patches = ["platforms/demo/kernel/platform.patch"]\n'
            b"copies = [\n"
            b'  { source = "platforms/demo/kernel/platform-copy.c", '
            b'destination = "platform-copy.c" },\n'
            b"]\n"
            b"appends = [\n"
            b'  { source = "platforms/demo/kernel/platform-append", destination = "Makefile" },\n'
            b"]\n"
        )
        inputs = {
            "scripts/fplinux_cli/quality/kernel/analysis.py": b"checker\n",
            "targets/demo/target.toml": target_manifest,
            "platforms/demo/platform.toml": platform_manifest,
            "targets/demo/kernel/target.patch": b"target patch\n",
            "targets/demo/kernel/target-copy.c": b"int target;\n",
            "targets/demo/bootstrap/referenced.h": b"#define DEMO 1\n",
            "targets/demo/kernel/target-append": b"obj-y += demo.o\n",
            "platforms/demo/kernel/platform.patch": b"platform patch\n",
            "platforms/demo/kernel/platform-copy.c": b"int platform;\n",
            "platforms/demo/kernel/platform-append": b"obj-y += platform.o\n",
            "targets/demo/bootstrap/main.c": b"int main;\n",
        }

        def recipe(contents: dict[str, bytes]) -> CheckReceiptRecipe:
            snapshot = WorkspaceSnapshot(
                tuple(WorkspaceFile(path, data, 0o644) for path, data in contents.items()),
                "a" * 64,
            )
            return check_scope_receipt_recipe(
                "kernel",
                check_scope_closure_digest("kernel", snapshot),
                image_generation="c" * 64,
                orchestration_recipe="d" * 64,
            )

        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary)
            publish_success_receipt(cache, recipe(inputs))
            for path in (
                "targets/demo/kernel/target.patch",
                "targets/demo/kernel/target-copy.c",
                "targets/demo/bootstrap/referenced.h",
                "targets/demo/kernel/target-append",
                "platforms/demo/kernel/platform.patch",
                "platforms/demo/kernel/platform-copy.c",
                "platforms/demo/kernel/platform-append",
            ):
                with self.subTest(projected=path):
                    changed = {**inputs, path: inputs[path] + b"changed\n"}
                    self.assertFalse(receipt_matches(cache, recipe(changed)))
            bootstrap_only = {**inputs, "targets/demo/bootstrap/main.c": b"int changed;\n"}
            self.assertTrue(receipt_matches(cache, recipe(bootstrap_only)))

    def test_global_kernel_receipt_tracks_shared_and_board_configs(self) -> None:
        """Relevant config changes miss while the other boot mode remains unrelated."""
        inputs = {
            "targets/phone/target.toml": (
                b'platform = "demo"\n[linux]\nconfig_fragment = "kernel/config.fragment"\n'
            ),
            "platforms/demo/platform.toml": (
                b'[linux]\ndefconfig = "platforms/demo/kernel/defconfig"\n'
            ),
            "platforms/demo/kernel/defconfig": b"CONFIG_SHARED=y\n",
            "targets/phone/kernel/config.fragment": b"CONFIG_BOARD=y\n",
            "scripts/fplinux_cli/build/storage/layout.py": b'root_policy = "ram"\n',
            "profiles/default/profile.toml": b'[linux.root]\nkind = "initramfs"\n',
            "profiles/microsd-uboot/profile.toml": b'[linux.root]\nkind = "external"\n',
            "targets/phone/bootstrap/main.c": b"int bootstrap;\n",
        }

        def receipt(contents: dict[str, bytes], profile: str | None) -> CheckReceiptRecipe:
            snapshot = WorkspaceSnapshot(
                tuple(WorkspaceFile(path, data, 0o644) for path, data in contents.items()),
                "a" * 64,
            )
            return check_scope_receipt_recipe(
                "kernel",
                check_scope_closure_digest("kernel", snapshot, profile=profile),
                image_generation="b" * 64,
                orchestration_recipe="c" * 64,
                profile=profile,
            )

        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary)
            for profile in (None, "microsd-uboot"):
                with self.subTest(profile=profile):
                    original = receipt(inputs, profile)
                    publish_success_receipt(cache, original)
                    self.assertTrue(receipt_matches(cache, receipt(inputs, profile)))
                    for path in (
                        "platforms/demo/kernel/defconfig",
                        "targets/phone/kernel/config.fragment",
                        "scripts/fplinux_cli/build/storage/layout.py",
                    ):
                        changed = {**inputs, path: b"CONFIG_CHANGED=y\n"}
                        self.assertFalse(receipt_matches(cache, receipt(changed, profile)))
                    unrelated = {**inputs, "targets/phone/bootstrap/main.c": b"int changed;\n"}
                    self.assertTrue(receipt_matches(cache, receipt(unrelated, profile)))
                    other_profile = "default" if profile else "microsd-uboot"
                    other_changed = {
                        **inputs,
                        f"profiles/{other_profile}/profile.toml": b"changed\n",
                    }
                    self.assertTrue(receipt_matches(cache, receipt(other_changed, profile)))
                    selected_changed = {
                        **inputs,
                        f"profiles/{profile or 'default'}/profile.toml": b"changed\n",
                    }
                    self.assertFalse(receipt_matches(cache, receipt(selected_changed, profile)))

    def test_editorconfig_change_updates_metadata_closure(self) -> None:
        """Track Prettier's repository EditorConfig as metadata input."""
        first = WorkspaceSnapshot(
            (
                WorkspaceFile("scripts/check.py", b"checker\n", 0o755),
                WorkspaceFile(".editorconfig", b"indent_size = 4\n", 0o644),
            ),
            "a" * 64,
        )
        second = WorkspaceSnapshot(
            (
                first.files[0],
                WorkspaceFile(".editorconfig", b"indent_size = 2\n", 0o644),
            ),
            "b" * 64,
        )
        self.assertNotEqual(
            check_scope_closure_digest("metadata", first),
            check_scope_closure_digest("metadata", second),
        )

    def test_executable_prettier_config_includes_local_helpers(self) -> None:
        """Imported local helpers are causal inputs to an executable Prettier config."""
        common = (
            WorkspaceFile("scripts/check.py", b"checker\n", 0o755),
            WorkspaceFile("prettier.config.mjs", b'import "./helper.mjs";\n', 0o644),
        )
        first = WorkspaceSnapshot(
            (*common, WorkspaceFile("helper.mjs", b"first\n", 0o644)),
            "a" * 64,
        )
        second = WorkspaceSnapshot(
            (*common, WorkspaceFile("helper.mjs", b"second\n", 0o644)),
            "b" * 64,
        )
        self.assertNotEqual(
            check_scope_closure_digest("metadata", first),
            check_scope_closure_digest("metadata", second),
        )

    def test_c_scope_tracks_manifest_source_bootstrap_and_quoted_header(self) -> None:
        """Mirror dynamic userspace C discovery without including orphan kernel C."""
        manifest = (
            b"[host]\n"
            b'runtime_tools = { console = "tool" }\n'
            b"[[host.tools]]\n"
            b'type = "cc-libusb"\n'
            b'name = "tool"\n'
            b'source = "platforms/demo/kernel/tool.c"\n'
            b"self_test = false\n"
        )
        common = (
            WorkspaceFile("scripts/check.py", b"checker\n", 0o755),
            WorkspaceFile("platforms/demo/platform.toml", manifest, 0o644),
            WorkspaceFile(
                "platforms/demo/kernel/tool.c",
                b'#include "tool.h"\nint tool;\n',
                0o644,
            ),
            WorkspaceFile("platforms/demo/kernel/tool.h", b"#define TOOL 1\n", 0o644),
            WorkspaceFile("targets/demo/kernel/orphan.c", b"int orphan;\n", 0o644),
            WorkspaceFile("targets/demo/kernel/bootstrap/start.c", b"int start;\n", 0o644),
        )
        first = WorkspaceSnapshot(common, "a" * 64)
        header_changed = WorkspaceSnapshot(
            (*common[:3], WorkspaceFile(common[3].path, b"#define TOOL 2\n", 0o644), *common[4:]),
            "b" * 64,
        )
        orphan_changed = WorkspaceSnapshot(
            (*common[:4], WorkspaceFile(common[4].path, b"int changed;\n", 0o644), common[5]),
            "c" * 64,
        )
        bootstrap_changed = WorkspaceSnapshot(
            (*common[:5], WorkspaceFile(common[5].path, b"int changed;\n", 0o644)),
            "d" * 64,
        )
        self.assertNotEqual(
            check_scope_closure_digest("c", first),
            check_scope_closure_digest("c", header_changed),
        )
        self.assertEqual(
            check_scope_closure_digest("c", first),
            check_scope_closure_digest("c", orphan_changed),
        )
        self.assertNotEqual(
            check_scope_closure_digest("c", first),
            check_scope_closure_digest("c", bootstrap_changed),
        )

    def test_aport_c_and_header_invalidate_c_scope_without_runtime_selection(self) -> None:
        """Track every C/H input from an aport that may not be in the rootfs."""
        first = WorkspaceSnapshot(
            (
                WorkspaceFile("scripts/check.py", b"checker\n", 0o755),
                WorkspaceFile(
                    "alpine/aports/local-only/local-only.c", b"int local_only;\n", 0o644
                ),
                WorkspaceFile(
                    "alpine/aports/local-only/local-only.h", b"#define LOCAL_ONLY 1\n", 0o644
                ),
            ),
            "a" * 64,
        )
        source_changed = WorkspaceSnapshot(
            (
                first.files[0],
                WorkspaceFile("alpine/aports/local-only/local-only.c", b"int changed;\n", 0o644),
                first.files[2],
            ),
            "b" * 64,
        )
        header_changed = WorkspaceSnapshot(
            (
                first.files[0],
                first.files[1],
                WorkspaceFile(
                    "alpine/aports/local-only/local-only.h", b"#define LOCAL_ONLY 2\n", 0o644
                ),
            ),
            "c" * 64,
        )
        self.assertNotEqual(
            check_scope_closure_digest("c", first),
            check_scope_closure_digest("c", source_changed),
        )
        self.assertNotEqual(
            check_scope_closure_digest("c", first),
            check_scope_closure_digest("c", header_changed),
        )

    def test_alpine_scope_tracks_each_present_apkbuild(self) -> None:
        """Track every aport and both package-selection manifest layers."""
        first = WorkspaceSnapshot(
            (
                WorkspaceFile("scripts/check.py", b"checker\n", 0o755),
                WorkspaceFile("alpine.lock.toml", b"lock\n", 0o644),
                WorkspaceFile("alpine/abuild.conf", b"abuild\n", 0o644),
                WorkspaceFile("alpine/aports/local-only/APKBUILD", b"first\n", 0o644),
                WorkspaceFile("platforms/demo/platform.toml", b"platform packages\n", 0o644),
                WorkspaceFile("targets/demo/target.toml", b"target packages\n", 0o644),
            ),
            "a" * 64,
        )
        changed = WorkspaceSnapshot(
            (
                *first.files[:3],
                WorkspaceFile(first.files[3].path, b"second\n", 0o644),
                *first.files[4:],
            ),
            "b" * 64,
        )
        self.assertNotEqual(
            check_scope_closure_digest("alpine", first),
            check_scope_closure_digest("alpine", changed),
        )
        for index in (4, 5):
            files = list(first.files)
            files[index] = WorkspaceFile(files[index].path, b"changed packages\n", 0o644)
            with self.subTest(path=files[index].path):
                self.assertNotEqual(
                    check_scope_closure_digest("alpine", first),
                    check_scope_closure_digest(
                        "alpine", WorkspaceSnapshot(tuple(files), "c" * 64)
                    ),
                )

    def test_shell_scope_tracks_extensionless_and_openrc_sources(self) -> None:
        """Track shell sources matching the checker, including OpenRC init scripts."""
        base = WorkspaceSnapshot(
            (
                WorkspaceFile("scripts/check.py", b"checker\n", 0o755),
                WorkspaceFile("tool", b"  #!/bin/sh  \necho ok\n", 0o755),
                WorkspaceFile("service.initd", b"#!/sbin/openrc-run\ncommand=/bin/true\n", 0o755),
                WorkspaceFile("helper.inc", b"first\n", 0o644),
            ),
            "a" * 64,
        )
        changed_tool = WorkspaceSnapshot(
            (
                base.files[0],
                WorkspaceFile("tool", b"  #!/bin/sh  \necho changed\n", 0o755),
                base.files[2],
            ),
            "b" * 64,
        )
        self.assertNotEqual(
            check_scope_closure_digest("shell", base),
            check_scope_closure_digest("shell", changed_tool),
        )
        changed_initd = WorkspaceSnapshot(
            (
                base.files[0],
                base.files[1],
                WorkspaceFile("service.initd", b"#!/sbin/openrc-run\ncommand=/bin/false\n", 0o755),
                base.files[3],
            ),
            "c" * 64,
        )
        image_generation = "e" * 64
        first_recipe = check_scope_receipt_recipe(
            "shell",
            check_scope_closure_digest("shell", base),
            image_generation=image_generation,
            orchestration_recipe="f" * 64,
        )
        initd_recipe = check_scope_receipt_recipe(
            "shell",
            check_scope_closure_digest("shell", changed_initd),
            image_generation=image_generation,
            orchestration_recipe="f" * 64,
        )
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary)
            publish_success_receipt(cache, first_recipe)
            self.assertTrue(receipt_matches(cache, first_recipe))
            self.assertFalse(receipt_matches(cache, initd_recipe))
        external = WorkspaceSnapshot(
            (*base.files, WorkspaceFile(".shellcheckrc", b"external-sources=true\n", 0o644)),
            "g" * 64,
        )
        external_changed = WorkspaceSnapshot(
            (
                external.files[0],
                external.files[1],
                external.files[2],
                WorkspaceFile("helper.inc", b"second\n", 0o644),
                external.files[4],
            ),
            "h" * 64,
        )
        self.assertNotEqual(
            check_scope_closure_digest("shell", external),
            check_scope_closure_digest("shell", external_changed),
        )

    def test_configuration_receipts_follow_the_files_checked_by_each_scope(self) -> None:
        """Relevant edits miss while unsupported neighbors and npm's lock remain hits."""
        cases = (
            (
                "metadata",
                {
                    "commitlint.config.mjs": b"export default {};\n",
                    ".github/workflows/demo.yml": b"name: Demo\n",
                    "platforms/demo/target-template/target.toml.in": b'name = "@DEVICE@"\n',
                    ".vale.ini": b"[*.md]\nBasedOnStyles = Vale\n",
                },
                {"other.mjs": b"export default {};\n", "other.JSON": b"{}\n"},
            ),
            (
                "shell",
                {
                    "alpine/abuild.conf": b"CFLAGS=-Os\n",
                },
                {"other.conf": b"setting=value\n"},
            ),
        )

        def recipe(scope: str, contents: dict[str, bytes]) -> CheckReceiptRecipe:
            snapshot = WorkspaceSnapshot(
                tuple(WorkspaceFile(path, data, 0o644) for path, data in contents.items()),
                "a" * 64,
            )
            return check_scope_receipt_recipe(
                scope,
                check_scope_closure_digest(scope, snapshot),
                image_generation="b" * 64,
                orchestration_recipe="c" * 64,
            )

        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary)
            for scope, checked, unsupported in cases:
                unrelated = {**unsupported, "package-lock.json": b"{}\n"}
                contents = {**checked, **unrelated}
                publish_success_receipt(cache, recipe(scope, contents))
                self.assertTrue(receipt_matches(cache, recipe(scope, contents)))
                for path in checked:
                    with self.subTest(scope=scope, causal=path):
                        changed = {**contents, path: contents[path] + b"\n"}
                        self.assertFalse(receipt_matches(cache, recipe(scope, changed)))
                for path in unrelated:
                    with self.subTest(scope=scope, unrelated=path):
                        changed = {**contents, path: contents[path] + b"\n"}
                        self.assertTrue(receipt_matches(cache, recipe(scope, changed)))


if __name__ == "__main__":
    unittest.main()
