# SPDX-License-Identifier: GPL-2.0-only
"""Host sdboot admission with real libfdt and stubbed MMC/fs/bootm boundaries."""

from __future__ import annotations

import hashlib
import struct
from typing import TYPE_CHECKING, ClassVar

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path

HARNESS = ROOT / "tests/host_tool/ums9117"
UBOOT = ROOT / "platforms/ums9117/uboot"


class SdbootFitAdmissionTests:
    """Admit embedded DTBs and hand off their active contents after bootm preparation."""

    executable: ClassVar[Path]

    @pytest.fixture(scope="class", autouse=True)
    @classmethod
    def _compiled_peer(cls, tmp_path_factory: pytest.TempPathFactory) -> Iterator[None]:
        """Link sdboot once with the controlled host boundaries."""
        with c_build_directory(tmp_path_factory, "ums9117_sdboot") as temporary:
            executable = temporary / "sdboot"
            compilation = compile_c(
                executable,
                [
                    "-std=gnu11",
                    "-D_GNU_SOURCE",
                    "-Wall",
                    "-Werror",
                    "-Wno-int-to-pointer-cast",
                    f"-I{HARNESS / 'sdboot-compat'}",
                    f"-I{HARNESS / 'ums9117-sdio-compat'}",
                    f"-I{UBOOT}",
                    f"-I{ROOT / 'platforms/ums9117/common'}",
                    str(HARNESS / "ums9117-sdboot.c"),
                    str(UBOOT / "ums9117-sdboot.c"),
                    "-l:libfdt.so.1",
                ],
                name="compile sdboot host consumer",
                check=False,
            )
            assert compilation.returncode == 0, compilation.stdout + compilation.stderr
            cls.executable = executable
            yield

    @pytest.mark.parametrize("defect", ["none", "bad-magic", "bad-size"])
    def test_embedded_dtb_alignment_and_invalid_payloads(
        self, tmp_path: Path, defect: str
    ) -> None:
        """Accept both FIT alignments; report DTB refusals as BOOTM failures with MMC released."""
        work = tmp_path
        dtb = self.make_dtb(work)
        kernel = bytearray(64)
        struct.pack_into("<III", kernel, 0x24, 0x016F2818, 0, 64)
        (work / "zImage").write_bytes(kernel)
        (work / "kernel.sha256").write_bytes(hashlib.sha256(kernel).digest())
        if defect == "none":
            alignments = set()
            for padding in ("pad", "padding"):
                fit, alignment = self.make_fit(work, dtb, padding)
                alignments.add(alignment)
                self.check_admission(self.executable, fit, alignment, 0)
            assert (alignments) == ({0, 4})
        else:
            # The stubbed FINDOTHER refuses the bad header, as the pinned U-Boot
            # fit_image_load() does; sdboot itself rejects the size mismatch.
            payload = (
                b"\0\0\0\0" + dtb[4:]
                if defect == "bad-magic"
                else dtb[:4] + struct.pack(">I", len(dtb) + 4) + dtb[8:]
            )
            fit, alignment = self.make_fit(work, payload, "padding")
            self.check_admission(self.executable, fit, alignment, 4)

    def test_prepared_dtb_padding_does_not_reach_handoff(self, tmp_path: Path) -> None:
        """Preserve the RNG property while excluding a stale name in unused DTB padding."""
        dtb = self.make_dtb(tmp_path)
        kernel = bytearray(64)
        struct.pack_into("<III", kernel, 0x24, 0x016F2818, 0, 64)
        (tmp_path / "zImage").write_bytes(kernel)
        (tmp_path / "kernel.sha256").write_bytes(hashlib.sha256(kernel).digest())
        fit, alignment = self.make_fit(tmp_path, dtb, "padding")
        result = run_process(
            [str(self.executable), str(fit), str(alignment), "0", "padded-fdt"],
            name="run sdboot with prepared DTB padding",
            timeout=10,
        )
        assert result.returncode == 0, result.stdout + result.stderr

    def make_dtb(self, work: Path) -> bytes:
        """Compile a standalone DTB using the declared quality-runtime dtc."""
        source = work / "linux.dts"
        source.write_text(
            '/dts-v1/; / { compatible = "test,board"; '
            "chosen { rng-seed = [01 23 45 67 89 ab cd ef]; }; };\n"
        )
        output = work / "linux.dtb"
        self.compile_tree(source, output)
        return output.read_bytes()

    def make_fit(self, work: Path, dtb: bytes, padding: str) -> tuple[Path, int]:
        """Vary a normal FIT string to exercise its two possible data alignments."""
        (work / "linux.dtb").write_bytes(dtb)
        (work / "fdt.sha256").write_bytes(hashlib.sha256(dtb).digest())
        source = work / "fit.its"
        template = (HARNESS / "fixtures/sdboot-fit.dts").read_text()
        source.write_text(template.replace("ALIGNMENT", padding))
        output = work / "FPLINUX.ITB"
        self.compile_tree(source, output)
        alignment = output.read_bytes().index(dtb) % 8
        return output, alignment

    def compile_tree(self, source: Path, output: Path) -> None:
        """Compile fixture bytes without touching project build outputs."""
        run_process(
            ["dtc", "-I", "dts", "-O", "dtb", "-o", str(output), str(source)],
            name="compile sdboot fixture tree",
            timeout=30,
            check=True,
        )

    def check_admission(self, executable: Path, fit: Path, alignment: int, failure: int) -> None:
        """Observe the registered command's final handoff or failure callback."""
        result = run_process(
            [str(executable), str(fit), str(alignment), str(failure)],
            name="run sdboot host consumer",
            timeout=10,
        )
        assert (result.returncode) == (0), result.stdout + result.stderr
