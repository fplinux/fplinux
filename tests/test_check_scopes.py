# SPDX-License-Identifier: GPL-2.0-only
"""Tests for public check scope selection semantics."""

from __future__ import annotations

import unittest

from fplinux_cli.container import (
    CHECK_SCOPES,
    analyzer_cache_names,
    resolve_check_scopes,
)


class CheckScopeTests(unittest.TestCase):
    """Keep scope selection stable and independent of argument order."""

    def test_empty_selection_means_all_scopes(self) -> None:
        """Keep the no-argument command as the complete quality gate."""
        self.assertEqual(resolve_check_scopes([]), CHECK_SCOPES)

    def test_selection_is_deduplicated_in_canonical_order(self) -> None:
        """Deduplicate selections and ignore their command-line order."""
        self.assertEqual(
            resolve_check_scopes(["kernel", "python", "kernel", "repository"]),
            ("repository", "python", "kernel"),
        )

    def test_analyzer_caches_follow_selected_scopes(self) -> None:
        """Avoid validating analyzer caches irrelevant to a scoped run."""
        self.assertEqual(analyzer_cache_names(("docs",)), ())
        self.assertEqual(analyzer_cache_names(("c",)), ("analysis",))
        self.assertEqual(
            analyzer_cache_names(("kernel",)),
            ("analysis", "downloads", "linux"),
        )

    def test_unknown_scope_is_rejected(self) -> None:
        """Reject names outside the public scope registry."""
        with self.assertRaisesRegex(SystemExit, "unknown check scope: imaginary"):
            resolve_check_scopes(["imaginary"])


if __name__ == "__main__":
    unittest.main()
