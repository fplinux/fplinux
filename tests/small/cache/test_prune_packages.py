# SPDX-License-Identifier: GPL-2.0-only
"""Package cache retention and obsolete aport cleanup."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path
from unittest import mock

import fplinux_cli.alpine.packages as alpine_packages
import fplinux_cli.alpine.registration as alpine_registration
import fplinux_cli.cache.prune.alpine as prune_alpine
import fplinux_cli.cache.prune.operations as prune_operations
from fplinux_cli.cache.prune.operations import apply_prune, plan_prune

from tests.small.cache.prune_fixtures import patch_prune_discovery


class PackagePruneTests(unittest.TestCase):
    """Exercise prune planning and application on isolated cache trees."""

    def test_profile_package_replacement_prunes_only_the_obsolete_aport_slot(self) -> None:
        """Replacing a profile package declaration leaves its former cache slot disposable."""
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary) / ".cache"
            apks = cache / alpine_packages.PACKAGE_CACHE_DIRECTORY
            current = {
                "fplinux-base",
                "fplinux-bundle-base",
                "fplinux-profile-y",
                "fplinux-bundle-host",
            }
            obsolete = "fplinux-profile-x"
            for package in (*current, obsolete):
                directory = apks / package
                directory.mkdir(parents=True)
                (directory / "payload").write_text(package + "\n")

            external = Path(temporary) / "external"
            external.mkdir()
            link = apks / "fplinux-link"
            link.symlink_to(external, target_is_directory=True)
            regular = apks / "fplinux-file"
            regular.write_text("keep\n")

            def load_target(_target: str, profile: str | None = None) -> dict[str, object]:
                return {"platform": "platform", "profile": profile}

            def selected_packages(
                _platform: dict[str, object], config: dict[str, object]
            ) -> tuple[str, ...]:
                return ("fplinux-profile-y",) if config["profile"] == "host" else ("fplinux-base",)

            def bundle_packages(
                _platform: dict[str, object], config: dict[str, object], _rootfs: tuple[str, ...]
            ) -> tuple[str, ...]:
                return (
                    ("fplinux-bundle-host",)
                    if config["profile"] == "host"
                    else ("fplinux-bundle-base",)
                )

            with (
                patch_prune_discovery("discover_targets", return_value=("phone",)),
                patch_prune_discovery("discover_profiles", return_value=("host",)),
                mock.patch.object(prune_alpine, "load_target", side_effect=load_target),
                mock.patch.object(prune_alpine, "load_platform", return_value={}),
                mock.patch.object(
                    prune_alpine,
                    "selected_packages",
                    side_effect=selected_packages,
                ),
                mock.patch.object(
                    prune_alpine,
                    "bundle_packages",
                    side_effect=bundle_packages,
                ),
            ):
                plan = plan_prune(cache)
                result = apply_prune(cache)

            decisions = {entry.path: entry.action for entry in plan.entries}
            self.assertEqual(decisions[f"apks/{obsolete}"], "candidate")
            for package in current:
                self.assertEqual(decisions[f"apks/{package}"], "protected")
            self.assertEqual(decisions["apks/fplinux-link"], "protected")
            self.assertEqual(decisions["apks/fplinux-file"], "protected")
            self.assertEqual(result.removed, (f"apks/{obsolete}",))
            self.assertFalse((apks / obsolete).exists())
            self.assertTrue(all((apks / package).is_dir() for package in current))
            self.assertTrue(link.is_symlink())
            self.assertTrue(regular.is_file())

    def test_selected_child_packages_preserve_their_aport_cache_slots(self) -> None:
        """Child selections retain producer output through planned and targeted cleanup."""
        for cleanup in ("prune", "targeted"):
            with self.subTest(cleanup=cleanup), tempfile.TemporaryDirectory() as temporary:
                cache = Path(temporary) / ".cache"
                apks = cache / "apks"
                for package in (
                    "fplinux-font-test",
                    "fplinux-bundle-test",
                    "fplinux-standalone",
                    "fplinux-obsolete",
                ):
                    directory = apks / package
                    directory.mkdir(parents=True)
                    (directory / "payload").write_text(package + "\n")

                # Stub package declarations; execute real pruning on the isolated cache.
                with (
                    patch_prune_discovery("discover_targets", return_value=("phone",)),
                    patch_prune_discovery("discover_profiles", return_value=()),
                    mock.patch.object(
                        prune_alpine, "load_target", return_value={"platform": "platform"}
                    ),
                    mock.patch.object(prune_alpine, "load_platform", return_value={}),
                    mock.patch.object(
                        prune_alpine,
                        "selected_packages",
                        return_value=("fplinux-font-test-12", "fplinux-standalone"),
                    ),
                    mock.patch.object(
                        prune_alpine,
                        "bundle_packages",
                        return_value=("fplinux-bundle-test-data",),
                    ),
                    mock.patch.object(
                        alpine_registration,
                        "SUBPACKAGE_APORTS",
                        {
                            "fplinux-font-test-12": "fplinux-font-test",
                            "fplinux-bundle-test-data": "fplinux-bundle-test",
                        },
                    ),
                ):
                    plan = plan_prune(cache)
                    self.assertEqual(
                        {entry.path: entry.action for entry in plan.entries},
                        {
                            "apks/fplinux-font-test": "protected",
                            "apks/fplinux-bundle-test": "protected",
                            "apks/fplinux-standalone": "protected",
                            "apks/fplinux-obsolete": "candidate",
                        },
                    )
                    removed = (
                        apply_prune(cache).removed
                        if cleanup == "prune"
                        else prune_operations.discard_obsolete_apks(cache)
                    )

                self.assertEqual(removed, ("apks/fplinux-obsolete",))
                self.assertFalse((apks / "fplinux-obsolete").exists())
                self.assertEqual(
                    (apks / "fplinux-font-test/payload").read_text(), "fplinux-font-test\n"
                )
                self.assertEqual(
                    (apks / "fplinux-bundle-test/payload").read_text(), "fplinux-bundle-test\n"
                )
                self.assertEqual(
                    (apks / "fplinux-standalone/payload").read_text(), "fplinux-standalone\n"
                )

    def test_package_config_failure_protects_every_aport_cache_slot(self) -> None:
        """An unreadable target/profile closure cannot make package output disposable."""
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary) / ".cache"
            apks = cache / alpine_packages.PACKAGE_CACHE_DIRECTORY
            for package in ("fplinux-base", "fplinux-obsolete"):
                (apks / package).mkdir(parents=True)

            with patch_prune_discovery(
                "discover_targets", side_effect=SystemExit("bad target manifest")
            ):
                plan = plan_prune(cache)

            self.assertEqual(
                {entry.path: entry.action for entry in plan.entries},
                {
                    "apks/fplinux-base": "protected",
                    "apks/fplinux-obsolete": "protected",
                },
            )
            self.assertTrue(
                all("closure is unavailable" in entry.reason for entry in plan.entries)
            )


if __name__ == "__main__":
    unittest.main()
