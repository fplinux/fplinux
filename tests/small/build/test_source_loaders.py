# SPDX-License-Identifier: GPL-2.0-only
"""Exercise loader source projection, artifact wrapping and causal reuse with a fake toolchain."""

from __future__ import annotations

import hashlib
import io
import struct
import subprocess
import tarfile
import tomllib
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import pytest
from fplinux_cli import common
from fplinux_cli.build import inputs as build_inputs
from fplinux_cli.build import loaders
from fplinux_cli.build import process as build_process

from tests import ROOT


def _elf(*, bits: int, machine: int, entry: int) -> bytes:
    header = bytearray(64)
    header[:6] = b"\x7fELF" + bytes((2 if bits == 64 else 1, 1))
    struct.pack_into("<H", header, 18, machine)
    struct.pack_into("<Q" if bits == 64 else "<I", header, 24, entry)
    return bytes(header)


@dataclass
class LoaderCheckout:
    """Own tiny source archives and the external compiler boundary they replace."""

    root: Path
    work: Path
    config: dict[str, Any]
    sources: dict[str, Any]
    invalid_entry: str | None = None
    forbid_compilation: bool = False

    def build(self) -> dict[str, tuple[str, str]]:
        """Use the public producer interface with the test's declared board and archives."""
        return loaders.build_loaders(
            self.sources,
            "phone",
            self.config,
            work=self.work,
            jobs=2,
            lock_output=self.work / "assets.lock.toml",
        )

    def run(
        self,
        command: list[str],
        *,
        cwd: Path | None = None,
    ) -> None:
        """Run real source patches and substitute only the unavailable cross toolchain."""
        if command[0] == "patch":
            subprocess.run(command, cwd=cwd, check=True, capture_output=True)
            return
        if self.forbid_compilation:
            pytest.fail("an unchanged loader recipe invoked the external compiler")
        if command[0].endswith("clang"):
            return
        if command[-1] in {"sp9820e_1h10_config", "sp9820e_1h10_defconfig"}:
            return
        output = Path(
            next(part for part in command if part.startswith(("O=", "BUILD_BASE="))).split("=", 1)[
                1
            ]
        )
        if "-C" in command:
            source = Path(command[command.index("-C") + 1])
            role = "fdl2"
            elf, binary, map_file = (
                output / "u-boot",
                output / "u-boot-dtb.bin",
                output / "u-boot.map",
            )
            bits, machine, entry = 32, 40, 0x9F000000
        else:
            assert cwd is not None
            source = cwd
            if command[-1] == "fdl1":
                role = "fdl1"
                elf = output / "nand_fdl/fdl1.axf"
                binary = output / "nand_fdl/fdl1.bin"
                map_file = output / "nand_fdl/fdl1.map"
                bits, machine, entry = 64, 183, 0x5200
                assert not (source / "imgheaderinsert").exists()
                assert not (source / "secure/sansa/lib/libdx_sbrom.a").exists()
            else:
                role = "sml"
                base = output / "qin1splus/release"
                elf, binary, map_file = (
                    base / "bl31/bl31.elf",
                    base / "bl31.bin",
                    base / "bl31/bl31.map",
                )
                bits, machine, entry = 64, 183, 0x8E000000
                assert (source / "plat/qin1splus/platform.mk").read_bytes() == b"board port\n"
        if self.invalid_entry == role:
            entry += 4
        for path, data in (
            (elf, _elf(bits=bits, machine=machine, entry=entry)),
            (binary, role.encode() + b":" + (source / "component.txt").read_bytes()),
            (map_file, b"linked output\n"),
        ):
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)


@pytest.fixture
def loader_checkout(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> LoaderCheckout:
    """Control downloaded sources and compilers while retaining production projection and cache."""
    source_root = tmp_path / "targets/phone/loader"
    source_root.mkdir(parents=True)
    component_patch = (ROOT / "tests/fixtures/source-loaders/component.patch").read_bytes()
    for name in ("chipram.patch", "u-boot.patch", "trusted-firmware.patch"):
        (source_root / name).write_bytes(component_patch)
    (source_root / "bl31").mkdir()
    (source_root / "bl31/platform.mk").write_bytes(b"board port\n")
    sources: dict[str, Any] = {}
    for name in ("chipram", "u_boot", "trusted_firmware"):
        archive = tmp_path / f"cache/downloads/loaders/{name}.tar.gz"
        archive.parent.mkdir(parents=True, exist_ok=True)
        with tarfile.open(archive, "w:gz") as stream:
            for relative, data in (
                ("component.txt", b"source\n"),
                ("imgheaderinsert", b"unavailable executable"),
                ("secure/sansa/lib/libdx_sbrom.a", b"unavailable archive"),
            ):
                member = tarfile.TarInfo(f"{name}-source/{relative}")
                member.size = len(data)
                stream.addfile(member, io.BytesIO(data))
        sources[name] = {
            "archive_url": f"https://example.invalid/{name}.tar.gz",
            "archive_sha256": hashlib.sha256(archive.read_bytes()).hexdigest(),
            "archive_prefix": f"{name}-source",
        }
    config: dict[str, Any] = {
        "loader": {
            "fdl1_load": 0x5000,
            "fdl2_load": 0x9EFFFE00,
            "sml_load": 0x8E000000,
            "build": {
                "source": "loader",
                "fdl1_source": "chipram",
                "fdl2_source": "u_boot",
                "sml_source": "trusted_firmware",
            },
        },
        "bootstrap": {"layout": {"entry_load": 0x80002060}},
        "rootfs": {"packages": ["example"]},
    }
    checkout = LoaderCheckout(tmp_path, tmp_path / "work", config, sources)
    monkeypatch.setattr(common, "ROOT", tmp_path)
    monkeypatch.setattr(build_inputs, "CACHE", tmp_path / "cache")
    monkeypatch.setattr(build_process, "run", checkout.run)
    monkeypatch.setenv("FPLINUX_ENVIRONMENT_RECIPE", "a" * 64)
    monkeypatch.setenv("FPLINUX_ENVIRONMENT_CONTENT", "b" * 64)
    return checkout


def test_source_loaders_publish_dhtb_downloaders_raw_monitor_and_matching_lock(
    loader_checkout: LoaderCheckout,
) -> None:
    """The consumer receives integrity-checked downloader payloads and an unwrapped monitor."""
    outputs = loader_checkout.build()

    assert set(outputs) == {"fdl1", "fdl2", "sml"}
    for role in ("fdl1", "fdl2", "sml"):
        payload = role.encode() + b":board-a\n"
        data = (loader_checkout.work / "assets" / f"{role}.bin").read_bytes()
        if role == "sml":
            assert data == payload
        else:
            assert data[:8] == b"DHTB\x01\0\0\0"
            assert data[8:40] == hashlib.sha256(payload).digest()
            assert data[0x28:0x30] == bytes(8)
            assert struct.unpack_from("<I", data, 0x30)[0] == len(payload)
            assert data[0x34:512] == bytes(460)
            assert data[512:] == payload
        assert outputs[role] == (f"{role}.bin", hashlib.sha256(data).hexdigest())
    lock = tomllib.loads((loader_checkout.work / "assets.lock.toml").read_text())
    assert {record["role"]: record["sha256"] for record in lock["source"]} == {
        role: digest for role, (_path, digest) in outputs.items()
    }


def test_loader_recipe_reuses_outputs_after_unrelated_rootfs_change(
    loader_checkout: LoaderCheckout,
) -> None:
    """A profile's package selection does not rebuild its unchanged early boot chain."""
    before = loader_checkout.build()
    loader_checkout.config["rootfs"]["packages"].append("another-example")
    loader_checkout.forbid_compilation = True

    assert loader_checkout.build() == before


def test_loader_source_patch_change_rebuilds_the_uploaded_payload(
    loader_checkout: LoaderCheckout,
) -> None:
    """Changed board code cannot reuse a downloader produced by the previous patch."""
    before = loader_checkout.build()
    patch = loader_checkout.root / "targets/phone/loader/chipram.patch"
    patch.write_bytes(patch.read_bytes().replace(b"+board-a", b"+board-b"))

    after = loader_checkout.build()

    assert after["fdl1"] != before["fdl1"]
    assert (loader_checkout.work / "assets/fdl1.bin").read_bytes()[512:] == b"fdl1:board-b\n"


@pytest.mark.parametrize("role", ["fdl1", "fdl2", "sml"])
def test_loader_build_rejects_a_linked_entry_outside_declared_placement(
    loader_checkout: LoaderCheckout, role: str
) -> None:
    """A source configuration mismatch cannot publish an image at the wrong execution address."""
    loader_checkout.invalid_entry = role

    with pytest.raises(SystemExit, match="ELF entry or architecture differs"):
        loader_checkout.build()

    assert not (loader_checkout.work / "assets").exists()
