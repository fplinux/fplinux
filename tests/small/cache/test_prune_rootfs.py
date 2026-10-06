# SPDX-License-Identifier: GPL-2.0-only
"""Rootfs prune selection and automatic generation cleanup."""

from __future__ import annotations

from typing import TYPE_CHECKING
from unittest import mock

import fplinux_cli.alpine.recipes as alpine_recipes
import fplinux_cli.alpine.signing as alpine_signing
import fplinux_cli.cache.prune.alpine as prune_alpine
import fplinux_cli.cache.prune.operations as prune_operations
import fplinux_cli.device_data.inputs as firmware_inputs
from fplinux_cli.cache.prune.operations import apply_prune, plan_prune
from fplinux_cli.environment.image_state import ImageState, publish_image_state
from fplinux_cli.environment.images import container_artifact_recipe_digest

from tests.small.cache.prune_fixtures import patch_prune_discovery

if TYPE_CHECKING:
    from pathlib import Path


class RootfsPruneTests:
    """Exercise prune planning and application on isolated cache trees."""

    @staticmethod
    def test_superseded_rootfs_is_a_candidate_and_all_current_are_protected(
        tmp_path: Path,
    ) -> None:
        """Every current target package selection protects its rootfs recipe."""
        cache = tmp_path / ".cache"
        public_key = alpine_signing.signing_public_key(cache)
        public_key.parent.mkdir(parents=True)
        public_key.write_bytes(b"public-key\n")
        publish_image_state(cache, ImageState("a" * 64, "b" * 64, "b" * 64))
        first_packages = ("package-a",)
        second_packages = ("package-a", "package-b")
        first_recipe = "1" * 64
        second_recipe = "2" * 64
        current = {first_recipe, second_recipe}
        stale = cache / "rootfs" / ("0" * 64)
        stale.mkdir(parents=True)
        (stale / "rootfs.cpio").write_bytes(b"stale")
        for recipe in current:
            (cache / "rootfs" / recipe).mkdir(parents=True)

        target_configs = {
            "first": {
                "platform": "platform-a",
                "linux": {"root": {"kind": "initramfs"}},
                "device_data": {"groups": {}},
            },
            "second": {
                "platform": "platform-b",
                "linux": {"root": {"kind": "initramfs"}},
                "device_data": {"groups": {}},
                "display_brightness": {
                    "backlight": "second-backlight",
                    "levels": [0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20],
                },
            },
        }
        platform_configs: dict[str, dict[str, object]] = {
            "platform-a": {},
            "platform-b": {},
        }
        package_selections = {
            "platform-a": first_packages,
            "platform-b": second_packages,
        }

        def rootfs_recipe(
            _image_recipe: str,
            _signing_key: str,
            packages: tuple[str, ...],
            *,
            firmware_inputs: tuple[firmware_inputs.FirmwareInput, ...],
            display_brightness: dict[str, object] | None,
            root_kind: str,
        ) -> str:
            assert (root_kind) == ("initramfs")
            assert (firmware_inputs) == (())
            assert (display_brightness) == (
                target_configs["first" if packages == first_packages else "second"].get(
                    "display_brightness"
                )
            )
            return {
                first_packages: first_recipe,
                second_packages: second_recipe,
            }[packages]

        with (
            patch_prune_discovery("discover_targets", return_value=("first", "second")),
            patch_prune_discovery("discover_profiles", return_value=()),
            mock.patch.object(
                prune_alpine,
                "load_target",
                side_effect=lambda target: target_configs[target],
            ),
            mock.patch.object(
                prune_alpine,
                "load_platform",
                side_effect=lambda platform: platform_configs[platform],
            ),
            mock.patch.object(
                prune_alpine,
                "selected_packages",
                side_effect=lambda _platform, target: package_selections[target["platform"]],
            ),
            mock.patch.object(
                prune_alpine,
                "alpine_rootfs_recipe",
                side_effect=rootfs_recipe,
            ),
            mock.patch.object(
                prune_alpine,
                "container_image_recipe_digest",
                return_value="a" * 64,
            ),
        ):
            plan = plan_prune(cache)
            decisions = {entry.path: entry.action for entry in plan.entries}

            assert (decisions[f"rootfs/{'0' * 64}"]) == ("candidate")
            assert ({path for path, action in decisions.items() if action == "protected"}) == (
                {f"rootfs/{recipe}" for recipe in current}
            )
            apply_prune(cache)
        assert not (stale.exists())
        assert all((cache / "rootfs" / recipe).exists() for recipe in current)

    @staticmethod
    def test_missing_signing_key_protects_existing_rootfs(tmp_path: Path) -> None:
        """Never prune rootfs generations when their package-signing input is unknown."""
        cache = tmp_path / ".cache"
        publish_image_state(cache, ImageState("a" * 64, "b" * 64, "b" * 64))
        existing = cache / "rootfs" / ("1" * 64)
        existing.mkdir(parents=True)
        with (
            patch_prune_discovery("discover_targets", return_value=("phone",)),
            patch_prune_discovery("discover_profiles", return_value=()),
            mock.patch.object(
                prune_alpine,
                "load_target",
                return_value={
                    "platform": "platform",
                    "linux": {"root": {"kind": "initramfs"}},
                    "device_data": {"groups": {}},
                },
            ),
            mock.patch.object(prune_alpine, "load_platform", return_value={}),
            mock.patch.object(prune_alpine, "selected_packages", return_value=()),
            mock.patch.object(prune_alpine, "alpine_rootfs_recipe", return_value="0" * 64),
            mock.patch.object(
                prune_alpine, "container_image_recipe_digest", return_value="a" * 64
            ),
        ):
            plan = plan_prune(cache)
        assert (len(plan.entries)) == (1)
        assert (plan.entries[0].action) == ("protected")
        assert ("rootfs recipes") in (plan.entries[0].reason)

    @staticmethod
    def test_partial_device_data_group_protects_existing_rootfs(tmp_path: Path) -> None:
        """A selected but incomplete private group makes the current recipe unknown."""
        cache = tmp_path / ".cache"
        public_key = alpine_signing.signing_public_key(cache)
        public_key.parent.mkdir(parents=True)
        public_key.write_bytes(b"public-key\n")
        publish_image_state(cache, ImageState("a" * 64, "b" * 64, "b" * 64))
        existing = cache / "rootfs" / ("1" * 64)
        existing.mkdir(parents=True)
        generation = cache / "device-data/phone/generations/generation-test"
        bluetooth = generation / "groups/bluetooth"
        bluetooth.mkdir(parents=True)
        (bluetooth / "present.bin").write_bytes(b"present!")
        current = cache / "device-data/phone/current"
        current.write_text("generation-test\n", encoding="ascii")
        target_config = {
            "platform": "platform",
            "linux": {"root": {"kind": "initramfs"}},
            "device_data": {
                "groups": {
                    "bluetooth": [
                        {
                            "source": "present.bin",
                            "destination": "chip/present.bin",
                            "size": 8,
                        },
                        {
                            "source": "missing.bin",
                            "destination": "chip/missing.bin",
                            "size": 8,
                        },
                    ]
                }
            },
        }

        with (
            patch_prune_discovery("discover_targets", return_value=("phone",)),
            patch_prune_discovery("discover_profiles", return_value=()),
            mock.patch.object(prune_alpine, "load_target", return_value=target_config),
            mock.patch.object(prune_alpine, "load_platform", return_value={}),
            mock.patch.object(prune_alpine, "selected_packages", return_value=()),
            mock.patch.object(prune_alpine, "alpine_rootfs_recipe", return_value="0" * 64),
            mock.patch.object(
                prune_alpine,
                "container_image_recipe_digest",
                return_value="a" * 64,
            ),
        ):
            plan = plan_prune(cache)

        assert (len(plan.entries)) == (1)
        assert (plan.entries[0].action) == ("protected")
        assert ("rootfs recipes") in (plan.entries[0].reason)
        assert existing.exists()

    @staticmethod
    def test_current_rootfs_recipes_include_each_declared_profile(tmp_path: Path) -> None:
        """One profile-only rootfs remains protected even when default differs."""
        cache = tmp_path / ".cache"
        public_key = alpine_signing.signing_public_key(cache)
        public_key.parent.mkdir(parents=True)
        public_key.write_bytes(b"public-key\n")
        publish_image_state(cache, ImageState("a" * 64, "b" * 64, "b" * 64))
        default_recipe = "3" * 64
        profile_recipe = "4" * 64
        for recipe in (default_recipe, profile_recipe):
            (cache / "rootfs" / recipe).mkdir(parents=True)
        device_data = cache / "device-data/phone"
        firmware_directory = device_data / "generations/generation-test/groups/bluetooth"
        firmware_directory.mkdir(parents=True)
        (firmware_directory / "controller.bin").write_bytes(b"firmware")
        (device_data / "current").write_text("generation-test\n", encoding="ascii")

        def target_config(_target: str, profile: str | None = None) -> dict[str, object]:
            firmware = (
                [
                    {
                        "source": "controller.bin",
                        "destination": "chip/controller.bin",
                        "size": 8,
                    }
                ]
                if profile == "host"
                else []
            )
            return {
                "platform": "platform",
                "profile": profile,
                "linux": {"root": {"kind": "external" if profile == "host" else "initramfs"}},
                "device_data": {
                    "groups": (
                        {
                            "bluetooth": firmware,
                        }
                        if firmware
                        else {}
                    )
                },
            }

        def selected_packages(
            _platform: dict[str, object], config: dict[str, object]
        ) -> tuple[str, ...]:
            return ("package-host",) if config["profile"] == "host" else ("package-base",)

        def rootfs_recipe(
            _image: str,
            _key: str,
            packages: tuple[str, ...],
            *,
            firmware_inputs: tuple[firmware_inputs.FirmwareInput, ...],
            display_brightness: dict[str, object] | None,
            root_kind: str,
        ) -> str:
            assert (display_brightness) is None
            assert (root_kind) == ("external" if packages == ("package-host",) else "initramfs")
            recipes: dict[tuple[str, ...], str] = {
                ("package-base",): default_recipe,
                ("package-host",): profile_recipe,
            }
            if packages == ("package-base",):
                assert (firmware_inputs) == (())
            else:
                assert (len(firmware_inputs)) == (1)
                assert (firmware_inputs[0].contents) == (b"firmware")
            return recipes[packages]

        with (
            patch_prune_discovery("discover_targets", return_value=("phone",)),
            patch_prune_discovery("discover_profiles", return_value=("host",)),
            mock.patch.object(prune_alpine, "load_target", side_effect=target_config),
            mock.patch.object(prune_alpine, "load_platform", return_value={}),
            mock.patch.object(
                prune_alpine,
                "selected_packages",
                side_effect=selected_packages,
            ),
            mock.patch.object(prune_alpine, "alpine_rootfs_recipe", side_effect=rootfs_recipe),
            mock.patch.object(
                prune_alpine,
                "container_image_recipe_digest",
                return_value="a" * 64,
            ),
        ):
            plan = plan_prune(cache)

        decisions = {entry.path: entry.action for entry in plan.entries}
        assert (decisions[f"rootfs/{default_recipe}"]) == ("protected")
        assert (decisions[f"rootfs/{profile_recipe}"]) == ("protected")

    @staticmethod
    def test_automatic_rootfs_cleanup_removes_only_superseded_generations(tmp_path: Path) -> None:
        """Targeted cleanup bounds rootfs state without invoking broad prune."""
        cache = tmp_path / ".cache"
        public_key = alpine_signing.signing_public_key(cache)
        public_key.parent.mkdir(parents=True)
        public_key.write_bytes(b"public-key\n")
        publish_image_state(cache, ImageState("a" * 64, "b" * 64, "b" * 64))
        current = "5" * 64
        stale = "6" * 64
        for recipe in (current, stale):
            (cache / "rootfs" / recipe).mkdir(parents=True)

        with (
            patch_prune_discovery("discover_targets", return_value=("phone",)),
            patch_prune_discovery("discover_profiles", return_value=()),
            mock.patch.object(
                prune_alpine,
                "load_target",
                return_value={
                    "platform": "platform",
                    "linux": {"root": {"kind": "initramfs"}},
                    "device_data": {"groups": {}},
                },
            ),
            mock.patch.object(prune_alpine, "load_platform", return_value={}),
            mock.patch.object(
                prune_alpine,
                "selected_packages",
                return_value=("package",),
            ),
            mock.patch.object(prune_alpine, "alpine_rootfs_recipe", return_value=current),
            mock.patch.object(
                prune_alpine,
                "container_image_recipe_digest",
                return_value="a" * 64,
            ),
        ):
            removed = prune_operations.discard_obsolete_rootfs(cache)

        assert (removed) == ((f"rootfs/{stale}",))
        assert (cache / "rootfs" / current).exists()
        assert not ((cache / "rootfs" / stale).exists())

    @staticmethod
    def test_automatic_rootfs_cleanup_retains_bluetooth_and_fm_recipe(tmp_path: Path) -> None:
        """Cleanup retains the built rootfs and drops recipes with wrong firmware groups."""
        cache = tmp_path / ".cache"
        public_key = alpine_signing.signing_public_key(cache)
        public_key.parent.mkdir(parents=True)
        public_key.write_bytes(b"public-key\n")
        publish_image_state(cache, ImageState("a" * 64, "b" * 64, "b" * 64))
        groups = {
            "bluetooth": [
                {"source": "bluetooth.bin", "destination": "chip/bluetooth.bin", "size": 9}
            ],
            "fm-radio": [{"source": "radio.bin", "destination": "chip/radio.bin", "size": 5}],
            "audio-profile": [{"source": "audio.bin", "destination": "chip/audio.bin", "size": 5}],
        }
        device_data = cache / "device-data/phone"
        for group, filename, contents in (
            ("bluetooth", "bluetooth.bin", b"bluetooth"),
            ("fm-radio", "radio.bin", b"radio"),
            ("audio-profile", "audio.bin", b"audio"),
        ):
            directory = device_data / "generations/generation-test/groups" / group
            directory.mkdir(parents=True)
            (directory / filename).write_bytes(contents)
        (device_data / "current").write_text("generation-test\n", encoding="ascii")
        captured = firmware_inputs.capture_external_device_data("phone", groups, cache)
        image_recipe = container_artifact_recipe_digest("a" * 64, "b" * 64)
        signing_key = alpine_signing.signing_key_identity(cache)

        def recipe_for(*selected_groups: str) -> str:
            selected = tuple(item for group in selected_groups for item in captured[group])
            return alpine_recipes.alpine_rootfs_recipe(
                image_recipe, signing_key, (), firmware_inputs=selected
            )

        current = recipe_for("bluetooth", "fm-radio")
        bluetooth_only = recipe_for("bluetooth")
        with_audio_profile = recipe_for("bluetooth", "fm-radio", "audio-profile")
        for recipe in (current, bluetooth_only, with_audio_profile):
            directory = cache / "rootfs" / recipe
            directory.mkdir(parents=True)
            (directory / "rootfs.cpio").write_bytes(b"rootfs")

        with (
            patch_prune_discovery("discover_targets", return_value=("phone",)),
            patch_prune_discovery("discover_profiles", return_value=()),
            mock.patch.object(
                prune_alpine,
                "load_target",
                return_value={
                    "platform": "platform",
                    "linux": {"root": {"kind": "initramfs"}},
                    "device_data": {"groups": groups},
                },
            ),
            mock.patch.object(prune_alpine, "load_platform", return_value={}),
            mock.patch.object(prune_alpine, "selected_packages", return_value=()),
            mock.patch.object(
                prune_alpine, "container_image_recipe_digest", return_value="a" * 64
            ),
        ):
            removed = prune_operations.discard_obsolete_rootfs(cache)

        assert (set(removed)) == ({f"rootfs/{bluetooth_only}", f"rootfs/{with_audio_profile}"})
        assert (cache / "rootfs" / current / "rootfs.cpio").is_file()
        assert not ((cache / "rootfs" / bluetooth_only).exists())
        assert not ((cache / "rootfs" / with_audio_profile).exists())

    @staticmethod
    def test_automatic_rootfs_cleanup_leaves_a_symlinked_entry_untouched(tmp_path: Path) -> None:
        """Automatic retention never follows or removes an unsafe rootfs cache link."""
        cache = tmp_path / ".cache"
        public_key = alpine_signing.signing_public_key(cache)
        public_key.parent.mkdir(parents=True)
        public_key.write_bytes(b"public-key\n")
        publish_image_state(cache, ImageState("a" * 64, "b" * 64, "b" * 64))
        stale = cache / "rootfs" / ("6" * 64)
        stale.mkdir(parents=True)
        outside = tmp_path / "outside"
        outside.mkdir()
        sentinel = outside / "sentinel"
        sentinel.write_bytes(b"keep")
        linked = cache / "rootfs" / ("7" * 64)
        linked.symlink_to(outside, target_is_directory=True)

        with (
            patch_prune_discovery("discover_targets", return_value=("phone",)),
            patch_prune_discovery("discover_profiles", return_value=()),
            mock.patch.object(
                prune_alpine,
                "load_target",
                return_value={
                    "platform": "platform",
                    "linux": {"root": {"kind": "initramfs"}},
                    "device_data": {"groups": {}},
                },
            ),
            mock.patch.object(prune_alpine, "load_platform", return_value={}),
            mock.patch.object(prune_alpine, "selected_packages", return_value=()),
            mock.patch.object(prune_alpine, "alpine_rootfs_recipe", return_value="8" * 64),
            mock.patch.object(
                prune_alpine, "container_image_recipe_digest", return_value="a" * 64
            ),
        ):
            removed = prune_operations.discard_obsolete_rootfs(cache)

        assert (removed) == ((f"rootfs/{stale.name}",))
        assert not (stale.exists())
        assert linked.is_symlink()
        assert (sentinel.read_bytes()) == (b"keep")

    @staticmethod
    def test_config_or_selection_failure_protects_existing_rootfs(tmp_path: Path) -> None:
        """An incomplete target inventory never makes an existing rootfs disposable."""
        cache = tmp_path / ".cache"
        public_key = alpine_signing.signing_public_key(cache)
        public_key.parent.mkdir(parents=True)
        public_key.write_bytes(b"public-key\n")
        publish_image_state(cache, ImageState("a" * 64, "b" * 64, "b" * 64))
        existing = cache / "rootfs" / ("2" * 64)
        existing.mkdir(parents=True)

        with (
            patch_prune_discovery(
                "discover_targets", side_effect=SystemExit("bad target manifest")
            ),
            mock.patch.object(
                prune_alpine, "container_image_recipe_digest", return_value="a" * 64
            ),
        ):
            plan = plan_prune(cache)

        assert (len(plan.entries)) == (1)
        assert (plan.entries[0].action) == ("protected")
        assert ("rootfs recipes") in (plan.entries[0].reason)
