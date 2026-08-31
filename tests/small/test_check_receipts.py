# SPDX-License-Identifier: GPL-2.0-only
"""Tests for successful exact per-scope check receipts."""

from __future__ import annotations

import json
import tempfile
import unittest
from dataclasses import replace
from pathlib import Path

from fplinux_cli.checkreceipts import (
    CheckReceiptRecipe,
    publish_success_receipt,
    receipt_matches,
    receipt_path,
)


def recipe(profile: str | None = None) -> CheckReceiptRecipe:
    """Return a fixed recipe with visibly distinct digest fields."""
    return CheckReceiptRecipe(
        scope="python",
        closure_digest="a" * 64,
        orchestration_recipe="b" * 64,
        image_generation="c" * 64,
        commands=(("python3", "/workspace/scripts/check.py", "python"),),
        profile=profile,
    )


class CheckReceiptTests(unittest.TestCase):
    """Only an exact successful check may be reused."""

    def test_published_success_is_an_exact_hit(self) -> None:
        """Accept the receipt published for its exact recipe."""
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary) / ".cache"
            expected = recipe()

            publish_success_receipt(cache, expected)
            self.assertTrue(receipt_matches(cache, expected))

    def test_changed_input_is_a_miss(self) -> None:
        """Do not reuse a success after its checked closure digest changes."""
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary) / ".cache"
            published = recipe()
            changed = replace(published, closure_digest="d" * 64)

            publish_success_receipt(cache, published)

            self.assertFalse(receipt_matches(cache, changed))

    def test_later_success_replaces_the_scope_receipt(self) -> None:
        """Keep one latest successful receipt per scope."""
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary) / ".cache"
            first = recipe()
            second = replace(first, closure_digest="d" * 64)

            publish_success_receipt(cache, first)
            publish_success_receipt(cache, second)

            self.assertFalse(receipt_matches(cache, first))
            self.assertTrue(receipt_matches(cache, second))

    def test_profile_receipt_does_not_replace_the_default_slot(self) -> None:
        """A named profile owns a separate fixed receipt path."""
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary) / ".cache"
            default = recipe()
            profile = recipe("usb-host-lab")

            publish_success_receipt(cache, default)
            publish_success_receipt(cache, profile)

            self.assertEqual(
                receipt_path(cache, default),
                cache / "check-results/python/success.json",
            )
            self.assertEqual(
                receipt_path(cache, profile),
                cache / "check-results/profiles/usb-host-lab/python/success.json",
            )
            self.assertTrue(receipt_matches(cache, default))
            self.assertTrue(receipt_matches(cache, profile))

    def test_publish_reuses_one_fixed_temporary_path(self) -> None:
        """An interrupted receipt write cannot leave unbounded temporary names."""
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary) / ".cache"
            expected = recipe("usb-host-lab")
            temporary_path = receipt_path(cache, expected).parent / ".success.tmp"
            temporary_path.parent.mkdir(parents=True)
            temporary_path.write_text("partial\n", encoding="utf-8")

            publish_success_receipt(cache, expected)

            self.assertFalse(temporary_path.exists())
            self.assertTrue(receipt_matches(cache, expected))

    def test_receipt_without_the_exact_profile_field_is_a_miss(self) -> None:
        """An older receipt shape is not reinterpreted for the default context."""
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary) / ".cache"
            expected = recipe()
            payload = expected.payload()
            del payload["profile"]
            path = receipt_path(cache, expected)
            path.parent.mkdir(parents=True)
            path.write_text(json.dumps(payload), encoding="utf-8")

            self.assertFalse(receipt_matches(cache, expected))


if __name__ == "__main__":
    unittest.main()
