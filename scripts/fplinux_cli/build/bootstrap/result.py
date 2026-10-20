# SPDX-License-Identifier: GPL-2.0-only
"""Artifacts and optional upload regions produced by a bootstrap build."""

from __future__ import annotations

from dataclasses import dataclass
from typing import TYPE_CHECKING, TypedDict

if TYPE_CHECKING:
    from pathlib import Path


class PayloadRegion(TypedDict):
    """One compact-file region and its physical upload destination."""

    offset: int
    size: int
    load_address: int


@dataclass(frozen=True)
class BootstrapBuild:
    """Verified public payload and its host-side session contract."""

    image: Path
    map: Path
    personalization: dict[str, int | str]
    regions: dict[str, PayloadRegion] | None = None
    dtb: Path | None = None
