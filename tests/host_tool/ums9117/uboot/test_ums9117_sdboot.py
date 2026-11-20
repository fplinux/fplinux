# SPDX-License-Identifier: GPL-2.0-only
"""Host sdboot admission with real libfdt and stubbed MMC/fs/bootm boundaries."""

from __future__ import annotations

import hashlib
import struct
from typing import TYPE_CHECKING

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process

if TYPE_CHECKING:
    from collections.abc import Iterator
    from pathlib import Path

HARNESS = ROOT / "tests/host_tool/ums9117/uboot"
UBOOT = ROOT / "platforms/ums9117/uboot"


class SdbootFitAdmissionTests:
    """Admit embedded DTBs and hand off their active contents after bootm preparation."""

    @pytest.fixture(scope="class")
    @classmethod
    def executable(cls, tmp_path_factory: pytest.TempPathFactory) -> Iterator[Path]:
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
            yield executable

    @pytest.mark.parametrize("defect", ["none", "bad-magic", "bad-size"])
    def test_embedded_dtb_alignment_and_invalid_payloads(
        self, executable: Path, tmp_path: Path, defect: str
    ) -> None:
        """Accept both FIT alignments; report DTB refusals as BOOTM failures with MMC released."""
        work = tmp_path
        dtb = self.make_dtb(work)
        if defect == "none":
            alignments = set()
            for padding in ("pad", "padding"):
                fit, alignment = self.make_fit(work, dtb, padding)
                alignments.add(alignment)
                self.check_admission(executable, fit, alignment, 0)
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
            self.check_admission(executable, fit, alignment, 4)

    @pytest.mark.parametrize(
        ("mode", "compatible", "status"),
        [
            ("padded-fdt", "fplinux,ums9117-musb-inherited", "okay"),
            ("autonomous", "sprd,ums9117-musb", "okay"),
        ],
    )
    def test_handoff_retains_only_the_selected_boot_properties(
        self, executable: Path, tmp_path: Path, mode: str, compatible: str, status: str
    ) -> None:
        """Keep live USB properties in USB mode and discard them on autonomous boot."""
        self.make_dtb(tmp_path)
        tree = tmp_path / "linux.dtb"
        run_process(
            ["fdtput", "-t", "s", str(tree), "/usb", "compatible", compatible],
            name="select the board USB controller",
            timeout=10,
        )
        dtb = tree.read_bytes()
        fit, alignment = self.make_fit(tmp_path, dtb, "padding")
        result = run_process(
            [str(executable), str(fit), str(alignment), "0", mode, status],
            name=f"run sdboot with {mode}",
            timeout=10,
            env={"FPLINUX_TEST_SDBOOT_SELECTOR": str(tmp_path / "active")},
        )
        assert result.returncode == 0, result.stdout + result.stderr

    @pytest.mark.parametrize(
        ("selector", "present_slot", "expected"),
        [
            (b"a\n", "a", (0, 1)),
            (b"b\n", "b", (0, 1)),
            (None, "a", (3, 0)),
            (b"A\n", "a", (3, 0)),
            (b"a\x00", "a", (3, 0)),
            (b"a\nb", "a", (3, 0)),
            (b"b\n", "a", (3, 1)),
        ],
    )
    def test_active_selector_loads_exactly_one_slot(
        self,
        executable: Path,
        tmp_path: Path,
        selector: bytes | None,
        present_slot: str,
        expected: tuple[int, int],
    ) -> None:
        """Use the selected slot once, refuse malformed state and never fall back."""
        failure, fit_opens = expected
        fit, alignment = self.make_fit(tmp_path, self.make_dtb(tmp_path), "padding")
        active = tmp_path / "active"
        if selector is None:
            active.unlink()
        else:
            active.write_bytes(selector)
        result = run_process(
            [str(executable), str(fit), str(alignment), str(failure)],
            name="run sdboot with the active selector",
            timeout=10,
            env={
                "FPLINUX_TEST_SDBOOT_SELECTOR": str(active),
                "FPLINUX_TEST_SDBOOT_FIT": f"/system/{present_slot}/FPLINUX.ITB",
            },
        )
        assert result.returncode == 0, result.stdout + result.stderr
        assert result.stdout.splitlines()[-1].endswith(f"fit_opens={fit_opens}")

    def test_selected_fit_requires_its_target_configuration(
        self, executable: Path, tmp_path: Path
    ) -> None:
        """A readable FIT selected on the card still needs the board's configuration."""
        fit, alignment = self.make_fit(
            tmp_path, self.make_dtb(tmp_path), "padding", target="inoi-240-modern-4g"
        )
        self.check_admission(executable, fit, alignment, 4)

    @pytest.mark.parametrize("image", ["kernel", "fdt", "ramdisk"])
    def test_selected_fit_requires_each_hash_check(
        self, executable: Path, tmp_path: Path, image: str
    ) -> None:
        """Refuse FIT metadata that asks U-Boot to skip a required hash check."""
        dtb = self.make_dtb(tmp_path)
        fit, _ = self.make_fit(tmp_path, dtb, "padding")
        run_process(
            ["fdtput", "-t", "x", str(fit), f"/images/{image}/hash", "uboot-ignore", "1"],
            name="add a FIT hash skip request",
            timeout=10,
            check=True,
        )
        self.check_admission(executable, fit, fit.read_bytes().index(dtb) % 8, 4)

    def make_dtb(self, work: Path) -> bytes:
        """Compile a standalone DTB using the declared quality-runtime dtc."""
        source = work / "linux.dts"
        source.write_bytes((HARNESS / "fixtures/sdboot/linux.dts").read_bytes())
        output = work / "linux.dtb"
        self.compile_tree(source, output)
        return output.read_bytes()

    def make_fit(
        self, work: Path, dtb: bytes, padding: str, *, target: str = "bq-2830l-tank-connect-max"
    ) -> tuple[Path, int]:
        """Vary a normal FIT string to exercise its two possible data alignments."""
        kernel = bytearray(64)
        struct.pack_into("<III", kernel, 0x24, 0x016F2818, 0, 64)
        (work / "zImage").write_bytes(kernel)
        (work / "kernel.sha256").write_bytes(hashlib.sha256(kernel).digest())
        (work / "active").write_bytes(b"a\n")
        (work / "linux.dtb").write_bytes(dtb)
        (work / "fdt.sha256").write_bytes(hashlib.sha256(dtb).digest())
        ramdisk = b"early firmware"
        (work / "early-firmware.cpio").write_bytes(ramdisk)
        (work / "ramdisk.sha256").write_bytes(hashlib.sha256(ramdisk).digest())
        source = work / "fit.its"
        template = (HARNESS / "fixtures/sdboot-fit.dts").read_text()
        source.write_text(template.replace("ALIGNMENT", padding).replace("TARGET", target))
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
            env={"FPLINUX_TEST_SDBOOT_SELECTOR": str(fit.parent / "active")},
        )
        assert (result.returncode) == (0), result.stdout + result.stderr
