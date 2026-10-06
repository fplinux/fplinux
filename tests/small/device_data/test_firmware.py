# SPDX-License-Identifier: GPL-2.0-only
"""Synthetic device-data firmware scenarios."""

from __future__ import annotations

import hashlib

import pytest
from fplinux_cli.device_data import bluetooth_firmware as bluetooth


class Cm4CompatibilityOperationTests:
    """Check exact image admission and copy semantics with synthetic instructions."""

    @staticmethod
    def _images() -> tuple[bytes, bytes]:
        original = b"prefix" + b"\x2d\x4c" + b"middle" + b"\xe0\x6d" + b"suffix"
        expected = b"prefix" + b"\x08\xe0" + b"middle" + b"\x2b\xe0" + b"suffix"
        return original, expected

    def test_instruction_rewrite_changes_only_the_two_branches(self) -> None:
        """The synthetic output retains every byte except the two branch instructions."""
        original, expected = self._images()

        assert (bluetooth.omit_initial_pub_policy(original, (6, 14))) == (expected)

    @pytest.mark.parametrize(
        "wrong_tail",
        [pytest.param(b"", id="truncated-original"), pytest.param(b"!", id="changed-original")],
    )
    def test_revision_admits_only_the_exact_original_and_prepared_images(
        self, wrong_tail: bytes
    ) -> None:
        """Wrong source identity or output identity cannot produce an admitted image."""
        original, expected = self._images()
        revision = bluetooth.Cm4Revision(
            size=22,
            original_sha256=hashlib.sha256(original).hexdigest(),
            prepared_sha256=hashlib.sha256(expected).hexdigest(),
            pub_policy_offsets=(6, 14),
        )

        assert (revision.prepare(original)) == (expected)
        with pytest.raises(ValueError, match="original CM4"):
            revision.prepare(original[:-1] + wrong_tail)
        wrong_output = bluetooth.Cm4Revision(
            size=22,
            original_sha256=hashlib.sha256(original).hexdigest(),
            prepared_sha256="0" * 64,
            pub_policy_offsets=(6, 14),
        )
        with pytest.raises(ValueError, match="unexpected image"):
            wrong_output.prepare(original)
        with pytest.raises(ValueError, match="patch instructions"):
            bluetooth.omit_initial_pub_policy(bytes(22), (6, 14))
