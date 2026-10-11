# SPDX-License-Identifier: GPL-2.0-only
"""Synthetic CM4 load-image and compatibility operation scenarios."""

from __future__ import annotations

import struct

import pytest
from fplinux_cli.device_data.cm4 import cm4_image, omit_initial_pub_policy

from tests.small.device_data.cm4_fixtures import cm4_firmware, load_image


class Cm4LoadImageTests:
    """Check initialized source extents without a physical NAND backup."""

    @pytest.mark.parametrize(
        ("table", "base", "expected_size"),
        [(0xE0, 0x18000000, 300), (0x120, 0x12000000, 364)],
    )
    def test_metadata_keeps_initialized_padding_and_excludes_partition_tail(
        self, table: int, base: int, expected_size: int
    ) -> None:
        """Raw FF bytes are image data; zero destination lengths are not image sizes."""
        original = load_image(table=table, base=base)
        partition = original + b"outside-image" + b"\xff" * 64

        assert cm4_image(partition) == original
        assert len(cm4_image(partition)) == expected_size

    def test_initial_entry_branch_may_change_without_changing_load_metadata(self) -> None:
        """The table reader admits a different initial branch and preserves its bytes."""
        original = bytearray(load_image())
        original[:8] = bytes.fromhex("00 f0 02 f8 00 f0 13 f8")

        assert cm4_image(bytes(original) + b"tail") == bytes(original)

    @pytest.mark.parametrize(
        ("offset", "replacement", "error"),
        [
            (0x34, bytes.fromhex("ff ff ff ff"), "load-table bounds"),
            (0xEC, bytes.fromhex("cc 00 00 18"), "load address"),
            (0x128, b"\x09", "back-reference"),
            (0x124, b"\x71", "output length"),
        ],
    )
    def test_unsupported_or_damaged_metadata_and_compressed_streams_fail(
        self, offset: int, replacement: bytes, error: str
    ) -> None:
        """A bad source extent or compressed token cannot yield a partial image."""
        partition = bytearray(load_image())
        partition[offset : offset + len(replacement)] = replacement

        with pytest.raises(ValueError, match=error):
            cm4_image(bytes(partition))

    def test_truncated_initialized_source_is_rejected(self) -> None:
        """A load table alone cannot admit missing source bytes."""
        with pytest.raises(ValueError, match="truncated CM4 compressed"):
            cm4_image(load_image()[:0x128])


class Cm4CompatibilityOperationTests:
    """Check verified code shape and copy semantics with synthetic instructions."""

    @pytest.mark.parametrize("routine", [0x100, 0x182])
    def test_relocated_routine_changes_only_two_branch_halfwords(self, routine: int) -> None:
        """Literal-pool and instruction relocation retain all other image bytes."""
        original = cm4_firmware(routine=routine)
        first, second = routine + 20, routine + 48
        expected = (
            original[:first]
            + b"\x08\xe0"
            + original[first + 2 : second]
            + b"\x2b\xe0"
            + original[second + 2 :]
        )

        assert omit_initial_pub_policy(original) == expected

    @pytest.mark.parametrize("routine", [0x100, 0x182])
    def test_routine_without_initial_retention_write_keeps_both_setup_calls(
        self, routine: int
    ) -> None:
        """One branch skips the shared writes and retains the complete surrounding setup."""
        original = cm4_firmware(routine=routine, initial_retention=False)
        branch = routine + 28
        expected = original[:branch] + b"\x2c\xe0" + original[branch + 2 :]

        assert omit_initial_pub_policy(original) == expected

    @pytest.mark.parametrize(
        ("offset", "replacement"),
        [
            (0x114, b"\x20\x31"),  # Setup would save to another destination.
            (0x128, b"\x00\xf0\x00\xf8"),  # The write uses an unsupported field operation.
            (0x13A, b"\x00\xbf"),  # A required policy store is missing.
        ],
    )
    def test_changed_single_span_setup_or_write_is_rejected(
        self, offset: int, replacement: bytes
    ) -> None:
        """The shorter form still requires the complete supported setup and write effects."""
        original = bytearray(cm4_firmware(initial_retention=False))
        original[offset : offset + len(replacement)] = replacement

        with pytest.raises(ValueError, match="initialization routine, found 0"):
            omit_initial_pub_policy(bytes(original))

    def test_single_span_without_retained_setup_is_rejected(self) -> None:
        """A policy block without its retained setup is rejected before patching."""
        original = bytearray(cm4_firmware(initial_retention=False))
        struct.pack_into("<H", original, 0x178, 0xBF00)

        with pytest.raises(ValueError, match="initialization routine, found 0"):
            omit_initial_pub_policy(bytes(original))

    def test_unrelated_valid_image_change_is_preserved(self) -> None:
        """A changed initial branch does not need a newly approved image digest."""
        original = bytearray(cm4_firmware())
        original[3] = 0x13

        prepared = omit_initial_pub_policy(bytes(original))

        assert prepared[:0x114] == original[:0x114]
        assert prepared[0x114:0x116] == b"\x08\xe0"
        assert prepared[0x116:0x130] == original[0x116:0x130]
        assert prepared[0x130:0x132] == b"\x2b\xe0"
        assert prepared[0x132:] == original[0x132:]

    @pytest.mark.parametrize(
        ("offset", "value"),
        [
            (0x116, 0xF8C4),  # A store where the routine must read the policy word.
            (0x11A, 0x2300),  # The field value no longer requests retention.
            (0x224, 0xFA0D),  # The called field operation uses a different input.
            (0x1E2, 0xA120),  # The literal points at a different register window.
        ],
    )
    def test_changed_write_or_helper_context_is_not_patched(self, offset: int, value: int) -> None:
        """Similar-looking code without the required operations fails before a write."""
        original = bytearray(cm4_firmware())
        struct.pack_into("<H", original, offset, value)

        with pytest.raises(ValueError, match="initialization routine, found 0"):
            omit_initial_pub_policy(bytes(original))

    def test_missing_and_multiple_compatible_routines_are_rejected(self) -> None:
        """The operation cannot choose an absent or ambiguous initialization routine."""
        with pytest.raises(ValueError, match="initialization routine, found 0"):
            omit_initial_pub_policy(bytes(256))
        first = cm4_firmware()
        duplicate = first + first

        with pytest.raises(ValueError, match="initialization routine, found 2"):
            omit_initial_pub_policy(duplicate)

    def test_different_compatible_forms_are_also_ambiguous(self) -> None:
        """Distinct supported layouts cannot select which initialization routine to patch."""
        first = cm4_firmware()
        second = cm4_firmware(initial_retention=False)

        with pytest.raises(ValueError, match="initialization routine, found 2"):
            omit_initial_pub_policy(first + second)
