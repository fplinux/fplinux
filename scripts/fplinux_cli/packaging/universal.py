# SPDX-License-Identifier: GPL-2.0-only
"""Assemble one host archive with shared payloads and a catalog of boards."""

from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path
from textwrap import dedent
from typing import TYPE_CHECKING, Any

from fplinux_cli.common import canonical_json_bytes, fail, sha256_bytes
from fplinux_cli.packaging.boot_components import BOOT_COMPONENTS, exact_component_offset
from fplinux_cli.packaging.readme import append_source_companion

if TYPE_CHECKING:
    from fplinux_cli.artifacts.release_identity import ReleaseContext

CATALOG_NAME = "targets.json"
_LAUNCHERS = Path(__file__).with_name("release-launcher")
_SHARED_PATHS = (
    "common/windows/",
    "licenses/",
    "runtime/",
    "scripts/",
    "third-party-licenses/",
    "tools/linux/",
    "tools/openssh/",
)


def release_archive_name(host: str, release_id: str) -> str:
    return f"FPLinux-{host}-x64-{release_id}.zip"


def _readme(host: str, source_stem: str, *, has_documents: bool) -> bytes:
    launcher = "Run.cmd" if host == "windows" else "sh Run.sh"
    platform = (
        "Use Windows 10 or 11 x64 and run as your regular user. The launcher "
        "may ask for permission to install the USB driver."
        if host == "windows"
        else "Use Linux x86-64 with udev, iproute2, coreutils and an IPv4 DHCP client. "
        "The launcher may ask for sudo to install USB rules."
    )
    text = dedent("""\
        FPLinux

        Extract the whole ZIP. Keep the phone powered off and disconnected.
        Run {launcher}, choose the model, then follow its USB instructions.

        {platform}

        FPLinux runs in RAM. Files saved on the phone in RAM disappear at
        power-off. If a fresh backup is needed, wait for the launcher to
        ask you to disconnect. Run the same launcher again when prompted.

        For models with a system card, choose Install on microSD. Leave the
        card in the phone. The launcher shows which card will be erased and
        installs through the same USB RAM session, using the bundled system.
        Choose Start from microSD to boot the installed system over USB.

        Update microSD checks your additional APK selections before writing
        to the card. For recovery, choose Select a previous microSD build.

        Commands for scripts, with the exact model ID from the menu:
          {launcher} run TARGET
          {launcher} run TARGET --boot microsd
          {launcher} console [TARGET]
          {launcher} verify [TARGET]
          {launcher} nand identify --target TARGET
          {launcher} card inspect TARGET
          {launcher} card install TARGET --erase-card CID
          {launcher} card update TARGET --card CID
          {launcher} card rollback TARGET --card CID --build-id ID

        Your PC stores the phone backup separately for each model. Use the
        same --phone-data PATH for all commands when keeping separate stores
        for two handsets of the same model.

        The launcher prints the path to runtime.log. For more loader detail,
        add --loader-trace to the run command.
        """).format(launcher=launcher, platform=platform)
    if has_documents:
        text += "\nModel-specific instructions are in the docs/ directory.\n"
    return append_source_companion(text.encode(), source_stem)


def _component_ranges(metadata: dict[str, Any], image_size: int) -> list[tuple[int, int, str]]:
    ranges = []
    for name in ("kernel", "root"):
        start, size = metadata[name]
        if type(start) is not int or type(size) is not int or start < 0 or size <= 0:
            fail(f"release has an invalid {name} range")
        ranges.append((start, start + size, name))
    ranges.sort()
    first, last = ranges
    if first[1] > last[0] or last[1] > image_size:
        fail("release boot component ranges overlap or exceed the image")
    return ranges


@dataclass
class HostArchive:
    """Collect the board views and immutable shared files for one host OS."""

    host: str
    release: ReleaseContext
    files: dict[str, bytes] = field(default_factory=dict)
    executables: set[str] = field(default_factory=set)
    targets: dict[str, Any] = field(default_factory=dict)
    components: dict[tuple[str, str], str] = field(default_factory=dict)

    def _object(self, contents: bytes) -> str:
        path = f"objects/{sha256_bytes(contents)}"
        self.files.setdefault(path, contents)
        return path

    def _shared(self, name: str, data: bytes, *, executable: bool = False) -> None:
        if name in self.files:
            if self.files[name] != data or executable != (name in self.executables):
                fail(f"release has conflicting shared host file: {name}")
            return
        self.files[name] = data
        if executable:
            self.executables.add(name)

    def _image_parts(self, image: bytes, metadata: dict[str, Any]) -> list[str]:
        platform = metadata["platform"]
        parts = []
        offset = 0
        for start, end, component in _component_ranges(metadata, len(image)):
            if start > offset:
                parts.append(self._object(image[offset:start]))
            payload = self._object(image[start:end])
            if self.components.setdefault((platform, component), payload) != payload:
                fail(f"release boards on {platform} have different {component} bytes")
            parts.append(payload)
            offset = end
        if offset < len(image):
            parts.append(self._object(image[offset:]))
        return parts

    def _fit_parts(self, fit: bytes, image: bytes, metadata: dict[str, Any]) -> list[str]:
        start, size = metadata["kernel"]
        kernel = image[start : start + size]
        position = exact_component_offset(fit, kernel, "the release kernel in the SD FIT")
        return [
            self._object(part) for part in (fit[:position], kernel, fit[position + size :]) if part
        ]

    def _bundle(
        self,
        target: str,
        files: dict[str, bytes],
        executables: set[str],
        *,
        ram: bool,
        documents: frozenset[str],
    ) -> dict[str, Any]:
        runtime = json.loads(files["runtime-manifest.json"])
        if runtime["target"] != target:
            fail(f"release runtime selection differs for {target}")
        metadata = json.loads(files[BOOT_COMPONENTS]) if ram else None
        if metadata is not None and (
            metadata["image"] != runtime["image"]
            or metadata["platform"] != runtime["identity"]["platform"]["name"]
        ):
            fail(f"release runtime and component selection differ for {target}")
        members = {}
        for name, contents in sorted(files.items()):
            if name in documents:
                path = f"docs/{target}/{name}"
                self._shared(path, contents)
                members[name] = [path]
            elif name.startswith(_SHARED_PATHS):
                self._shared(name, contents, executable=name in executables)
                members[name] = [name]
            elif name == "assets.lock.toml":
                path = f"assets/{target}.lock.toml"
                self._shared(path, contents)
                members[name] = [path]
            elif metadata is not None and name == runtime["image"]:
                members[name] = self._image_parts(contents, metadata)
            elif metadata is not None and name == "card/fit.itb":
                members[name] = self._fit_parts(contents, files[runtime["image"]], metadata)
            elif metadata is not None and name == "card/root.ext4.xz":
                path = self._object(contents)
                platform = metadata["platform"]
                if self.components.setdefault((platform, "card-root"), path) != path:
                    fail(f"release boards on {platform} have different microSD base files")
                members[name] = [path]
            else:
                members[name] = [self._object(contents)]
            if name in {"LICENSE", "THIRD_PARTY_NOTICES.md"}:
                self._shared(name, contents)
        return {"files": members, "executables": sorted(executables)}

    def add(
        self,
        target: str,
        files: dict[str, bytes],
        executables: set[str],
        *,
        microsd: tuple[dict[str, bytes], set[str]] | None = None,
        documents: frozenset[str] = frozenset(),
    ) -> None:
        """Record the RAM runtime and the optional installed system-card runtime."""
        runtime = json.loads(files["runtime-manifest.json"])
        entry = {
            "name": runtime["identity"]["target"]["display_name"],
            **self._bundle(target, files, executables, ram=True, documents=documents),
        }
        if microsd is not None:
            entry["microsd"] = self._bundle(target, *microsd, ram=False, documents=documents)
        self.targets[target] = entry

    def finish(self, source_stem: str) -> None:
        """Add the catalog, instructions and native launcher at the ZIP root."""
        self.files[CATALOG_NAME] = canonical_json_bytes(
            {**self.release.fields(), "targets": self.targets}
        )
        self.files["README.txt"] = _readme(
            self.host,
            source_stem,
            has_documents=any(name.startswith("docs/") for name in self.files),
        )
        self.files["fplinux.py"] = (_LAUNCHERS / "release_entry.py").read_bytes()
        self.executables.add("fplinux.py")
        if self.host == "windows":
            self.files["Run.cmd"] = (_LAUNCHERS / "run.cmd").read_bytes()
        else:
            self.files["Run.sh"] = (_LAUNCHERS / "run.sh").read_bytes()
            self.executables.add("Run.sh")
