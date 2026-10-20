# SPDX-License-Identifier: GPL-2.0-only
"""Headless target skeletons written to a disposable checkout by the target creator."""

from __future__ import annotations

import contextlib
import io
import shutil
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING
from unittest import mock

import pytest
from fplinux_cli import common
from fplinux_cli.cli import target_new
from fplinux_cli.manifests import paths, platforms, targets
from fplinux_cli.packaging import archive as package
from fplinux_cli.packaging.readme import ordinary_readme

REPOSITORY = common.ROOT
DISPLAY_KEYS = {"spi_mode", "lcd_id", "backlight_channels", "backlight_level"}

if TYPE_CHECKING:
    from collections.abc import Iterator


@dataclass(frozen=True)
class TargetCheckout:
    """A disposable checkout with a writable target directory."""

    root: Path
    targets: Path


@pytest.fixture
def target_checkout() -> Iterator[TargetCheckout]:
    """Provide a checkout with the platform inputs and an empty target directory."""
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        platform = root / "platforms/ums9117"
        (platform / "host").mkdir(parents=True)
        (platform / "linux/overlay").mkdir(parents=True)
        shutil.copy2(REPOSITORY / "platforms/ums9117/platform.toml", platform)
        shutil.copy2(REPOSITORY / "platforms/ums9117/host/target_skeleton.py", platform / "host")
        shutil.copytree(
            REPOSITORY / "platforms/ums9117/target-template", platform / "target-template"
        )
        shutil.copytree(REPOSITORY / "platforms/ums9117/loader", platform / "loader")
        shutil.copytree(REPOSITORY / "alpine/aports", root / "alpine/aports")
        shutil.copytree(REPOSITORY / "profiles", root / "profiles")
        target_directory = root / "targets"
        target_directory.mkdir()
        with mock.patch.object(common, "ROOT", root):
            yield TargetCheckout(root=root, targets=target_directory)


def create_target(
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


def add_platform(target_checkout: TargetCheckout, name: str) -> Path:
    """Declare a second platform that has a manifest but no target skeleton module."""
    platform = target_checkout.root / "platforms" / name
    platform.mkdir()
    shutil.copy2(REPOSITORY / "platforms/ums9117/platform.toml", platform)
    return platform


class NewTargetTests:
    """Create skeletons from the real platform template module, manifests and profiles."""

    def test_new_target_loads_as_a_headless_ram_only_phone(
        self, target_checkout: TargetCheckout
    ) -> None:
        """The skeleton boots through FDL1 without a display or mandatory personal data."""
        printed = create_target("hammer-horizon-lte")

        config = targets.load_target("hammer-horizon-lte")
        assert (config["identity"]) == (
            {
                "brand": "HAMMER",
                "product": "Horizon LTE",
                "hardware_codes": [],
                "compatible": "hammer,horizon-lte",
                "display_name": "HAMMER Horizon LTE",
            }
        )
        assert (config["nand"]) == ({"raw_device": "/dev/ums9117-nand-raw"})
        assert (config["runtime"]["assets"]) == ({"fdl1": "assets/t117_fdl1.bin"})
        assert not (
            target_checkout.targets / "hammer-horizon-lte/loader/assets.lock.toml"
        ).exists()
        assert DISPLAY_KEYS.isdisjoint(config["runtime"]["adapter"])
        assert (config["runtime"]["adapter"]["exec_distance"]) == (0)
        assert config["device_data"] == {"groups": {}}
        assert (paths.discover_profiles("hammer-horizon-lte")) == (("default",))
        with pytest.raises(SystemExit, match="does not support profile microsd-uboot"):
            targets.load_target("hammer-horizon-lte", "microsd-uboot")
        release = package.load_release_manifest("hammer-horizon-lte", config)
        assert ("assets/t117_fdl1.bin") in (release["runtime_files"])
        assert not (target_checkout.targets / "hammer-horizon-lte/README.md").exists()

        lines = printed.splitlines()
        assert (lines[0]) == ("Created headless target targets/hammer-horizon-lte:")
        assert (lines[-1]) == (
            "Next: ./fplinux build hammer-horizon-lte && ./fplinux run hammer-horizon-lte"
        )
        created = sorted(
            path.relative_to(target_checkout.targets / "hammer-horizon-lte").as_posix()
            for path in (target_checkout.targets / "hammer-horizon-lte").rglob("*")
            if path.is_file()
        )
        assert (sorted(line.strip() for line in lines[1:-1])) == (created)

    def test_new_target_release_readme_identifies_the_phone_and_loader_sequence(
        self, target_checkout: TargetCheckout
    ) -> None:
        """Generated instructions identify the skeleton and preserve its loader sequence."""
        create_target("hammer-horizon-lte")
        routes = target_checkout.root / "site/src/data/phones.json"
        routes.parent.mkdir(parents=True)
        routes.write_text("[]\n", encoding="utf-8")
        config = targets.load_target("hammer-horizon-lte")
        readme = ordinary_readme("hammer-horizon-lte", config).decode("utf-8")
        prose = " ".join(readme.split())

        assert readme.startswith("FPLinux for HAMMER Horizon LTE\n")
        assert "Keep the powered-off phone disconnected" in prose
        assert "python3.14 -I fplinux.py run\n" in readme
        assert "Wait until the runner asks for the phone, then follow its instruction" in prose
        assert "Power the phone off, hold *, then connect USB while keeping * pressed." in prose
        assert "If connected too early, disconnect and restart this sequence." in prose
        assert readme.index("python3.14 -I fplinux.py run\n") < readme.index(
            "Wait until the runner"
        )
        assert readme.index("Wait until the runner") < readme.index("Power the phone off, hold *")
        assert "python3.14 -I fplinux.py run --reconnect" in readme
        assert "Phone support, controls and limits: docs/porting/checklist/index.html" in prose
        assert "confirm it on this phone before use" in prose
        assert "Configuration alone does not establish support" in prose

    @pytest.mark.usefixtures("target_checkout")
    def test_new_target_release_carries_every_declared_bundle_apk(self) -> None:
        """The generated release includes exactly the platform and target bundle packages."""
        create_target("hammer-horizon-lte")
        config = targets.load_target("hammer-horizon-lte")
        platform = platforms.load_platform(config["platform"])
        release = package.load_release_manifest("hammer-horizon-lte", config)

        declared_apks = {
            f"apks/{name}.apk"
            for name in (*platform["bundle"]["packages"], *config["bundle"]["packages"])
        }
        release_apks = {
            relative
            for relative in release["bundle_files"]
            if relative.startswith("apks/") and relative.endswith(".apk")
        }
        assert (release_apks) == (declared_apks)

    @pytest.mark.usefixtures("target_checkout")
    @pytest.mark.parametrize(
        ("name", "brand", "product", "compatible", "expected"),
        [
            ("derived", "A&B", "Phone 2+ (4G)", None, "a-b,phone-2-4g"),
            ("explicit", "HAMMER", "Horizon LTE", "hammer,hrz-lte", "hammer,hrz-lte"),
        ],
        ids=["derived", "explicit"],
    )
    def test_compatible_defaults_to_the_public_names_unless_given(
        self,
        name: str,
        brand: str,
        product: str,
        compatible: str | None,
        expected: str,
    ) -> None:
        """A derived compatible folds punctuation; an explicit one is kept verbatim."""
        create_target(name, brand=brand, product=product, compatible=compatible)
        identity = targets.load_target(name)["identity"]
        assert (identity["compatible"]) == (expected)

    def test_existing_target_directory_is_refused_unchanged(
        self, target_checkout: TargetCheckout
    ) -> None:
        """A second creation cannot overwrite or extend an existing target."""
        existing = target_checkout.targets / "hammer-horizon-lte"
        existing.mkdir()
        (existing / "notes.txt").write_text("keep\n", encoding="utf-8")

        with pytest.raises(SystemExit, match=r"^fplinux: target hammer-horizon-lte already"):
            create_target("hammer-horizon-lte")

        assert ([path.name for path in existing.iterdir()]) == (["notes.txt"])
        assert ((existing / "notes.txt").read_text(encoding="utf-8")) == ("keep\n")

    @pytest.mark.parametrize(
        "name",
        [
            "",
            "Hammer",
            "hammer_lte",
            "hammer.lte",
            "-hammer",
            "hammer-",
            "hammer--lte",
            "../hammer",
            "hammer/lte",
        ],
        ids=[
            "empty",
            "Hammer",
            "hammer_lte",
            "hammer.lte",
            "-hammer",
            "hammer-",
            "hammer--lte",
            "../hammer",
            "hammer/lte",
        ],
    )
    def test_malformed_target_names_are_refused_before_writing(
        self, target_checkout: TargetCheckout, name: str
    ) -> None:
        """Only lowercase hyphen-separated names reach the target directory."""
        with (
            pytest.raises(SystemExit, match="invalid target name"),
        ):
            create_target(name)
        assert (list(target_checkout.root.rglob("*hammer*"))) == ([])

    @pytest.mark.parametrize(
        ("product", "compatible", "message"),
        [
            ("Horizon  LTE", None, "product must be canonical printable ASCII text"),
            ("Horizon LTE", "HAMMER,lte", "compatible must be a lowercase vendor,device"),
            ("Horizon LTE", "sprd,ums9117", "target and platform compatibles must be distinct"),
        ],
        ids=["Horizon  LTE-1", "Horizon LTE-2", "Horizon LTE-3"],
    )
    def test_invalid_identity_leaves_no_target_behind(
        self, target_checkout: TargetCheckout, product: str, compatible: str | None, message: str
    ) -> None:
        """Identity errors are reported, including one found only by the manifest check."""
        with (
            pytest.raises(SystemExit, match=message),
        ):
            create_target("hammer-horizon-lte", product=product, compatible=compatible)
        assert (list(target_checkout.targets.iterdir())) == ([])

    def test_device_name_must_fit_the_boot_screen_before_writing(
        self, target_checkout: TargetCheckout
    ) -> None:
        """A 31-byte device name is created; one byte more is refused with no target left."""
        create_target("fits", product="Horizon LTE Extra Long 2")
        display_name = targets.load_target("fits")["identity"]["display_name"]
        assert (display_name) == ("HAMMER Horizon LTE Extra Long 2")
        assert (len(display_name.encode("ascii"))) == (31)

        with pytest.raises(
            SystemExit, match=r"^fplinux: bootstrap display name must fit in 31 bytes$"
        ):
            create_target("too-long", product="Horizon LTE Extra Long 22")
        assert ([path.name for path in target_checkout.targets.iterdir()]) == (["fits"])

    def test_the_only_platform_is_the_default_and_is_recorded(
        self, target_checkout: TargetCheckout
    ) -> None:
        """Without --platform, the one directory holding a platform manifest is selected."""
        (target_checkout.root / "platforms/drafts").mkdir()
        (target_checkout.root / "platforms/README.md").write_text(
            "# Platforms\n", encoding="utf-8"
        )

        create_target("hammer-horizon-lte")

        assert (targets.load_target("hammer-horizon-lte")["platform"]) == ("ums9117")

    def test_several_platforms_require_an_explicit_platform(
        self, target_checkout: TargetCheckout
    ) -> None:
        """An omitted platform is refused when two exist; the named one is then recorded."""
        add_platform(target_checkout, "other")

        with pytest.raises(
            SystemExit, match=r"^fplinux: choose the platform with --platform: other, ums9117$"
        ):
            create_target("hammer-horizon-lte")
        assert (list(target_checkout.targets.iterdir())) == ([])

        create_target("hammer-horizon-lte", platform="ums9117")
        assert (targets.load_target("hammer-horizon-lte")["platform"]) == ("ums9117")

    @pytest.mark.parametrize("platform", ["missing", "drafts"], ids=["missing", "drafts"])
    def test_unknown_platforms_are_refused_before_writing(
        self, target_checkout: TargetCheckout, platform: str
    ) -> None:
        """A missing platform and a directory without a platform manifest are both unknown."""
        (target_checkout.root / "platforms/drafts").mkdir()
        with (
            pytest.raises(
                SystemExit,
                match=f"^fplinux: unknown platform: {platform}; available platforms: ums9117$",
            ),
        ):
            create_target("hammer-horizon-lte", platform=platform)
        assert (list(target_checkout.targets.iterdir())) == ([])

    @pytest.mark.parametrize(
        ("module", "message"),
        [
            (None, "platform other provides no target skeleton: "),
            ("TEMPLATE = 'target-template'\n", "does not expose template_directory"),
        ],
        ids=["absent", "incomplete"],
    )
    def test_platform_without_a_usable_skeleton_module_is_refused_before_writing(
        self, target_checkout: TargetCheckout, module: str | None, message: str
    ) -> None:
        """A platform must supply its template through the expected module functions."""
        platform = add_platform(target_checkout, "other")
        if module is not None:
            (platform / "host").mkdir()
            (platform / "host/target_skeleton.py").write_text(module, encoding="utf-8")
        with (
            pytest.raises(SystemExit, match=message),
        ):
            create_target("hammer-horizon-lte", platform="other")
        assert (list(target_checkout.targets.iterdir())) == ([])
