# SPDX-License-Identifier: GPL-2.0-only
"""Tests for directory naming from selected source inventories."""

from __future__ import annotations

import pytest
from fplinux_cli.quality.source_gate import validate_directory_names


def test_packages_and_resources_use_their_own_naming_policy() -> None:
    """Package markers select snake_case without changing resource or filename policy."""
    validate_directory_names(
        [
            "scripts/phone_tools/__init__.py",
            "scripts/phone_tools/_private/__init__.py",
            "scripts/phone_tools/_private/Current_Name.py",
            "scripts/phone_tools/test-data/Input_File.txt",
            ".github/workflows/check.yml",
            ".local-tools/fixture.bin",
            "LICENSES/GPL-2.0-only.txt",
            ".github/ISSUE_TEMPLATE/bug-report.yml",
        ]
    )


def test_resource_directory_cannot_borrow_an_unselected_package_marker() -> None:
    """Only an initializer present in the inventory makes a directory a package."""
    inventory = ["fixtures/phone_tools/input.txt"]
    with pytest.raises(
        SystemExit,
        match=r"^fplinux: source directory must use kebab-case: fixtures/phone_tools$",
    ):
        validate_directory_names(inventory)
    validate_directory_names([*inventory, "fixtures/phone_tools/__init__.py"])


def test_package_marker_does_not_make_its_ancestor_a_package() -> None:
    """Each package directory must have its own selected initializer."""
    with pytest.raises(
        SystemExit,
        match=r"^fplinux: source directory must use kebab-case: fixtures/phone_tools$",
    ):
        validate_directory_names(["fixtures/phone_tools/child/__init__.py"])


@pytest.mark.parametrize(
    "owner",
    ["platforms/demo", "targets/demo", "platforms/demo/target-template"],
)
def test_linux_overlay_accepts_the_kernel_documentation_directory(owner: str) -> None:
    """Full Linux binding paths retain the kernel's prescribed directory name."""
    validate_directory_names(
        [f"{owner}/linux/overlay/Documentation/devicetree/bindings/demo.yaml"]
    )


@pytest.mark.parametrize(
    ("path", "invalid_directory"),
    [
        ("docs/Documentation/guide.txt", "docs/Documentation"),
        ("platforms/demo/linux/Documentation/guide.txt", "platforms/demo/linux/Documentation"),
        (
            "resources/linux/overlay/Documentation/guide.txt",
            "resources/linux/overlay/Documentation",
        ),
        ("targets/demo/linux/overlay/OtherCaps/input.txt", "targets/demo/linux/overlay/OtherCaps"),
        (
            "targets/demo/linux/overlay/Documentation/OtherCaps/input.txt",
            "targets/demo/linux/overlay/Documentation/OtherCaps",
        ),
        ("targets/demo/linux/resources_bad/input.txt", "targets/demo/linux/resources_bad"),
    ],
    ids=["ordinary-docs", "outside-overlay", "outside-owner", "sibling", "child", "resource"],
)
def test_linux_documentation_exception_keeps_neighboring_directory_policy(
    path: str, invalid_directory: str
) -> None:
    """The external name exempts only one directory inside its Linux owner."""
    with pytest.raises(SystemExit) as error:
        validate_directory_names([path])
    assert f"must use kebab-case: {invalid_directory}" in str(error.value)


@pytest.mark.parametrize(
    ("directory", "package", "policy"),
    [
        pytest.param("PhoneTools", False, "kebab-case", id="uppercase-resource"),
        pytest.param("phone_Tools", False, "kebab-case", id="mixed-resource"),
        pytest.param("phone--tools", False, "kebab-case", id="empty-resource-word"),
        pytest.param("phone.tools", False, "kebab-case", id="dot-resource"),
        pytest.param("phone-tools-", False, "kebab-case", id="trailing-resource-separator"),
        pytest.param("PhoneTools", True, "snake_case", id="uppercase-package"),
        pytest.param("phone_Tools", True, "snake_case", id="mixed-package"),
        pytest.param("phone-tools", True, "snake_case", id="hyphenated-package"),
        pytest.param("3phone_tools", True, "snake_case", id="nonidentifier-package"),
    ],
)
def test_invalid_directory_reports_the_path_and_required_policy(
    directory: str, *, package: bool, policy: str
) -> None:
    """Malformed names fail with enough detail to locate and rename the directory."""
    inventory = [f"files/{directory}/input.bin"]
    if package:
        inventory.append(f"files/{directory}/__init__.py")
    with pytest.raises(SystemExit) as error:
        validate_directory_names(inventory)
    assert f"must use {policy}: files/{directory}" in str(error.value)


@pytest.mark.parametrize(
    ("path", "invalid_directory"),
    [
        ("docs/LICENSES/license.txt", "docs/LICENSES"),
        ("LICENSES/Other_Files/license.txt", "LICENSES/Other_Files"),
        ("docs/.github/ISSUE_TEMPLATE/bug.yml", "docs/.github/ISSUE_TEMPLATE"),
        (".github/ISSUE_TEMPLATE/Other_Files/bug.yml", ".github/ISSUE_TEMPLATE/Other_Files"),
    ],
    ids=["nested-license-name", "license-child", "nested-issue-name", "issue-child"],
)
def test_external_names_are_exempt_only_at_their_exact_directory(
    path: str, invalid_directory: str
) -> None:
    """Fixed owner names do not exempt unrelated directories or their descendants."""
    with pytest.raises(SystemExit) as error:
        validate_directory_names([path])
    assert f"must use kebab-case: {invalid_directory}" in str(error.value)
