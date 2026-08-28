# SPDX-License-Identifier: GPL-2.0-only
# ruff: noqa: EM101 -- validation failures use exact artifact diagnostics.
"""Build and verify one native U-Boot FIT from exact kernel artifacts."""

from __future__ import annotations

import hashlib
import shutil
import subprocess
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from . import artifact_state, build_env
from .artifact_state import (
    canonical_json_bytes,
    receipt_matches,
    regular_file_record,
    require_lowercase_sha256,
    write_canonical_json,
)
from .common import sha256_file
from .device_tree import exact_path_properties, parse_nul_string

RECEIPT_NAME = ".fplinux-fit-receipt.json"
_ITS_NAME = "FPLINUX.its"
_COMMAND_TIMEOUT_SECONDS = 120


class FitImageError(RuntimeError):
    """A FIT recipe, tool, artifact or receipt is invalid."""


@dataclass(frozen=True)
class FitPlan:
    """Exact FIT inputs whose verified result may be reused."""

    recipe: str
    target: str
    display_name: str
    spec: dict[str, Any]
    zimage: dict[str, int | str]
    dtb: dict[str, int | str]
    tools_receipt: dict[str, str]
    its: bytes


def _dts_string(value: str) -> str:
    if not value or any(ord(character) < 0x20 for character in value):
        raise FitImageError("FIT description contains an invalid character")
    return value.replace("\\", "\\\\").replace('"', '\\"')


def _its_source(target: str, display_name: str, spec: dict[str, Any]) -> bytes:
    name = _dts_string(display_name)
    return f"""/dts-v1/;

/ {{
\tdescription = "FPLinux {name}";
\t#address-cells = <1>;

\timages {{
\t\tkernel {{
\t\t\tdescription = "Linux zImage";
\t\t\tdata = /incbin/("zImage");
\t\t\ttype = "kernel";
\t\t\tarch = "arm";
\t\t\tos = "linux";
\t\t\tcompression = "none";
\t\t\tload = <0x{spec["kernel_load"]:08x}>;
\t\t\tentry = <0x{spec["kernel_entry"]:08x}>;

\t\t\thash {{
\t\t\t\talgo = "sha256";
\t\t\t}};
\t\t}};

\t\tfdt {{
\t\t\tdescription = "Linux device tree";
\t\t\tdata = /incbin/("linux.dtb");
\t\t\ttype = "flat_dt";
\t\t\tarch = "arm";
\t\t\tcompression = "none";
\t\t\tload = <0x{spec["fdt_load"]:08x}>;

\t\t\thash {{
\t\t\t\talgo = "sha256";
\t\t\t}};
\t\t}};
\t}};

\tconfigurations {{
\t\tdefault = "{target}";

\t\t{target} {{
\t\t\tdescription = "{name}";
\t\t\tkernel = "kernel";
\t\t\tfdt = "fdt";
\t\t}};
\t}};
}};
""".encode()


def create_plan(  # noqa: PLR0913, PLR0917 -- causal inputs remain separate.
    target: str,
    display_name: str,
    spec: dict[str, Any],
    zimage: Path,
    dtb: Path,
    tools_receipt: dict[str, str],
) -> FitPlan:
    """Create the causal recipe for one unsigned, SHA-256-hashed FIT."""
    if spec.get("kind") != "sha256":
        raise FitImageError("FIT kind is invalid")
    if set(tools_receipt) != {"recipe", "sha256"}:
        raise FitImageError("U-Boot tools receipt identity is invalid")
    normalized_tools = {
        "recipe": require_lowercase_sha256(
            tools_receipt.get("recipe"), "U-Boot tools recipe", FitImageError
        ),
        "sha256": require_lowercase_sha256(
            tools_receipt.get("sha256"), "U-Boot tools receipt SHA-256", FitImageError
        ),
    }
    its = _its_source(target, display_name, spec)
    zimage_record = regular_file_record(zimage, "FIT file", FitImageError)
    dtb_record = regular_file_record(dtb, "FIT file", FitImageError)
    manifest = {
        "target": target,
        "display_name": display_name,
        "spec": spec,
        "zimage": zimage_record,
        "dtb": dtb_record,
        "tools_receipt": normalized_tools,
        "source_date_epoch": build_env.SOURCE_DATE_EPOCH,
        "its_sha256": hashlib.sha256(its).hexdigest(),
        "implementation": {
            "fit_image": regular_file_record(Path(__file__), "FIT file", FitImageError),
            "artifact_state": regular_file_record(
                Path(artifact_state.__file__), "FIT file", FitImageError
            ),
            "device_tree": regular_file_record(
                Path(__file__).with_name("device_tree.py"), "FIT file", FitImageError
            ),
        },
    }
    recipe = hashlib.sha256(canonical_json_bytes(manifest)).hexdigest()
    return FitPlan(
        recipe,
        target,
        display_name,
        dict(spec),
        zimage_record,
        dtb_record,
        normalized_tools,
        its,
    )


def _receipt_payload(plan: FitPlan, output: Path) -> dict[str, object]:
    return {
        "recipe": plan.recipe,
        "tools_receipt": plan.tools_receipt,
        "fit": regular_file_record(output / str(plan.spec["filename"]), "FIT file", FitImageError),
    }


def cache_hit(output: Path, plan: FitPlan) -> bool:
    """Return true only for the exact FIT artifact authorized by the receipt."""
    try:
        expected = _receipt_payload(plan, output)
    except FitImageError:
        return False
    return receipt_matches(output / RECEIPT_NAME, expected)


def receipt_identity(output: Path, plan: FitPlan) -> dict[str, str]:
    """Return the identity of one fully rechecked FIT receipt."""
    if not cache_hit(output, plan):
        raise FitImageError("FIT receipt is missing, stale or invalid")
    return {"recipe": plan.recipe, "sha256": sha256_file(output / RECEIPT_NAME)}


def _cell(value: bytes, name: str) -> int:
    if len(value) != 4:
        raise FitImageError(f"FIT {name} must be one 32-bit cell")
    return int.from_bytes(value, "big")


def _string(properties: dict[str, bytes], name: str, expected: str) -> None:
    if name not in properties or parse_nul_string(properties[name], f"FIT {name}") != expected:
        raise FitImageError(f"FIT {name} differs from its recipe")


def _verify_fit(fit: Path, plan: FitPlan, zimage: Path, dtb: Path) -> None:
    configuration = f"/configurations/{plan.target}"
    properties = exact_path_properties(
        fit,
        (
            "/",
            "/images/kernel",
            "/images/kernel/hash",
            "/images/fdt",
            "/images/fdt/hash",
            "/configurations",
            configuration,
        ),
    )
    root = properties["/"]
    if _cell(root.get("timestamp", b""), "timestamp") != int(build_env.SOURCE_DATE_EPOCH):
        raise FitImageError("FIT timestamp differs from SOURCE_DATE_EPOCH")
    _string(root, "description", f"FPLinux {plan.display_name}")
    kernel = properties["/images/kernel"]
    fdt = properties["/images/fdt"]
    kernel_data = kernel.get("data", b"")
    fdt_data = fdt.get("data", b"")
    if (
        kernel_data != zimage.read_bytes()
        or regular_file_record(zimage, "FIT file", FitImageError) != plan.zimage
    ):
        raise FitImageError("FIT kernel payload differs from its recipe")
    if (
        fdt_data != dtb.read_bytes()
        or regular_file_record(dtb, "FIT file", FitImageError) != plan.dtb
    ):
        raise FitImageError("FIT device-tree payload differs from its recipe")
    for node, data in (("kernel", kernel_data), ("fdt", fdt_data)):
        digest = properties[f"/images/{node}/hash"]
        _string(digest, "algo", "sha256")
        if digest.get("value") != hashlib.sha256(data).digest():
            raise FitImageError(f"FIT {node} SHA-256 value is invalid")
    _string(kernel, "type", "kernel")
    _string(kernel, "arch", "arm")
    _string(kernel, "os", "linux")
    _string(kernel, "compression", "none")
    if _cell(kernel.get("load", b""), "kernel load") != plan.spec["kernel_load"]:
        raise FitImageError("FIT kernel load address differs from its recipe")
    if _cell(kernel.get("entry", b""), "kernel entry") != plan.spec["kernel_entry"]:
        raise FitImageError("FIT kernel entry address differs from its recipe")
    _string(fdt, "type", "flat_dt")
    _string(fdt, "arch", "arm")
    _string(fdt, "compression", "none")
    if _cell(fdt.get("load", b""), "FDT load") != plan.spec["fdt_load"]:
        raise FitImageError("FIT FDT load address differs from its recipe")
    _string(properties["/configurations"], "default", plan.target)
    _string(properties[configuration], "description", plan.display_name)
    _string(properties[configuration], "kernel", "kernel")
    _string(properties[configuration], "fdt", "fdt")


def _run_dumpimage(dumpimage: Path, fit: Path, temporary: Path, zimage: Path, dtb: Path) -> None:
    listed = subprocess.run(
        [str(dumpimage), "-l", str(fit)],
        capture_output=True,
        text=True,
        env=build_env.build_environment(),
        check=False,
        timeout=_COMMAND_TIMEOUT_SECONDS,
    )
    if listed.returncode != 0:
        raise FitImageError(listed.stderr.strip() or "dumpimage rejected the FIT")
    for position, source, name in ((0, zimage, "zImage"), (1, dtb, "linux.dtb")):
        extracted = temporary / f"extracted-{name}"
        result = subprocess.run(
            [
                str(dumpimage),
                "-T",
                "flat_dt",
                "-p",
                str(position),
                "-o",
                str(extracted),
                str(fit),
            ],
            capture_output=True,
            text=True,
            env=build_env.build_environment(),
            check=False,
            timeout=_COMMAND_TIMEOUT_SECONDS,
        )
        if result.returncode != 0 or not extracted.is_file():
            raise FitImageError(result.stderr.strip() or f"dumpimage did not extract {name}")
        if extracted.read_bytes() != source.read_bytes():
            raise FitImageError(f"dumpimage extracted different {name} bytes")


def build(  # noqa: PLR0913, PLR0917 -- tool and artifact paths remain explicit.
    mkimage: Path,
    dumpimage: Path,
    zimage: Path,
    dtb: Path,
    output: Path,
    plan: FitPlan,
) -> Path:
    """Build and atomically publish one verified deterministic FIT."""
    if cache_hit(output, plan):
        return output / str(plan.spec["filename"])
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=output.parent, prefix=".fit-image.") as name:
        temporary = Path(name)
        shutil.copyfile(zimage, temporary / "zImage")
        shutil.copyfile(dtb, temporary / "linux.dtb")
        (temporary / _ITS_NAME).write_bytes(plan.its)
        fit = temporary / str(plan.spec["filename"])
        subprocess.run(
            [str(mkimage), "-f", _ITS_NAME, fit.name],
            cwd=temporary,
            env=build_env.build_environment(),
            check=True,
            timeout=_COMMAND_TIMEOUT_SECONDS,
        )
        fit.chmod(0o644)
        _verify_fit(fit, plan, zimage, dtb)
        _run_dumpimage(dumpimage, fit, temporary, zimage, dtb)

        staging = temporary / "publish"
        staging.mkdir()
        shutil.copyfile(fit, staging / fit.name)
        (staging / fit.name).chmod(0o644)
        write_canonical_json(staging / RECEIPT_NAME, _receipt_payload(plan, staging), mode=0o644)
        if output.exists():
            if output.is_symlink() or not output.is_dir():
                raise FitImageError(f"FIT output is invalid: {output}")
            shutil.rmtree(output)
        staging.replace(output)
    if not cache_hit(output, plan):
        raise FitImageError("published FIT receipt is not reusable")
    return output / str(plan.spec["filename"])
