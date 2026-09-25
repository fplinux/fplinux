# SPDX-License-Identifier: GPL-2.0-only
"""Headless target skeletons written to a disposable checkout by the target creator."""

from __future__ import annotations

import contextlib
import io
import shutil
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from fplinux_cli import common, target_new
from fplinux_cli.cli import package
from fplinux_cli.manifests import paths, targets

REPOSITORY = common.ROOT
DISPLAY_KEYS = {"spi_mode", "lcd_id", "backlight_channels", "backlight_level"}


def table_rows(markdown: str, heading: str) -> list[list[str]]:
    """Return the data cells of the first pipe table under one second-level heading."""
    lines = markdown.split(f"\n## {heading}\n", 1)[1].split("\n## ", 1)[0].splitlines()
    table = [line for line in lines if line.startswith("|")]
    return [[cell.strip() for cell in line.strip("|").split("|")] for line in table[2:]]


class NewTargetTests(unittest.TestCase):
    """Create skeletons from the real platform template module, manifests and profiles."""

    def setUp(self) -> None:
        """Provide a checkout with the platform inputs and an empty target directory."""
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        platform = self.root / "platforms/ums9117"
        (platform / "host").mkdir(parents=True)
        shutil.copy2(REPOSITORY / "platforms/ums9117/platform.toml", platform)
        shutil.copy2(REPOSITORY / "platforms/ums9117/host/target_skeleton.py", platform / "host")
        shutil.copytree(
            REPOSITORY / "platforms/ums9117/target-template", platform / "target-template"
        )
        shutil.copytree(REPOSITORY / "profiles", self.root / "profiles")
        self.targets = self.root / "targets"
        self.targets.mkdir()
        root = mock.patch.object(common, "ROOT", self.root)
        root.start()
        self.addCleanup(root.stop)

    def create(
        self,
        name: str,
        *,
        platform: str | None = None,
        brand: str = "HAMMER",
        product: str = "Horizon LTE",
        compatible: str | None = None,
    ) -> str:
        """Create one target and return what the command printed."""
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            target_new.create_target(
                name, platform=platform, brand=brand, product=product, compatible=compatible
            )
        return output.getvalue()

    def add_platform(self, name: str) -> Path:
        """Declare a second platform that has a manifest but no target skeleton module."""
        platform = self.root / "platforms" / name
        platform.mkdir()
        shutil.copy2(REPOSITORY / "platforms/ums9117/platform.toml", platform)
        return platform

    def test_new_target_loads_as_a_headless_ram_only_phone(self) -> None:
        """The manifest boundary accepts the skeleton with only FDL1 and no display."""
        printed = self.create("hammer-horizon-lte")

        config = targets.load_target("hammer-horizon-lte")
        self.assertEqual(
            config["identity"],
            {
                "brand": "HAMMER",
                "product": "Horizon LTE",
                "hardware_codes": [],
                "compatible": "hammer,horizon-lte",
                "display_name": "HAMMER Horizon LTE",
            },
        )
        self.assertEqual(config["nand"], {"raw_device": "/dev/ums9117-nand-raw"})
        self.assertEqual(config["runtime"]["assets"], {"fdl1": "assets/t117_fdl1.bin"})
        self.assertTrue(DISPLAY_KEYS.isdisjoint(config["runtime"]["adapter"]))
        self.assertEqual(config["runtime"]["adapter"]["exec_distance"], 0)
        self.assertEqual(config["device_data"], {"groups": {}})
        self.assertEqual(paths.discover_profiles("hammer-horizon-lte"), ("default",))
        with self.assertRaisesRegex(SystemExit, "does not support profile microsd-uboot"):
            targets.load_target("hammer-horizon-lte", "microsd-uboot")
        release = package.load_release_manifest("hammer-horizon-lte", config)
        self.assertIn("assets/t117_fdl1.bin", release["runtime_files"])

        lines = printed.splitlines()
        self.assertEqual(lines[0], "Created headless target targets/hammer-horizon-lte:")
        self.assertEqual(
            lines[-1],
            "Next: ./fplinux build hammer-horizon-lte && ./fplinux run hammer-horizon-lte",
        )
        created = sorted(
            path.relative_to(self.targets / "hammer-horizon-lte").as_posix()
            for path in (self.targets / "hammer-horizon-lte").rglob("*")
            if path.is_file()
        )
        self.assertEqual(sorted(line.strip() for line in lines[1:-1]), created)

    def test_new_target_readme_claims_no_presence_or_support(self) -> None:
        """Every feature and application stays unestablished until tested on the phone."""
        self.create("hammer-horizon-lte")
        readme = (self.targets / "hammer-horizon-lte/README.md").read_text(encoding="utf-8")

        self.assertTrue(readme.startswith("# HAMMER Horizon LTE\n"))
        features = table_rows(readme, "Features")
        self.assertGreater(len(features), 0)
        for feature, hardware, support, difference in features:
            with self.subTest(feature=feature):
                self.assertIn(hardware, {"Unknown", "N/A"})
                self.assertEqual(support, "Unknown")
                self.assertEqual(difference, "—")
        applications = table_rows(readme, "Applications")
        self.assertGreater(len(applications), 0)
        for application, support, difference in applications:
            with self.subTest(application=application):
                self.assertEqual(support, "Unknown")
                self.assertEqual(difference, "—")

    def test_compatible_defaults_to_the_public_names_unless_given(self) -> None:
        """A derived compatible folds punctuation; an explicit one is kept verbatim."""
        cases = (
            ("derived", "A&B", "Phone 2+ (4G)", None, "a-b,phone-2-4g"),
            ("explicit", "HAMMER", "Horizon LTE", "hammer,hrz-lte", "hammer,hrz-lte"),
        )
        for name, brand, product, compatible, expected in cases:
            with self.subTest(name=name):
                self.create(name, brand=brand, product=product, compatible=compatible)
                identity = targets.load_target(name)["identity"]
                self.assertEqual(identity["compatible"], expected)

    def test_existing_target_directory_is_refused_unchanged(self) -> None:
        """A second creation cannot overwrite or extend an existing target."""
        existing = self.targets / "hammer-horizon-lte"
        existing.mkdir()
        (existing / "notes.txt").write_text("keep\n", encoding="utf-8")

        with self.assertRaisesRegex(SystemExit, "^fplinux: target hammer-horizon-lte already"):
            self.create("hammer-horizon-lte")

        self.assertEqual([path.name for path in existing.iterdir()], ["notes.txt"])
        self.assertEqual((existing / "notes.txt").read_text(encoding="utf-8"), "keep\n")

    def test_malformed_target_names_are_refused_before_writing(self) -> None:
        """Only lowercase hyphen-separated names reach the target directory."""
        for name in (
            "",
            "Hammer",
            "hammer_lte",
            "hammer.lte",
            "-hammer",
            "hammer-",
            "hammer--lte",
            "../hammer",
            "hammer/lte",
        ):
            with (
                self.subTest(name=name),
                self.assertRaisesRegex(SystemExit, "invalid target name"),
            ):
                self.create(name)
        self.assertEqual(list(self.root.rglob("*hammer*")), [])

    def test_invalid_identity_leaves_no_target_behind(self) -> None:
        """Identity errors are reported, including one found only by the manifest check."""
        cases = (
            ("Horizon  LTE", None, "product must be canonical printable ASCII text"),
            ("Horizon LTE", "HAMMER,lte", "compatible must be a lowercase vendor,device"),
            ("Horizon LTE", "sprd,ums9117", "target and platform compatibles must be distinct"),
        )
        for product, compatible, message in cases:
            with (
                self.subTest(message=message),
                self.assertRaisesRegex(SystemExit, message),
            ):
                self.create("hammer-horizon-lte", product=product, compatible=compatible)
            self.assertEqual(list(self.targets.iterdir()), [])

    def test_device_name_must_fit_the_boot_screen_before_writing(self) -> None:
        """A 31-byte device name is created; one byte more is refused with no target left."""
        self.create("fits", product="Horizon LTE Extra Long 2")
        display_name = targets.load_target("fits")["identity"]["display_name"]
        self.assertEqual(display_name, "HAMMER Horizon LTE Extra Long 2")
        self.assertEqual(len(display_name.encode("ascii")), 31)

        with self.assertRaisesRegex(
            SystemExit, r"^fplinux: bootstrap display name must fit in 31 bytes$"
        ):
            self.create("too-long", product="Horizon LTE Extra Long 22")
        self.assertEqual([path.name for path in self.targets.iterdir()], ["fits"])

    def test_the_only_platform_is_the_default_and_is_recorded(self) -> None:
        """Without --platform, the one directory holding a platform manifest is selected."""
        (self.root / "platforms/drafts").mkdir()
        (self.root / "platforms/README.md").write_text("# Platforms\n", encoding="utf-8")

        self.create("hammer-horizon-lte")

        self.assertEqual(targets.load_target("hammer-horizon-lte")["platform"], "ums9117")

    def test_several_platforms_require_an_explicit_platform(self) -> None:
        """An omitted platform is refused when two exist; the named one is then recorded."""
        self.add_platform("other")

        with self.assertRaisesRegex(
            SystemExit, r"^fplinux: choose the platform with --platform: other, ums9117$"
        ):
            self.create("hammer-horizon-lte")
        self.assertEqual(list(self.targets.iterdir()), [])

        self.create("hammer-horizon-lte", platform="ums9117")
        self.assertEqual(targets.load_target("hammer-horizon-lte")["platform"], "ums9117")

    def test_unknown_platforms_are_refused_before_writing(self) -> None:
        """A missing platform and a directory without a platform manifest are both unknown."""
        (self.root / "platforms/drafts").mkdir()
        for platform in ("missing", "drafts"):
            with (
                self.subTest(platform=platform),
                self.assertRaisesRegex(
                    SystemExit,
                    rf"^fplinux: unknown platform: {platform}; available platforms: ums9117$",
                ),
            ):
                self.create("hammer-horizon-lte", platform=platform)
        self.assertEqual(list(self.targets.iterdir()), [])

    def test_platform_without_a_usable_skeleton_module_is_refused_before_writing(self) -> None:
        """A platform must supply its template through the expected module functions."""
        platform = self.add_platform("other")
        cases: tuple[tuple[str, str | None, str], ...] = (
            ("absent", None, "platform other provides no target skeleton: "),
            ("incomplete", "TEMPLATE = 'target-template'\n", "does not expose template_directory"),
        )
        for name, module, message in cases:
            if module is not None:
                (platform / "host").mkdir()
                (platform / "host/target_skeleton.py").write_text(module, encoding="utf-8")
            with self.subTest(name=name), self.assertRaisesRegex(SystemExit, message):
                self.create("hammer-horizon-lte", platform="other")
            self.assertEqual(list(self.targets.iterdir()), [])


if __name__ == "__main__":
    unittest.main()
