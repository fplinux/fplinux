# SPDX-License-Identifier: GPL-2.0-only
"""Behavior tests for canonical target and platform identity data."""

from __future__ import annotations

import unittest

from fplinux_cli import builder
from fplinux_cli.identity import (
    IdentityError,
    validate_platform_identity,
    validate_target_identity,
)
from fplinux_cli.identity_codegen import (
    bootstrap_identity_header,
    linux_identity_dtsi,
    linux_machine_binding,
    linux_platform_identity_header,
    runtime_identity,
)


class IdentityTests(unittest.TestCase):
    """Keep human names, hardware codes and DT compatibles unambiguous."""

    def target(self, **changes: object) -> dict[str, object]:
        """Return one complete target identity declaration."""
        return {
            "brand": "Nokia",
            "product": "3210 4G",
            "hardware_codes": ["TA-1618"],
            "compatible": "nokia,ta-1618",
            **changes,
        }

    def platform(self, **changes: object) -> dict[str, object]:
        """Return one complete platform identity declaration."""
        return {
            "vendor": "Unisoc",
            "soc": "UMS9117",
            "aliases": ["T117"],
            "compatible": "sprd,ums9117",
            **changes,
        }

    def test_display_name_is_derived_for_known_and_unknown_codes(self) -> None:
        """Represent unknown codes explicitly without inventing presentation text."""
        known = validate_target_identity(self.target())
        unknown = validate_target_identity(
            self.target(
                brand="INOI",
                product="244 Modern 4G",
                hardware_codes=[],
                compatible="inoi,244-modern-4g",
            )
        )
        multiple = validate_target_identity(self.target(hardware_codes=["CODE1", "CODE2"]))

        self.assertEqual(known["display_name"], "Nokia 3210 4G (TA-1618)")
        self.assertEqual(unknown["display_name"], "INOI 244 Modern 4G")
        self.assertEqual(multiple["display_name"], "Nokia 3210 4G (CODE1, CODE2)")

    def test_noncanonical_identity_values_are_rejected(self) -> None:
        """Reject ambiguous whitespace, code spelling and compatible syntax."""
        cases = (
            self.target(product=" 3210 4G"),
            self.target(product="3210  4G"),
            self.target(hardware_codes=["ta-1618"]),
            self.target(hardware_codes=["TA-1618", "TA-1618"]),
            self.target(compatible="Nokia,TA-1618"),
        )
        for value in cases:
            with self.subTest(value=value), self.assertRaises(IdentityError):
                validate_target_identity(value)

    def test_platform_aliases_are_distinct_from_the_soc(self) -> None:
        """Keep vendor aliases separate from the canonical Linux SoC name."""
        identity = validate_platform_identity(self.platform())
        self.assertEqual(identity["display_name"], "Unisoc UMS9117")
        with self.assertRaisesRegex(IdentityError, "must not repeat"):
            validate_platform_identity(self.platform(aliases=["UMS9117"]))

    def test_generated_consumers_share_the_normalized_identity(self) -> None:
        """Generate runtime, bootstrap and DT contracts from the same values."""
        target = validate_target_identity(self.target())
        platform = validate_platform_identity(self.platform())
        runtime = runtime_identity(target, "ums9117", platform)
        header = bootstrap_identity_header(target, "TA1618")
        dtsi = linux_identity_dtsi(target, platform)
        binding = linux_machine_binding(target, platform)
        platform_header = linux_platform_identity_header(platform)

        self.assertEqual(runtime["target"]["display_name"], target["display_name"])
        self.assertEqual(runtime["platform"]["display_name"], platform["display_name"])
        self.assertIn(b'FPLINUX_BOOTSTRAP_DISPLAY_NAME "Nokia 3210 4G (TA-1618)"', header)
        self.assertIn(b'FPLINUX_BOOTSTRAP_RECORD_PREFIX "TA1618"', header)
        self.assertIn(b'model = "Nokia 3210 4G (TA-1618)";', dtsi)
        self.assertIn(b'compatible = "nokia,ta-1618", "sprd,ums9117";', dtsi)
        self.assertIn(b"const: Nokia 3210 4G (TA-1618)", binding)
        self.assertIn(b'FPLINUX_PLATFORM_COMPATIBLE "sprd,ums9117"', platform_header)

    def test_bootstrap_name_must_fit_the_fixed_screen_buffer(self) -> None:
        """Fail before compiling a name that the freestanding screen truncates."""
        target = validate_target_identity(
            self.target(product="A Product Name That Is Much Too Long")
        )
        with self.assertRaisesRegex(IdentityError, "must fit"):
            bootstrap_identity_header(target, "TA1618")

    def test_linux_recipe_tracks_generated_identity_but_not_platform_aliases(self) -> None:
        """Rebuild DT bytes for visible identity, not provenance-only aliases."""
        source = {"version": "test", "sha256": "a" * 64}
        target = {
            "identity": validate_target_identity(self.target()),
            "linux": {"patches": [], "copies": [], "appends": []},
        }
        platform = {
            "identity": validate_platform_identity(self.platform()),
            "linux": {"patches": [], "copies": [], "appends": []},
        }
        baseline = builder.linux_recipe_digest(source, "phone", target, platform)

        changed_target = {
            **target,
            "identity": validate_target_identity(self.target(product="Changed Phone")),
        }
        changed_alias = {
            **platform,
            "identity": validate_platform_identity(self.platform(aliases=["T117", "PIKE2"])),
        }

        self.assertNotEqual(
            baseline,
            builder.linux_recipe_digest(source, "phone", changed_target, platform),
        )
        self.assertEqual(
            baseline,
            builder.linux_recipe_digest(source, "phone", target, changed_alias),
        )


if __name__ == "__main__":
    unittest.main()
