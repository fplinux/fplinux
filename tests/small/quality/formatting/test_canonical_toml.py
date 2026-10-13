# SPDX-License-Identifier: GPL-2.0-only
"""In-memory TOML ordering preserves values, comments and collection ownership."""

from __future__ import annotations

import tomllib

import pytest
from fplinux_cli.quality.formatting.canonical_toml import normalize_toml

from tests import ROOT

FIXTURES = ROOT / "tests/fixtures/formatting/toml"


@pytest.mark.parametrize(
    ("case_name", "logical_path"),
    [
        ("target-fields", "platforms/demo/target-template/target.toml.in"),
        ("section-comments", "targets/demo/target.toml"),
        ("inline-firmware", "targets/demo/target.toml"),
        ("platform-copy-fragments", "platforms/demo/platform.toml"),
        ("host-recipe-children", "platforms/demo/platform.toml"),
        ("loader-declarations", "platforms/demo/loader/assets.lock.toml"),
        ("loader-map-members", "targets/demo/target.toml"),
        ("environment-inputs", "environment.lock.toml"),
        ("bundle-loader-outputs", "assets.lock.toml"),
        ("independent-dictionaries", "_typos.toml"),
        ("python-tool-rules", "pyproject.toml"),
        ("container-lock", "container.lock.toml"),
        ("source-lock", "sources.lock.toml"),
        ("boot-lock", "platforms/demo/uboot/u-boot.lock.toml"),
        ("release-manifest", "targets/demo/release/manifest.toml"),
        ("feature-package", "tests/fixtures/profile-config/default-with-feature-package.toml"),
        ("missing-microsd", "tests/fixtures/profile-config/target-without-microsd.toml"),
        ("negative-package-input", "alpine.lock.toml"),
        ("nested-package-list", "targets/demo/target.toml"),
        ("reuse-annotations", "REUSE.toml"),
        ("unknown-tables", "settings.toml"),
    ],
    ids=[
        "target-fields",
        "section-comments",
        "inline-firmware",
        "platform-copy-fragments",
        "host-recipe-children",
        "loader-declarations",
        "loader-map-members",
        "environment-inputs",
        "bundle-loader-outputs",
        "independent-dictionaries",
        "python-tool-rules",
        "container-lock",
        "source-lock",
        "boot-lock",
        "release-manifest",
        "feature-package",
        "missing-microsd",
        "negative-package-input",
        "nested-package-list",
        "reuse-annotations",
        "unknown-tables",
    ],
)
def test_canonical_documents_keep_values_comments_and_collection_ownership(
    case_name: str, logical_path: str
) -> None:
    """Compare exact bytes and preserve every value while applying document-specific order."""
    case = FIXTURES / case_name
    source = (case / "input.toml.fixture").read_bytes()
    expected = (case / "expected.toml.fixture").read_bytes()

    actual = normalize_toml(logical_path, source)

    assert actual == expected
    assert normalize_toml(logical_path, actual) == actual
    assert tomllib.loads(actual.decode()) == tomllib.loads(source.decode())


def test_target_package_sets_sort_without_sorting_other_arrays() -> None:
    """Only package sets change element order; duplicate values remain duplicated."""
    source = (FIXTURES / "target-package-sets/input.toml.fixture").read_bytes()

    actual = normalize_toml("targets/demo/target.toml", source)

    assert tomllib.loads(actual.decode()) == {
        "identity": {"hardware_codes": ["Z-2", "A-1"]},
        "display_brightness": {"levels": [20, 4, 0]},
        "rootfs": {"packages": ["a", "z"]},
        "bundle": {"packages": ["a", "a", "z"]},
    }
    assert normalize_toml("targets/demo/target.toml", actual) == actual


@pytest.mark.parametrize(
    ("case_name", "logical_path"),
    [
        ("package-comments", "platforms/demo/platform.toml"),
        ("leading-package-annotations", "targets/demo/target.toml"),
    ],
    ids=["inline-comments", "leading-comments"],
)
def test_package_annotations_follow_their_sorted_element(
    case_name: str, logical_path: str
) -> None:
    """A package's inline or leading annotation moves with the package it describes."""
    case = FIXTURES / case_name
    source = (case / "input.toml.fixture").read_bytes()
    expected = (case / "expected.toml.fixture").read_bytes()

    actual = normalize_toml(logical_path, source)

    assert actual == expected
    assert normalize_toml(logical_path, actual) == actual


def test_alpine_catalogue_sorts_files_without_sorting_install_lists() -> None:
    """Catalogue records and annotations travel together; apk input lists stay ordered."""
    case = FIXTURES / "alpine-catalogue"
    source = (case / "input.toml.fixture").read_bytes()
    expected = (case / "expected.toml.fixture").read_bytes()

    actual = normalize_toml("alpine.lock.toml", source)

    assert actual == expected
    assert normalize_toml("alpine.lock.toml", actual) == actual
    catalogue = tomllib.loads(actual.decode())["package"]
    assert catalogue == [
        {"repository": "community", "file": "a.apk", "sha256": "a-sha", "bytes": 1},
        {"repository": "main", "file": "z.apk", "sha256": "z-sha", "bytes": 2},
    ]


@pytest.mark.parametrize(
    "source",
    [b"[invalid", b"x = 1\nx = 2\n", b"x = '\xff'\n"],
    ids=["invalid-syntax", "duplicate-key", "invalid-utf8"],
)
def test_syntax_errors_and_invalid_utf8_are_rejected(source: bytes) -> None:
    """Broken syntax is reported instead of rewritten into another document."""
    with pytest.raises(ValueError):  # noqa: PT011 -- stable rejection type; diagnostics vary.
        normalize_toml("targets/demo/target.toml", source)
