# SPDX-License-Identifier: GPL-2.0-only
"""Tests for successful exact per-scope check receipts."""

from __future__ import annotations

import json
import shutil
import tempfile
from dataclasses import replace
from pathlib import Path
from unittest import mock

import pytest
from fplinux_cli import common
from fplinux_cli.quality.receipts import (
    CheckReceiptRecipe,
    check_orchestration_recipe_digest,
    publish_success_receipt,
    receipt_matches,
    receipt_path,
)

from tests import ROOT


def recipe() -> CheckReceiptRecipe:
    """Return a fixed recipe with visibly distinct digest fields."""
    return CheckReceiptRecipe(
        scope="python",
        closure_digest="a" * 64,
        orchestration_recipe="b" * 64,
        environment_generation="c" * 64,
    )


class CheckReceiptTests:
    """Only an exact successful check may be reused."""

    @pytest.mark.parametrize("scope", ["alpine", "kernel"], ids=("alpine", "kernel"))
    def test_package_registration_edit_revokes_check_orchestration_receipt(
        self, scope: str
    ) -> None:
        """Checks cannot retain their success when package-selection declarations change."""
        repository = ROOT
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            shutil.copytree(
                repository / "scripts/fplinux_cli",
                root / "scripts/fplinux_cli",
                ignore=shutil.ignore_patterns("__pycache__", "*.pyc"),
            )
            registration = root / "scripts/fplinux_cli/alpine/registration.py"
            cache = root / ".cache"
            source = 'COMMON_PACKAGES = ("runtime-a",)\n'
            with mock.patch.object(common, "ROOT", root):
                registration.write_text(source, encoding="utf-8")
                original = replace(
                    recipe(),
                    scope=scope,
                    build_type="release" if scope == "kernel" else None,
                    orchestration_recipe=check_orchestration_recipe_digest("d" * 64),
                )
                publish_success_receipt(cache, original)
                assert receipt_matches(cache, original)

                registration.write_text(
                    'COMMON_PACKAGES = ("runtime-a", "runtime-b")\n',
                    encoding="utf-8",
                )
                changed = replace(
                    original, orchestration_recipe=check_orchestration_recipe_digest("d" * 64)
                )
                assert not (receipt_matches(cache, changed))

    def test_kernel_types_retain_separate_success_receipts(self) -> None:
        """A debug check must miss before it runs without discarding a release success."""
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary)
            release = replace(recipe(), scope="kernel", build_type="release")
            debug = replace(release, build_type="debug")
            publish_success_receipt(cache, release)
            assert not (receipt_matches(cache, debug))
            publish_success_receipt(cache, debug)
            assert receipt_matches(cache, release)
            assert receipt_matches(cache, debug)

    def test_later_success_replaces_the_scope_receipt(self) -> None:
        """Keep one latest successful receipt per scope."""
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary) / ".cache"
            first = recipe()
            second = replace(first, closure_digest="d" * 64)

            publish_success_receipt(cache, first)
            publish_success_receipt(cache, second)

            assert not (receipt_matches(cache, first))
            assert receipt_matches(cache, second)

    def test_named_kernel_profile_keeps_its_own_receipt(self) -> None:
        """A kernel check for a named profile starts with no cached success."""
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary) / ".cache"
            default = replace(recipe(), scope="kernel", build_type="release")
            profile = replace(default, profile="microsd-uboot")

            publish_success_receipt(cache, default)
            assert not receipt_matches(cache, profile)
            publish_success_receipt(cache, profile)

            assert receipt_matches(cache, default)
            assert receipt_matches(cache, profile)

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

            assert not (receipt_matches(cache, expected))
