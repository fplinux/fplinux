# SPDX-License-Identifier: GPL-2.0-only
"""Behavior tests for canonical target and platform identity data."""

from __future__ import annotations

import pytest
from fplinux_cli.build.identity import (
    bootstrap_identity_header,
    linux_identity_dtsi,
    linux_machine_binding,
    linux_machine_binding_path,
    linux_platform_identity_header,
    runtime_identity,
)
from fplinux_cli.build.kernel import prepare as linux_build
from fplinux_cli.manifests.identity import (
    IdentityError,
    validate_platform_identity,
    validate_target_identity,
)


class IdentityTests:
    """Keep human names, hardware codes and DT compatibles unambiguous."""

    @staticmethod
    def target(**changes: object) -> dict[str, object]:
        """Return one complete target identity declaration."""
        return {
            "brand": "Nokia",
            "product": "3210 4G",
            "hardware_codes": ["TA-1618"],
            "compatible": "nokia,ta-1618",
            **changes,
        }

    @staticmethod
    def platform(**changes: object) -> dict[str, object]:
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

        assert (known["display_name"]) == ("Nokia 3210 4G (TA-1618)")
        assert (unknown["display_name"]) == ("INOI 244 Modern 4G")
        assert (multiple["display_name"]) == ("Nokia 3210 4G (CODE1, CODE2)")

    @pytest.mark.parametrize(
        "changes",
        [
            {"product": " 3210 4G"},
            {"product": "3210  4G"},
            {"hardware_codes": ["ta-1618"]},
            {"hardware_codes": ["TA-1618", "TA-1618"]},
            {"compatible": "Nokia,TA-1618"},
        ],
        ids=[
            "leading-space",
            "doubled-space",
            "lowercase-code",
            "duplicate-code",
            "uppercase-compatible",
        ],
    )
    def test_noncanonical_identity_values_are_rejected(self, changes: dict[str, object]) -> None:
        """Reject ambiguous whitespace, code spelling and compatible syntax."""
        with pytest.raises(IdentityError):
            validate_target_identity(self.target(**changes))

    def test_platform_aliases_are_distinct_from_the_soc(self) -> None:
        """Keep vendor aliases separate from the canonical Linux SoC name."""
        identity = validate_platform_identity(self.platform())
        assert (identity["display_name"]) == ("Unisoc UMS9117")
        with pytest.raises(IdentityError, match="must not repeat"):
            validate_platform_identity(self.platform(aliases=["UMS9117"]))

    def test_generated_consumers_share_the_normalized_identity(self) -> None:
        """Generate runtime, bootstrap and DT contracts from the same values."""
        target = validate_target_identity(self.target())
        platform = validate_platform_identity(self.platform())
        runtime = runtime_identity(target, "ums9117", platform)
        header = bootstrap_identity_header(target, "TA1618")
        dtsi = linux_identity_dtsi(target, platform)
        binding = linux_machine_binding(target, platform, arch="arm")
        platform_header = linux_platform_identity_header(platform)

        assert (runtime["target"]["display_name"]) == (target["display_name"])
        assert (runtime["platform"]["display_name"]) == (platform["display_name"])
        assert (b'FPLINUX_BOOTSTRAP_DISPLAY_NAME "Nokia 3210 4G (TA-1618)"') in (header)
        assert (b'FPLINUX_BOOTSTRAP_RECORD_PREFIX "TA1618"') in (header)
        assert (b'model = "Nokia 3210 4G (TA-1618)";') in (dtsi)
        assert (b'compatible = "nokia,ta-1618", "sprd,ums9117";') in (dtsi)
        assert (b"const: Nokia 3210 4G (TA-1618)") in (binding)
        assert (b'FPLINUX_PLATFORM_COMPATIBLE "sprd,ums9117"') in (platform_header)

    @pytest.mark.parametrize(
        ("arch", "directory"),
        [("arm", "arm"), ("arm64", "arm"), ("riscv", "riscv")],
        ids=["arm", "arm64", "riscv"],
    )
    def test_machine_schema_id_matches_its_architecture_directory(
        self, arch: str, directory: str
    ) -> None:
        """Place generated machine bindings in the architecture's schema namespace."""
        target = validate_target_identity(self.target())
        platform = validate_platform_identity(self.platform())
        assert (linux_machine_binding_path(target, arch=arch)) == (
            f"Documentation/devicetree/bindings/{directory}/nokia,ta-1618.yaml"
        )
        binding = linux_machine_binding(target, platform, arch=arch)
        assert (
            f"$id: http://devicetree.org/schemas/{directory}/nokia,ta-1618.yaml#\n".encode()
        ) in (binding)

    def test_bootstrap_name_must_fit_the_fixed_screen_buffer(self) -> None:
        """Fail before compiling a name that the freestanding screen truncates."""
        target = validate_target_identity(
            self.target(product="A Product Name That Is Much Too Long")
        )
        with pytest.raises(IdentityError, match="must fit"):
            bootstrap_identity_header(target, "TA1618")

    def test_linux_recipe_tracks_generated_identity_but_not_platform_aliases(self) -> None:
        """Rebuild DT bytes for visible identity, not provenance-only aliases."""
        source = {"version": "test", "sha256": "a" * 64}
        target = {
            "identity": validate_target_identity(self.target()),
            "linux": {
                "patches": [],
                "copies": [],
                "appends": [],
                "root": {"kind": "initramfs"},
            },
        }
        platform = {
            "identity": validate_platform_identity(self.platform()),
            "linux": {
                "arch": "arm",
                "dts_directory": "arch/arm/boot/dts/unisoc",
                "platform_identity_header": "arch/arm/mach-ums9117/fplinux-platform-identity.h",
                "patches": [],
                "copies": [],
                "appends": [],
            },
        }
        baseline = linux_build.linux_recipe_digest(source, "phone", target, platform)

        changed_target = {
            **target,
            "identity": validate_target_identity(self.target(product="Changed Phone")),
        }
        changed_alias = {
            **platform,
            "identity": validate_platform_identity(self.platform(aliases=["T117", "PIKE2"])),
        }

        assert (baseline) != (
            linux_build.linux_recipe_digest(source, "phone", changed_target, platform)
        )
        assert (baseline) == (
            linux_build.linux_recipe_digest(source, "phone", target, changed_alias)
        )
