# SPDX-License-Identifier: GPL-2.0-only
"""Add punctuation-bearing parameter IDs to a controlled selection module."""

import pytest


class ParameterCase:
    """Expose parameter IDs at either subprocess runtime boundary."""

    @pytest.mark.parametrize("value", ["first", "second"], ids=["first", "other::id"])
    def test_value(self, value: str) -> None:
        """Accept the controlled inputs while the module fixture records execution."""
        assert value in ("first", "second")
