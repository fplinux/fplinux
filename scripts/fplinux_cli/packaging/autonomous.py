# SPDX-License-Identifier: GPL-2.0-only
"""Collect the native tools shared by the selected archive runtimes."""

from __future__ import annotations

import io
import tempfile
import zipfile
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from fplinux_cli import common
from fplinux_cli.common import ZIP_TIMESTAMP, canonical_json_bytes, fail, sha256_bytes
from fplinux_cli.device_data.prepare import NAND_ECC_TOOL
from fplinux_cli.runtime.windows_tools import windows_tool_paths


@dataclass(frozen=True)
class AutonomousPayload:
    """Additional archive members and executable ZIP modes."""

    files: dict[str, bytes]
    executables: set[str]


def _read_tools(paths: dict[str, Path], *, host: str) -> AutonomousPayload:
    files = {}
    executables = set()
    for name, path in paths.items():
        if path.is_symlink() or not path.is_file():
            fail(f"{host} archive input is missing or invalid: {path}")
        files[name] = path.read_bytes()
        if path.suffix.lower() == ".exe" if host == "windows" else path.stat().st_mode & 0o111:
            executables.add(name)
    return AutonomousPayload(files, executables)


def _linux_ecc_sources(platform: str, *, cache: Path) -> dict[str, bytes]:
    """Deliver the verified source projection and license for the NAND corrector."""
    from fplinux_cli.build.host_tools.host import (  # noqa: PLC0415
        copy_host_project_files,
        extract_host_members,
    )
    from fplinux_cli.build.sources import source_lock_entry  # noqa: PLC0415
    from fplinux_cli.manifests.platforms import load_platform  # noqa: PLC0415
    from fplinux_cli.manifests.source_catalog import acquire_source  # noqa: PLC0415

    recipe = next(
        item for item in load_platform(platform)["host"]["tools"] if item["name"] == NAND_ECC_TOOL
    )
    sources = common.load_toml(common.ROOT / "sources.lock.toml")
    lock = source_lock_entry(sources, recipe["source_lock"])
    archive = acquire_source(common.ROOT, recipe["source_lock"], cache)
    payload = io.BytesIO()
    with tempfile.TemporaryDirectory(dir=cache, prefix=".autonomous-ecc-source-") as temporary:
        source = Path(temporary) / recipe["source_directory"]
        prefix = recipe["archive_prefix"].replace("{commit}", lock["commit"])
        extract_host_members(archive, prefix, recipe["members"], lock["files"], source)
        copy_host_project_files(source, recipe["copies"])
        license_contents = (source / "COPYING").read_bytes()
        with zipfile.ZipFile(payload, "w", compression=zipfile.ZIP_DEFLATED) as output:
            for path in sorted(source.iterdir()):
                member = zipfile.ZipInfo(f"{NAND_ECC_TOOL}/{path.name}", ZIP_TIMESTAMP)
                member.compress_type = zipfile.ZIP_DEFLATED
                output.writestr(member, path.read_bytes())
    return {
        "sources/ums9117-nand-ecc-source.zip": payload.getvalue(),
        "licenses/host-tools/ums9117-nand-ecc-COPYING.txt": license_contents,
    }


@dataclass
class HostPayloads:
    """Prepare each native tool set once while constructing a release."""

    cache: Path
    jobs: int
    linux: AutonomousPayload | None = None
    driver: AutonomousPayload | None = None
    windows: dict[tuple[str, bool], AutonomousPayload] = field(default_factory=dict)
    ecc_sources: dict[str, dict[str, bytes]] = field(default_factory=dict)

    def _linux(self, platform: str, *, has_ecc: bool) -> AutonomousPayload:
        from fplinux_cli.build.host_tools.linux_host import prepare_linux_host  # noqa: PLC0415

        if self.linux is None:
            self.linux = _read_tools(
                prepare_linux_host(cache=self.cache, jobs=self.jobs), host="linux"
            )
        files = dict(self.linux.files)
        if platform == "ums9117":
            notice = common.ROOT / "LICENSES/Unlicense.txt"
            if notice.is_symlink() or not notice.is_file():
                fail(f"host tool license is missing or invalid: {notice}")
            files["licenses/host-tools/Unlicense.txt"] = notice.read_bytes()
        if has_ecc:
            if platform not in self.ecc_sources:
                self.ecc_sources[platform] = _linux_ecc_sources(platform, cache=self.cache)
            files.update(self.ecc_sources[platform])
        return AutonomousPayload(files, self.linux.executables.copy())

    def _windows(
        self, platform: str, *, has_ecc: bool, runtime: dict[str, Any]
    ) -> AutonomousPayload:
        from fplinux_cli.build.host_tools import windows_driver  # noqa: PLC0415
        from fplinux_cli.build.host_tools.windows import prepare_windows_host  # noqa: PLC0415

        if self.driver is None:
            self.driver = _read_tools(
                windows_driver.prepare_windows_driver(cache=self.cache), host="windows"
            )
        key = platform, has_ecc
        if key not in self.windows:
            paths = prepare_windows_host(cache=self.cache, platform=platform, nand_ecc=has_ecc)
            self.windows[key] = _read_tools(paths, host="windows")
        native = self.windows[key]
        if collisions := native.files.keys() & self.driver.files.keys():
            fail(f"duplicate Windows archive path: {min(collisions)}")
        files = {**native.files, **self.driver.files}
        files["windows-host.json"] = canonical_json_bytes(
            {
                "host_tools": windows_tool_paths(runtime["host_tools"]),
                "sha256": {name: sha256_bytes(data) for name, data in sorted(files.items())},
            }
        )
        return AutonomousPayload(files, native.executables | self.driver.executables)

    def project(
        self, config: dict[str, Any], *, host: str, runtime: dict[str, Any]
    ) -> AutonomousPayload:
        """Select the host tool files and roles admitted for this runtime."""
        platform = config["platform"]
        descriptor = runtime["device_data"]
        has_ecc = descriptor is not None and descriptor["ecc_module"] is not None
        if host == "windows":
            return self._windows(platform, has_ecc=has_ecc, runtime=runtime)
        return self._linux(platform, has_ecc=has_ecc)
