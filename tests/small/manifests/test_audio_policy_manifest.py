# SPDX-License-Identifier: GPL-2.0-only
"""Target manifest audio choices shared by firmware and DT consumers."""

from __future__ import annotations

from unittest import mock

import pytest
from fplinux_cli import common
from fplinux_cli.manifests import targets
from fplinux_cli.manifests.audio import AudioPolicyError, validate_audio_policy


@pytest.mark.parametrize(
    ("target", "expected"),
    [
        ("nokia-ta1618", (False, False, False)),
        ("inoi-240-modern-4g", (True, False, False)),
        ("bq-2830l-tank-connect-max", (False, True, False)),
        ("uniwa-v1000-4g", (False, False, True)),
    ],
)
def test_target_policy_preserves_the_current_audio_topology(
    target: str, expected: tuple[bool, bool, bool]
) -> None:
    """The supported audio variants select the established three topology choices."""
    policy = targets.load_target(target)["audio_policy"]
    assert (
        policy["speaker_vibration"],
        policy["speaker_from_headphones"],
        policy["headfree_headphones_only"],
    ) == expected


@pytest.mark.parametrize("value", [1, "true", None])
def test_target_policy_rejects_non_boolean_choices(value: object) -> None:
    """Invalid policy cannot reach profile generation or Linux projection."""
    raw = common.load_toml(common.ROOT / "targets/nokia-ta1618/target.toml")
    raw["audio_profile"]["speaker_vibration"] = value
    with (
        mock.patch.object(targets, "load_toml", return_value=raw),
        pytest.raises(SystemExit, match="speaker_vibration must be a boolean"),
    ):
        targets.load_target("nokia-ta1618")


def test_standalone_policy_requires_all_choices() -> None:
    """A portable snapshot is complete rather than receiving checkout-only defaults."""
    with pytest.raises(AudioPolicyError, match="must contain exactly"):
        validate_audio_policy({"speaker_vibration": False})
