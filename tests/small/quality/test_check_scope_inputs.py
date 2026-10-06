# SPDX-License-Identifier: GPL-2.0-only
"""Causal source closures and recipes for selected check scopes."""

from __future__ import annotations

import tempfile
from pathlib import Path

import pytest
from fplinux_cli.quality.inputs import check_scope_closure_digest
from fplinux_cli.quality.receipts import (
    CheckReceiptRecipe,
    check_scope_receipt_recipe,
    publish_success_receipt,
    receipt_matches,
)
from fplinux_cli.workspace.capture import WorkspaceFile, WorkspaceSnapshot


class CheckScopeTests:
    """Keep scope recipes bound to the inputs checked by their consumers."""

    @pytest.mark.parametrize(
        ("scope", "original", "changed"),
        [
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
        ],
        ids=("package-selection", "shared-source-ownership"),
    )
    def test_registration_edits_revoke_affected_check_scope_receipts(
        self, scope: str, original: bytes, changed: bytes
    ) -> None:
        """Ownership and shared-source declarations cannot reuse checks of the old inputs."""

        def receipt(scope: str, registration: bytes) -> CheckReceiptRecipe:
            snapshot = WorkspaceSnapshot(
                (
                    WorkspaceFile(
                        "scripts/fplinux_cli/alpine/registration.py", registration, 0o644
                    ),
                    WorkspaceFile("alpine/aports/fplinux-base/APKBUILD", b"pkgname=base\n", 0o644),
                    WorkspaceFile("alpine/aports/consumer-a/app.c", b"int app;\n", 0o644),
                    WorkspaceFile(
                        "scripts/fplinux_cli/quality/kernel/analysis.py", b"# checker\n", 0o644
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

        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary)
            publish_success_receipt(cache, receipt(scope, original))
            assert receipt_matches(cache, receipt(scope, original))
            assert not (receipt_matches(cache, receipt(scope, changed)))

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
        assert (check_scope_closure_digest("kernel", before)) != (
            check_scope_closure_digest("kernel", after)
        )
        assert (check_scope_closure_digest("kernel", before, build_type="debug")) == (
            check_scope_closure_digest("kernel", after, build_type="debug")
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
        assert (release) == (debug)

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
            assert receipt_matches(cache, first)
            assert not (receipt_matches(cache, generation_changed))
            assert not (receipt_matches(cache, orchestration_changed))

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

        assert (default.profile) is None
        assert (profile.profile) == ("microsd-uboot")
        assert (default.payload()) != (profile.payload())

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
        assert (check_scope_closure_digest("c", first)) == (
            check_scope_closure_digest("c", second)
        )
        assert (check_scope_closure_digest("kernel", first)) == (
            check_scope_closure_digest("kernel", second)
        )
        assert (check_scope_closure_digest("source", first)) != (
            check_scope_closure_digest("source", second)
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
        assert (check_scope_closure_digest("c", first)) == (
            check_scope_closure_digest("c", kernel_changed)
        )
        assert (check_scope_closure_digest("kernel", first)) != (
            check_scope_closure_digest("kernel", kernel_changed)
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

        assert (check_scope_closure_digest("c", first)) != (
            check_scope_closure_digest("c", second)
        )

    @pytest.mark.parametrize(
        "path",
        [
            "targets/demo/kernel/target.patch",
            "targets/demo/kernel/target-copy.c",
            "targets/demo/bootstrap/referenced.h",
            "targets/demo/kernel/target-append",
            "platforms/demo/kernel/platform.patch",
            "platforms/demo/kernel/platform-copy.c",
            "platforms/demo/kernel/platform-append",
        ],
        ids=(
            "target-patch",
            "target-copy",
            "referenced-bootstrap-header",
            "target-append",
            "platform-patch",
            "platform-copy",
            "platform-append",
        ),
    )
    def test_kernel_scope_tracks_manifest_sources_without_whole_bootstrap(self, path: str) -> None:
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
            changed = {**inputs, path: inputs[path] + b"changed\n"}
            assert not (receipt_matches(cache, recipe(changed)))
            bootstrap_only = {**inputs, "targets/demo/bootstrap/main.c": b"int changed;\n"}
            assert receipt_matches(cache, recipe(bootstrap_only))

    @pytest.mark.parametrize("profile", [None, "microsd-uboot"], ids=("default", "microsd-uboot"))
    def test_global_kernel_receipt_tracks_shared_and_board_configs(
        self, profile: str | None
    ) -> None:
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
            original = receipt(inputs, profile)
            publish_success_receipt(cache, original)
            assert receipt_matches(cache, receipt(inputs, profile))
            for path in (
                "platforms/demo/kernel/defconfig",
                "targets/phone/kernel/config.fragment",
                "scripts/fplinux_cli/build/storage/layout.py",
            ):
                changed = {**inputs, path: b"CONFIG_CHANGED=y\n"}
                assert not (receipt_matches(cache, receipt(changed, profile)))
            unrelated = {**inputs, "targets/phone/bootstrap/main.c": b"int changed;\n"}
            assert receipt_matches(cache, receipt(unrelated, profile))
            other_profile = "default" if profile else "microsd-uboot"
            other_changed = {
                **inputs,
                f"profiles/{other_profile}/profile.toml": b"changed\n",
            }
            assert receipt_matches(cache, receipt(other_changed, profile))
            selected_changed = {
                **inputs,
                f"profiles/{profile or 'default'}/profile.toml": b"changed\n",
            }
            assert not (receipt_matches(cache, receipt(selected_changed, profile)))

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
        assert (check_scope_closure_digest("metadata", first)) != (
            check_scope_closure_digest("metadata", second)
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
        assert (check_scope_closure_digest("metadata", first)) != (
            check_scope_closure_digest("metadata", second)
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
        assert (check_scope_closure_digest("c", first)) != (
            check_scope_closure_digest("c", header_changed)
        )
        assert (check_scope_closure_digest("c", first)) == (
            check_scope_closure_digest("c", orphan_changed)
        )
        assert (check_scope_closure_digest("c", first)) != (
            check_scope_closure_digest("c", bootstrap_changed)
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
        assert (check_scope_closure_digest("c", first)) != (
            check_scope_closure_digest("c", source_changed)
        )
        assert (check_scope_closure_digest("c", first)) != (
            check_scope_closure_digest("c", header_changed)
        )

    @pytest.mark.parametrize("index", [4, 5], ids=("platform-packages", "target-packages"))
    def test_alpine_scope_tracks_each_present_apkbuild(self, index: int) -> None:
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
        assert (check_scope_closure_digest("alpine", first)) != (
            check_scope_closure_digest("alpine", changed)
        )
        files = list(first.files)
        files[index] = WorkspaceFile(files[index].path, b"changed packages\n", 0o644)
        assert (check_scope_closure_digest("alpine", first)) != (
            check_scope_closure_digest("alpine", WorkspaceSnapshot(tuple(files), "c" * 64))
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
        assert (check_scope_closure_digest("shell", base)) != (
            check_scope_closure_digest("shell", changed_tool)
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
            assert receipt_matches(cache, first_recipe)
            assert not (receipt_matches(cache, initd_recipe))
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
        assert (check_scope_closure_digest("shell", external)) != (
            check_scope_closure_digest("shell", external_changed)
        )

    @pytest.mark.parametrize(
        ("scope", "path", "expected_hit"),
        [
            ("metadata", "commitlint.config.mjs", False),
            ("metadata", ".github/workflows/demo.yml", False),
            ("metadata", "platforms/demo/target-template/target.toml.in", False),
            ("metadata", ".vale.ini", False),
            ("metadata", "other.mjs", True),
            ("metadata", "other.JSON", True),
            ("metadata", "package-lock.json", True),
            ("shell", "alpine/abuild.conf", False),
            ("shell", "other.conf", True),
            ("shell", "package-lock.json", True),
        ],
        ids=(
            "metadata-commitlint",
            "metadata-workflow",
            "metadata-template",
            "metadata-vale",
            "metadata-unrelated-script",
            "metadata-unsupported-json",
            "metadata-npm-lock",
            "shell-abuild",
            "shell-unrelated-config",
            "shell-npm-lock",
        ),
    )
    def test_configuration_receipts_follow_the_files_checked_by_each_scope(
        self, scope: str, path: str, *, expected_hit: bool
    ) -> None:
        """Relevant edits miss while unsupported neighbors and npm's lock remain hits."""
        inputs_by_scope = {
            "metadata": (
                {
                    "commitlint.config.mjs": b"export default {};\n",
                    ".github/workflows/demo.yml": b"name: Demo\n",
                    "platforms/demo/target-template/target.toml.in": b'name = "@DEVICE@"\n',
                    ".vale.ini": b"[*.md]\nBasedOnStyles = Vale\n",
                },
                {"other.mjs": b"export default {};\n", "other.JSON": b"{}\n"},
            ),
            "shell": (
                {
                    "alpine/abuild.conf": b"CFLAGS=-Os\n",
                },
                {"other.conf": b"setting=value\n"},
            ),
        }

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
            checked, unsupported = inputs_by_scope[scope]
            unrelated = {**unsupported, "package-lock.json": b"{}\n"}
            contents = {**checked, **unrelated}
            publish_success_receipt(cache, recipe(scope, contents))
            assert receipt_matches(cache, recipe(scope, contents))
            changed = {**contents, path: contents[path] + b"\n"}
            assert receipt_matches(cache, recipe(scope, changed)) == expected_hit
