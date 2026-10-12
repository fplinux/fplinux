# SPDX-License-Identifier: GPL-2.0-only
"""Validate the target audio topology passed to its current consumers."""

from __future__ import annotations


class AudioPolicyError(ValueError):
    """Reject an incomplete or non-boolean audio policy."""


AUDIO_POLICY_FIELDS = frozenset(
    {"speaker_vibration", "speaker_from_headphones", "headfree_headphones_only"}
)


def audio_policy_from_profile(value: object) -> dict[str, bool]:
    """Use the same false defaults for complete and Linux-only target loading."""
    if value is None:
        value = {}
    if not isinstance(value, dict):
        message = "target audio-profile must be a table"
        raise AudioPolicyError(message)
    return validate_audio_policy(
        {field: value.get(field, False) for field in AUDIO_POLICY_FIELDS},
        "target audio-profile",
    )


def validate_audio_policy(value: object, name: str = "audio policy") -> dict[str, bool]:
    """Require the three board-owned audio topology choices."""
    fields = AUDIO_POLICY_FIELDS
    if not isinstance(value, dict) or set(value) != fields:
        raise AudioPolicyError(f"{name} must contain exactly: {', '.join(sorted(fields))}")
    result: dict[str, bool] = {}
    for field in sorted(fields):
        choice = value[field]
        if not isinstance(choice, bool):
            raise AudioPolicyError(f"{name} {field} must be a boolean")
        result[field] = choice
    return result
