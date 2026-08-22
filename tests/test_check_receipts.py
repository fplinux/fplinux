# SPDX-License-Identifier: GPL-2.0-only
"""Tests for successful exact per-scope check receipts."""

from __future__ import annotations

import tempfile
import unittest
from dataclasses import replace
from pathlib import Path

from fplinux_cli.checkreceipts import (
    CheckReceiptRecipe,
    publish_success_receipt,
    receipt_matches,
)


def recipe() -> CheckReceiptRecipe:
    """Return a fixed recipe with visibly distinct digest fields."""
    return CheckReceiptRecipe(
        scope="python",
        closure_digest="a" * 64,
        orchestration_recipe="b" * 64,
        image_identity="sha256:" + "c" * 64,
        commands=(("python3", "/workspace/scripts/check.py", "python"),),
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


if __name__ == "__main__":
    unittest.main()
