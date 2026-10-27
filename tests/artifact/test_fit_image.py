# SPDX-License-Identifier: GPL-2.0-only
"""Actual-artifact tests for profile-owned native U-Boot FIT images."""

from __future__ import annotations

import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import pytest
from fplinux_cli.build.storage import fit as fit_image
from fplinux_cli.common import sha256_file

from tests import ROOT
from tests.process import run_process

ZIMAGE = b"FPLinux test zImage payload\n"
DTB = b"FPLinux test DTB payload\n"


@pytest.fixture(scope="module")
def installed_fit_tools() -> tuple[Path, Path]:
    """Resolve the two required FIT tools from the pinned quality image."""
    resolved: list[Path] = []
    for name in ("mkimage", "dumpimage"):
        location = shutil.which(name)
        if location is None:
            pytest.fail(f"quality image lacks required FIT tool: {name}")
        resolved.append(Path(location))
    tools = (resolved[0], resolved[1])
    for tool in tools:
        try:
            result = run_process(
                [str(tool), "-V"],
                name=f"{tool.name} version check",
                timeout=30,
            )
        except OSError as error:
            pytest.fail(f"quality FIT tool cannot run: {tool}: {error}")
        if result.returncode != 0:
            pytest.fail(f"quality FIT tool self-test failed: {tool}: {result.stderr}")
    return tools


def _copy_declared_tool(root: Path, source: Path) -> Path:
    """Isolate a supplied binary from concurrent cache publication."""
    destination = root / source.name
    try:
        shutil.copyfile(source, destination)
        destination.chmod(0o755)
    except OSError as error:
        pytest.fail(f"cannot copy quality FIT tool {source}: {error}")
    return destination


@dataclass(frozen=True)
class FitInputs:
    """Declared tools, payloads and profile policy used by one FIT build."""

    root: Path
    mkimage: Path
    dumpimage: Path
    zimage: Path
    dtb: Path
    output: Path
    spec: dict[str, Any]
    tools_receipt: dict[str, str]

    def plan(self) -> fit_image.FitPlan:
        """Describe the current fixture inputs through the production recipe."""
        return fit_image.create_plan(
            "nokia-ta1618",
            "Nokia 3210 4G (TA-1618)",
            self.spec,
            zimage=self.zimage,
            dtb=self.dtb,
            tools_receipt=self.tools_receipt,
        )

    def build(
        self,
        plan: fit_image.FitPlan | None = None,
        *,
        mkimage: Path | None = None,
    ) -> Path:
        """Publish the current FIT through the declared U-Boot tools."""
        return fit_image.build(
            mkimage=self.mkimage if mkimage is None else mkimage,
            dumpimage=self.dumpimage,
            zimage=self.zimage,
            dtb=self.dtb,
            output=self.output,
            plan=self.plan() if plan is None else plan,
        )

    def read_fit(self, fit: Path, node: str, name: str, value_type: str) -> str:
        """Read one FIT property with dtc's fdtget, independent of the producer."""
        try:
            result = run_process(
                ["fdtget", "-t", value_type, str(fit), node, name],
                name=f"fdtget {node} {name}",
                timeout=30,
            )
        except OSError as error:
            pytest.fail(f"quality image cannot run fdtget: {error}")
        assert (result.returncode) == (0), result.stderr
        return result.stdout.strip()

    def fit_payload(self, fit: Path, node: str) -> bytes:
        """Decode the byte-wise hexadecimal fdtget dump of one embedded image."""
        dump = self.read_fit(fit, node, "data", "bx")
        return bytes(int(value, 16) for value in dump.split())


@pytest.fixture
def fit_inputs(tmp_path: Path, installed_fit_tools: tuple[Path, Path]) -> FitInputs:
    """Copy declared U-Boot tools and create isolated FIT inputs."""
    root = tmp_path
    source_mkimage, source_dumpimage = installed_fit_tools
    mkimage = _copy_declared_tool(root, source_mkimage)
    dumpimage = _copy_declared_tool(root, source_dumpimage)
    zimage = root / "zImage"
    dtb = root / "linux.dtb"
    output = root / "output"
    zimage.write_bytes(ZIMAGE)
    dtb.write_bytes(DTB)
    spec: dict[str, Any] = {
        "kind": "sha256",
        "filename": "FPLINUX.ITB",
        "kernel_load": 0x82000000,
        "kernel_entry": 0x82000000,
        "fdt_load": 0x83E00000,
    }
    tools_receipt = {"recipe": "a" * 64, "sha256": "b" * 64}
    return FitInputs(root, mkimage, dumpimage, zimage, dtb, output, spec, tools_receipt)


class FitImageTests:
    """Build and reuse a FIT with the quality image's pinned U-Boot tools."""

    def test_builds_a_fit_with_the_declared_payloads_and_addresses(
        self, fit_inputs: FitInputs
    ) -> None:
        """The dtc tools read back the payloads, load addresses and default configuration."""
        plan = fit_inputs.plan()
        fit = fit_inputs.build(plan)

        assert (fit) == (fit_inputs.output / "FPLINUX.ITB")
        assert (fit_inputs.read_fit(fit, "/images/kernel", "load", "x")) == ("82000000")
        assert (fit_inputs.read_fit(fit, "/images/kernel", "entry", "x")) == ("82000000")
        assert (fit_inputs.read_fit(fit, "/images/fdt", "load", "x")) == ("83e00000")
        assert (fit_inputs.fit_payload(fit, "/images/kernel")) == (ZIMAGE)
        assert (fit_inputs.fit_payload(fit, "/images/fdt")) == (DTB)
        assert (fit_inputs.read_fit(fit, "/configurations", "default", "s")) == ("nokia-ta1618")
        configuration = "/configurations/nokia-ta1618"
        assert (fit_inputs.read_fit(fit, configuration, "kernel", "s")) == ("kernel")
        assert (fit_inputs.read_fit(fit, configuration, "fdt", "s")) == ("fdt")
        assert fit_image.cache_hit(fit_inputs.output, plan)

    def test_changed_zimage_misses_then_rebuilds_the_fit(self, fit_inputs: FitInputs) -> None:
        """One declared kernel input change revokes only this FIT cache entry."""
        first = fit_inputs.plan()
        fit_inputs.build(first)
        before = sha256_file(fit_inputs.output / fit_inputs.spec["filename"])

        fit_inputs.zimage.write_bytes(b"FPLinux changed zImage payload\n")
        changed = fit_inputs.plan()

        assert not (fit_image.cache_hit(fit_inputs.output, changed))
        rebuilt = fit_inputs.build(changed)
        assert fit_image.cache_hit(fit_inputs.output, changed)
        assert (before) != (sha256_file(rebuilt))

    def test_unrelated_file_keeps_a_complete_cache_hit(self, fit_inputs: FitInputs) -> None:
        """A sibling outside the declared kernel and DTB inputs cannot rebuild FIT."""
        fit_inputs.build()
        receipt = (fit_inputs.output / fit_image.RECEIPT_NAME).read_bytes()
        (fit_inputs.root / "unrelated-host-note").write_text("not a FIT input\n", encoding="utf-8")

        after = fit_inputs.plan()
        assert fit_image.cache_hit(fit_inputs.output, after)
        assert (fit_inputs.build(after, mkimage=fit_inputs.root / "must-not-run")) == (
            fit_inputs.output / fit_inputs.spec["filename"]
        )
        assert ((fit_inputs.output / fit_image.RECEIPT_NAME).read_bytes()) == (receipt)

    def test_missing_and_tampered_fit_are_rebuilt(self, fit_inputs: FitInputs) -> None:
        """Absent or changed published bytes cannot remain reusable."""
        plan = fit_inputs.plan()
        fit = fit_inputs.build(plan)
        expected = fit.read_bytes()
        fit.unlink()
        assert not (fit_image.cache_hit(fit_inputs.output, plan))

        assert (fit_inputs.build(plan).read_bytes()) == (expected)
        fit.write_bytes(b"tampered\n")
        assert not (fit_image.cache_hit(fit_inputs.output, plan))

        assert (fit_inputs.build(plan).read_bytes()) == (expected)
        assert fit_image.cache_hit(fit_inputs.output, plan)

    def test_tool_failure_preserves_prior_complete_fit(self, fit_inputs: FitInputs) -> None:
        """A failed new build cannot replace the previous verified publication."""
        plan = fit_inputs.plan()
        fit = fit_inputs.build(plan)
        prior_fit = fit.read_bytes()
        prior_receipt = (fit_inputs.output / fit_image.RECEIPT_NAME).read_bytes()
        failing = fit_inputs.root / "failing-mkimage"
        shutil.copyfile(ROOT / "tests/artifact/fixtures/fit-image/failing_image_tool.sh", failing)
        failing.chmod(0o755)
        fit_inputs.zimage.write_bytes(b"FPLinux changed zImage payload\n")
        changed = fit_inputs.plan()

        with pytest.raises(subprocess.CalledProcessError) as failure:
            fit_inputs.build(changed, mkimage=failing)

        assert (failure.value.returncode) == (19)
        assert (fit.read_bytes()) == (prior_fit)
        assert ((fit_inputs.output / fit_image.RECEIPT_NAME).read_bytes()) == (prior_receipt)
        assert fit_image.cache_hit(fit_inputs.output, plan)
