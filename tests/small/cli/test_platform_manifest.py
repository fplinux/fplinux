# SPDX-License-Identifier: GPL-2.0-only
"""Platform manifest validation at the loader boundary."""

from __future__ import annotations

import re
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from fplinux_cli import common
from fplinux_cli.manifests import platforms

REPOSITORY_MANIFEST = common.ROOT / "platforms/ums9117/platform.toml"


class PlatformManifestTests(unittest.TestCase):
    """Refuse a platform that omits or misstates its Linux destinations or U-Boot lines."""

    def setUp(self) -> None:
        """Load edited copies of the repository manifest from an isolated project root."""
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        root = Path(self.temporary.name)
        self.manifest = root / "platforms/ums9117/platform.toml"
        self.manifest.parent.mkdir(parents=True)
        self.base = REPOSITORY_MANIFEST.read_text(encoding="utf-8")
        patcher = mock.patch.object(common, "ROOT", root)
        patcher.start()
        self.addCleanup(patcher.stop)

    def with_line(self, key: str, replacement: str) -> str:
        """Replace the one assignment of `key`, including a multi-line array value."""
        edited, count = re.subn(
            rf"^{re.escape(key)} = (?:\[[^\]]*\]|[^\[\n].*)\n",
            lambda _assignment: replacement,
            self.base,
            flags=re.MULTILINE,
        )
        self.assertEqual(count, 1, f"{key} must be assigned once in the repository manifest")
        return edited

    def load(self, text: str) -> None:
        """Validate one manifest text through the production loader."""
        self.manifest.write_text(text, encoding="utf-8")
        platforms.load_platform("ums9117")

    def with_dt_checks(self, declaration: str | None) -> str:
        """Replace compiled-DT ownership declarations with one test-owned TOML value."""
        text = re.sub(
            r"^\[\[linux\.dt_config_checks\]\]\n.*?(?=^\[|\Z)",
            "",
            self.base,
            flags=re.MULTILINE | re.DOTALL,
        )
        if declaration is None:
            return text
        return text.replace("[linux]\n", f"[linux]\ndt_config_checks = {declaration}\n", 1)

    def test_dt_ownership_checks_require_unambiguous_paths_and_symbols(self) -> None:
        """The loader refuses declarations that cannot name one built-in DT owner."""
        cases = (
            (None, "platform linux must contain exactly: .*dt_config_checks"),
            ('"none"', "dt_config_checks must be an array"),
            ('[{path="/soc/usb"}]', "must contain exactly: compatible, config, path"),
            (
                '[{path="soc/usb", compatible="example,usb", config="CONFIG_USB"}]',
                "path must be a canonical absolute device-tree path",
            ),
            (
                '[{path="/soc/../usb", compatible="example,usb", config="CONFIG_USB"}]',
                "path must be a canonical absolute device-tree path",
            ),
            (
                '[{path="/soc/usb", compatible="example,usb", config="CONFIG_USB=y"}]',
                "config must be a CONFIG_\\* symbol",
            ),
            (
                (
                    '[{path="/soc/usb", compatible="example,usb", config="CONFIG_USB"},'
                    '{path="/soc/usb", compatible="example,usb", config="CONFIG_OTHER"}]'
                ),
                "repeats ownership for /soc/usb compatible example,usb",
            ),
        )
        for declaration, message in cases:
            with (
                self.subTest(declaration=declaration),
                self.assertRaisesRegex(SystemExit, message),
            ):
                self.load(self.with_dt_checks(declaration))
        self.load(self.with_dt_checks("[]"))
        self.load(
            self.with_dt_checks(
                '[{path="/soc/usb", compatible="example,usb-cold", config="CONFIG_COLD"},'
                '{path="/soc/usb", compatible="example,usb-live", config="CONFIG_LIVE"}]'
            )
        )

    def test_property_requirement_preserves_the_unconditional_owner(self) -> None:
        """One DT owner may separately require built support for an optional property."""
        declaration = (
            '[{path="/soc/keypad", compatible="example,keypad", config="CONFIG_KEYBOARD"},'
            '{path="/soc/keypad", compatible="example,keypad", property="aux-gpios",'
            'config="CONFIG_AUX_KEY"}]'
        )
        self.manifest.write_text(self.with_dt_checks(declaration), encoding="utf-8")
        loaded = platforms.load_platform("ums9117")
        self.assertEqual(
            loaded["linux"]["dt_config_checks"],
            [
                {
                    "path": "/soc/keypad",
                    "compatible": "example,keypad",
                    "config": "CONFIG_KEYBOARD",
                },
                {
                    "path": "/soc/keypad",
                    "compatible": "example,keypad",
                    "property": "aux-gpios",
                    "config": "CONFIG_AUX_KEY",
                },
            ],
        )

    def test_property_requirement_refuses_empty_or_ambiguous_declarations(self) -> None:
        """A feature requirement must name a property and have one owner per condition."""
        for value in ('""', "1"):
            declaration = (
                '[{path="/soc/keypad", compatible="example,keypad", '
                f'property={value}, config="CONFIG_AUX_KEY"}}]'
            )
            with (
                self.subTest(value=value),
                self.assertRaisesRegex(SystemExit, "property must be a non-empty string"),
            ):
                self.load(self.with_dt_checks(declaration))
        declaration = (
            '[{path="/soc/keypad", compatible="example,keypad", property="aux-gpios",'
            'config="CONFIG_AUX_KEY"},'
            '{path="/soc/keypad", compatible="example,keypad", property="aux-gpios",'
            'config="CONFIG_OTHER"}]'
        )
        with self.assertRaisesRegex(SystemExit, "repeats ownership .*property aux-gpios"):
            self.load(self.with_dt_checks(declaration))

    def test_missing_destination_or_uboot_requirement_is_refused(self) -> None:
        """Omitting any new key names the incomplete table instead of falling back to a path."""
        self.load(self.base)
        cases = (
            ("dts_directory", "platform linux must contain exactly: .*dts_directory"),
            (
                "platform_identity_header",
                "platform linux must contain exactly: .*platform_identity_header",
            ),
            ("required_config", "platform uboot must contain exactly: .*required_config"),
        )
        for key, message in cases:
            with self.subTest(key=key), self.assertRaisesRegex(SystemExit, message):
                self.load(self.with_line(key, ""))

    def test_destinations_and_uboot_lines_are_validated(self) -> None:
        """Paths stay inside the Linux tree and U-Boot requirements are whole .config lines."""
        cases = (
            (
                "dts_directory",
                'dts_directory = "../outside"\n',
                "platform linux dts_directory must be a normalized relative path",
            ),
            (
                "platform_identity_header",
                'platform_identity_header = "/usr/include/identity.h"\n',
                "platform linux platform_identity_header must be a normalized relative path",
            ),
            (
                "required_config",
                'required_config = ["CONFIG_TARGET_DEMO"]\n',
                "platform uboot required_config must contain only CONFIG_",
            ),
            (
                "required_config",
                'required_config = ["CONFIG_TARGET_DEMO=y\\nCONFIG_OTHER=y"]\n',
                "platform uboot required_config must contain only CONFIG_",
            ),
            (
                "required_config",
                "required_config = []\n",
                "platform uboot required_config must be a non-empty array",
            ),
        )
        for key, replacement, message in cases:
            with self.subTest(value=replacement), self.assertRaisesRegex(SystemExit, message):
                self.load(self.with_line(key, replacement))

        self.load(
            self.with_line(
                "required_config",
                "required_config = ['CONFIG_BOOTCOMMAND=\"run a; run b\"', "
                '"# CONFIG_DEMO is not set"]\n',
            )
        )


if __name__ == "__main__":
    unittest.main()
