# SPDX-License-Identifier: GPL-2.0-only
"""Check the FDT dependency of the adapter selected from a Qin ZIP."""

from __future__ import annotations

import sys
import zipfile
from textwrap import dedent
from typing import TYPE_CHECKING

import pytest
from fplinux_cli.runtime.release_catalog import load_catalog, materialize_bundle

from tests.host_process.packaging.autonomous_package_fixtures import (
    archive_inputs as archive_inputs,  # noqa: PLC0414 -- pytest fixture export.
)
from tests.process import run_process

if TYPE_CHECKING:
    from tests.host_process.packaging.autonomous_package_fixtures import ArchiveInputs


@pytest.mark.parametrize("archive_inputs", ["xiaomi-qin-1s-plus"], indirect=True)
def test_qin_zip_loads_its_sealed_fdt_module(archive_inputs: ArchiveInputs) -> None:
    archive_path = archive_inputs.package("linux")
    extracted = archive_inputs.root / "recipient"
    with zipfile.ZipFile(archive_path) as archive:
        archive.extractall(extracted)
    root = extracted / archive_path.stem
    bundle = materialize_bundle(
        root, archive_inputs.root / "recipient-data", load_catalog(root), archive_inputs.target
    )
    relative = "scripts/fplinux_cli/fdt.py"
    module = bundle / relative
    command = [
        sys.executable,
        "-I",
        "-S",
        "-B",
        "-c",
        dedent("""\
            import sys
            from pathlib import Path

            bundle = Path(sys.argv[1])
            sys.path.insert(0, str(bundle / "scripts"))
            from fplinux_cli.runtime.bundle import adapter_path, admit_bundle, load_adapter

            admitted = admit_bundle(bundle)
            load_adapter(bundle / adapter_path(admitted.runtime))
            print(sys.modules["fplinux_cli.fdt"].__file__)
            """),
        str(bundle),
    ]
    loaded = run_process(command, name="bundled Qin adapter", timeout=10, cwd=extracted, env={})
    assert loaded.returncode == 0, loaded.stderr
    assert loaded.stdout.strip() == str(module)
    assert module.read_bytes() == (archive_inputs.bundle / relative).read_bytes()

    module.write_bytes(module.read_bytes() + b"\n")
    changed = run_process(
        command, name="changed Qin adapter dependency", timeout=10, cwd=extracted, env={}
    )
    assert changed.returncode == 1
    assert "scripts/fplinux_cli/fdt.py SHA256 mismatch" in changed.stderr
