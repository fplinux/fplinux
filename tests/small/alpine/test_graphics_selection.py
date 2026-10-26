# SPDX-License-Identifier: GPL-2.0-only
"""Target package selection keeps optional GPU delivery on GPU-capable phones."""

from __future__ import annotations

import pytest
from fplinux_cli.alpine import selection
from fplinux_cli.manifests import paths, platforms, targets

TARGET_PROFILES = [
    pytest.param(target, profile, gpu_packages, id=f"{target}-{profile}")
    for target, gpu_packages in (
        ("bq-2830l-tank-connect-max", False),
        ("inoi-240-modern-4g", False),
        ("inoi-244-modern-4g", False),
        ("maxvi-k15n-4g", False),
        ("nokia-110-4g", False),
        ("nokia-ta1618", False),
        ("uniwa-v1000-4g", False),
        ("xiaomi-qin-1s-plus", True),
    )
    for profile in paths.discover_profiles(target)
]


@pytest.mark.parametrize("build_type", ["release", "debug"])
@pytest.mark.parametrize(("target", "profile", "gpu_packages"), TARGET_PROFILES)
def test_target_delivery_keeps_cpu_applications_and_selects_gpu_packages_only_for_qin(
    target: str, profile: str, build_type: str, *, gpu_packages: bool
) -> None:
    """Real target compositions build GBM everywhere and full Mesa only for Qin."""
    config = targets.load_target(target, profile, build_type=build_type)
    platform = platforms.load_platform(config["platform"])
    rootfs = selection.selected_packages(platform, config)
    bundle = selection.bundle_packages(platform, config, rootfs)
    order = selection.aport_build_order((*rootfs, *bundle))

    assert {"fplinux-showcase", "fplinux-tyrquake"} <= set(bundle)
    assert "fplinux-mesa-gbm" in order
    assert order.index("fplinux-mesa-gbm") < order.index("fplinux-libweston")
    assert order.index("fplinux-mesa-gbm") < order.index("fplinux-showcase")
    assert order.index("fplinux-mesa-gbm") < order.index("fplinux-tyrquake")
    if build_type == "debug":
        assert "fplinux-debug-tools" in rootfs
        assert order.index("fplinux-mesa-gbm") < order.index("fplinux-debug-tools")

    gpu_addons = {"fplinux-showcase-gpu", "fplinux-tyrquake-gpu"}
    if gpu_packages:
        assert gpu_addons <= set(bundle)
        assert {"fplinux-mesa", "fplinux-libweston-gl"} <= set(rootfs)
        assert order.index("fplinux-mesa-gbm") < order.index("fplinux-mesa")
        assert order.index("fplinux-mesa") < order.index("fplinux-libweston-gl")
    else:
        assert gpu_addons.isdisjoint((*rootfs, *bundle))
        assert "fplinux-mesa" not in order
        assert "fplinux-libweston-gl" not in order
