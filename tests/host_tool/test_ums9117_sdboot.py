# SPDX-License-Identifier: GPL-2.0-only
"""Host sdboot admission with real libfdt and stubbed MMC/fs/bootm boundaries."""

from __future__ import annotations

import hashlib
import struct
import tempfile
import unittest
from pathlib import Path

from tests.process import run_process

ROOT = Path(__file__).resolve().parents[2]
HARNESS = ROOT / "tests/host_tool"
UBOOT = ROOT / "platforms/ums9117/uboot"


class SdbootFitAdmissionTests(unittest.TestCase):
    """A valid embedded DTB may be only four-byte aligned inside its FIT."""

    def test_embedded_dtb_alignment_and_invalid_payloads(self) -> None:
        """Accept both FIT alignments; reject bad DTB headers and data lengths."""
        with tempfile.TemporaryDirectory() as name:
            work = Path(name)
            executable = work / "sdboot"
            compilation = run_process(
                [
                    "cc",
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
                    "-o",
                    str(executable),
                ],
                name="compile sdboot host consumer",
                timeout=30,
            )
            self.assertEqual(
                compilation.returncode, 0, msg=compilation.stdout + compilation.stderr
            )
            dtb = self.make_dtb(work)
            kernel = bytearray(64)
            struct.pack_into("<III", kernel, 0x24, 0x016F2818, 0, 64)
            (work / "zImage").write_bytes(kernel)
            (work / "kernel.sha256").write_bytes(hashlib.sha256(kernel).digest())
            alignments = set()
            for padding in ("pad", "padding"):
                fit, alignment = self.make_fit(work, dtb, padding)
                alignments.add(alignment)
                with self.subTest(alignment=alignment):
                    self.check_admission(executable, fit, alignment, 0)
            self.assertEqual(alignments, {0, 4})
            bad_magic = b"\0\0\0\0" + dtb[4:]
            bad_size = dtb[:4] + struct.pack(">I", len(dtb) + 4) + dtb[8:]
            for label, payload in (("bad magic", bad_magic), ("wrong size", bad_size)):
                fit, alignment = self.make_fit(work, payload, "padding")
                with self.subTest(payload=label):
                    self.check_admission(executable, fit, alignment, 4)

    def make_dtb(self, work: Path) -> bytes:
        """Compile a standalone DTB using the declared quality-runtime dtc."""
        source = work / "linux.dts"
        source.write_text('/dts-v1/; / { compatible = "test,board"; };\n')
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
        self.assertEqual(result.returncode, 0, msg=result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
