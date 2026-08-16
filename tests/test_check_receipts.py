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
    receipt_path,
    run_and_publish_success,
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

            self.assertEqual(
                publish_success_receipt(cache, expected),
                receipt_path(cache, expected),
            )
            self.assertTrue(receipt_matches(cache, expected))

    def test_changed_input_is_a_miss(self) -> None:
        """Do not reuse a success after any checked input changes."""
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary) / ".cache"
            published = recipe()
            changed = replace(published, closure_digest="d" * 64)

            publish_success_receipt(cache, published)

            self.assertFalse(receipt_matches(cache, changed))

    def test_failure_and_interrupt_keep_last_good_exact_success(self) -> None:
        """A forced rerun cannot delete or block an earlier exact success."""
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary) / ".cache"
            expected = recipe()
            success = publish_success_receipt(cache, expected)
            original = success.read_bytes()

            with self.assertRaisesRegex(RuntimeError, "failed scope"):
                run_and_publish_success(cache, expected, raise_runtime_error)
            self.assertEqual(success.read_bytes(), original)
            self.assertTrue(receipt_matches(cache, expected))

            with self.assertRaises(KeyboardInterrupt):
                run_and_publish_success(cache, expected, raise_keyboard_interrupt)
            self.assertEqual(success.read_bytes(), original)
            self.assertTrue(receipt_matches(cache, expected))

    def test_later_success_replaces_the_scope_receipt(self) -> None:
        """Keep one latest successful receipt per scope."""
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary) / ".cache"
            first = recipe()
            second = replace(first, closure_digest="d" * 64)

            first_path = publish_success_receipt(cache, first)
            second_path = publish_success_receipt(cache, second)

            self.assertEqual(first_path, second_path)
            self.assertFalse(receipt_matches(cache, first))
            self.assertTrue(receipt_matches(cache, second))


def raise_runtime_error() -> None:
    """Raise a regular task failure."""
    message = "failed scope"
    raise RuntimeError(message)


def raise_keyboard_interrupt() -> None:
    """Model an interrupted scope."""
    raise KeyboardInterrupt


if __name__ == "__main__":
    unittest.main()
