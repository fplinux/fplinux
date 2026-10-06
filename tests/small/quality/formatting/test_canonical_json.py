# SPDX-License-Identifier: GPL-2.0-only
"""Field ordering tests at the JSON and JSONC source-text boundary."""

from __future__ import annotations

import json

import pytest
from fplinux_cli.quality.formatting.canonical_json import normalize_json


class CanonicalJsonTests:
    """Check literal canonical output without serializing input values again."""

    def test_package_description_and_dependency_order_retain_values(self) -> None:
        """Sort independent dependency keys without changing scripts or arrays."""
        source = (
            b'{"devDependencies":{"zed":"1","alpha":"2"},'
            b'"scripts":{"zed":"echo z","alpha":"echo a"},'
            b'"description":"Demo","private":true,"name":"demo",'
            b'"extra":["zed","alpha"],"large":9007199254740993,"small":1e-09}'
        )
        expected = (
            b'{"name":"demo","private":true,"description":"Demo",'
            b'"scripts":{"zed":"echo z","alpha":"echo a"},'
            b'"devDependencies":{"alpha":"2","zed":"1"},'
            b'"extra":["zed","alpha"],"large":9007199254740993,"small":1e-09}'
        )
        actual = normalize_json("package.json", source)
        assert (actual) == (expected)
        assert (json.loads(actual)) == (json.loads(source))
        assert (normalize_json("package.json", actual)) == (actual)

    def test_jsonc_comments_move_with_settings(self) -> None:
        """Retain leading and inline comments when independently named keys move."""
        source = b"""// configuration header
{
  "settings": {
    // Zed setting
    "zed": 0, // zero
    /* Alpha setting */
    "alpha": ["zed", "alpha"], // order matters
    // object footer
  },
  "name": "demo"
}
"""
        expected = b"""// configuration header
{
  "name": "demo",
  "settings": {
    /* Alpha setting */
    "alpha": ["zed", "alpha"], // order matters
    // Zed setting
    "zed": 0, // zero
    // object footer
  }
}
"""
        actual = normalize_json("config.jsonc", source)
        assert (actual) == (expected)
        assert (normalize_json("config.jsonc", actual)) == (actual)

    def test_compact_block_comments_and_escaped_keys_are_preserved(self) -> None:
        """Use parser spans so comments, escapes and large numbers remain exact."""
        source = b'{"hashes":{"z\\u0065d":9007199254740993/* size */,"alpha":null}}'
        expected = b'{"hashes":{"alpha":null,"z\\u0065d":9007199254740993/* size */}}'
        actual = normalize_json("manifest.jsonc", source)
        assert (actual) == (expected)
        assert (normalize_json("manifest.jsonc", actual)) == (actual)

    def test_settings_maps_sort_while_arrays_keep_their_order(self) -> None:
        """Sort settings at each independent map boundary without moving arrays."""
        source = b'{"z.setting":{"zed":false,"alpha":true},"a.setting":[{"zed":1,"alpha":2},0]}'
        expected = b'{"a.setting":[{"alpha":2,"zed":1},0],"z.setting":{"alpha":true,"zed":false}}'
        actual = normalize_json(".vscode/settings.json", source)
        assert (actual) == (expected)
        assert (json.loads(actual)) == (json.loads(source))
        assert (normalize_json(".vscode/settings.json", actual)) == (actual)

    def test_formatter_override_order_retains_the_override_sequence(self) -> None:
        """Place override selectors before options without reversing precedence."""
        source = (
            b'{"overrides":[{"options":{"semi":false,"tabWidth":4},"files":"z.*"},'
            b'{"files":"a.*","options":{"semi":true}}]}'
        )
        expected = (
            b'{"overrides":[{"files":"z.*","options":{"tabWidth":4,"semi":false}},'
            b'{"files":"a.*","options":{"semi":true}}]}'
        )
        actual = normalize_json(".prettierrc.json", source)
        assert (actual) == (expected)
        assert (normalize_json(".prettierrc.json", actual)) == (actual)

    def test_package_lock_is_left_to_its_package_manager(self) -> None:
        """Exclude package-manager output even when its property order differs."""
        source = b'{"dependencies":{"zed":1,"alpha":2},"name":"demo"}\n'
        assert (normalize_json("package-lock.json", source)) == (source)

    def test_parseable_template_retains_placeholders_and_non_ascii_text(self) -> None:
        """Apply the description order without altering template strings."""
        source = '{"description":"@DESCRIPTION@ 🍋","name":"@NAME@"}\n'.encode()
        expected = '{"name":"@NAME@","description":"@DESCRIPTION@ 🍋"}\n'.encode()
        actual = normalize_json("package.json.in", source)
        assert (actual) == (expected)
        assert (normalize_json("package.json.in", actual)) == (actual)

    @pytest.mark.parametrize(
        ("relative", "source"),
        [
            ("config.json", b'{"name":}'),
            ("config.json", b'{"name":"demo",}'),
            ("config.json", b'{/* comment */"name":"demo"}'),
            ("config.jsonc", b'{"settings":{"alpha":1,"alpha":2}}'),
        ],
        ids=["missing-value", "trailing-comma", "json-comment", "duplicate-key"],
    )
    def test_invalid_syntax_and_duplicate_keys_remain_errors(
        self, relative: str, source: bytes
    ) -> None:
        """Reject parser recovery and plain JSON comments before returning output."""
        with pytest.raises(ValueError):  # noqa: PT011 -- stable rejection type; diagnostics vary.
            normalize_json(relative, source)
