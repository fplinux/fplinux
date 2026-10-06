# SPDX-License-Identifier: GPL-2.0-only
"""Order project TOML fields without interpreting or repairing their values."""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import PurePosixPath
from typing import TYPE_CHECKING, cast

import tomlkit
from tomlkit.container import Container
from tomlkit.items import AoT, Array, Comment, InlineTable, Item, Key, Table, Whitespace

if TYPE_CHECKING:
    from collections.abc import Mapping, Sequence

_COPY_FIELDS = ("source", "destination")
_SOURCE_FIELDS = ("repository", "commit", "archive_url", "archive_sha256", "license", "files")
_FIRMWARE_FIELDS = ("source", "destination", "size", "sha256")

_TARGET_ORDER = {
    "": (
        "platform",
        "identity",
        "nand",
        "display_brightness",
        "rootfs",
        "bundle",
        "linux",
        "bootstrap",
        "adapter",
        "device_data",
        "board_maps",
        "bluetooth",
        "audio_profile",
        "fm_radio",
        "microsd",
    ),
    "identity": ("brand", "product", "hardware_codes", "compatible"),
    "nand": ("raw_device", "id", "raw_page_bytes"),
    "display_brightness": ("backlight", "levels"),
    "rootfs": ("packages",),
    "bundle": ("packages",),
    "linux": (
        "config_fragment",
        "memory",
        "dtb",
        "debug_dtb",
        "patches",
        "forbidden_dtb_markers",
        "forbidden_config",
        "copies",
        "appends",
    ),
    "linux.memory": ("base", "size"),
    "linux.copies": _COPY_FIELDS,
    "linux.appends": _COPY_FIELDS,
    "bootstrap": ("lcd_config", "image", "map", "dtb_destination", "record_prefix"),
    "adapter": (
        "spi_mode",
        "lcd_id",
        "backlight_channels",
        "backlight_level",
        "exec_distance",
        "session_name",
        "boot_instructions",
    ),
    "device_data": ("parser",),
    "board_maps": ("firmware",),
    "board_maps.firmware": _FIRMWARE_FIELDS,
    "bluetooth": ("firmware",),
    "bluetooth.firmware": _FIRMWARE_FIELDS,
    "audio_profile": ("firmware",),
    "audio_profile.firmware": _FIRMWARE_FIELDS,
    "fm_radio": ("firmware",),
    "fm_radio.firmware": _FIRMWARE_FIELDS,
    "microsd": ("linux_patches", "bootstrap", "uboot"),
    "microsd.bootstrap": ("source", "image", "map"),
    "microsd.uboot": ("defconfig", "patches", "copies"),
    "microsd.uboot.copies": _COPY_FIELDS,
}

_PLATFORM_ORDER = {
    "": ("identity", "rootfs", "bundle", "uboot", "linux", "bootstrap", "runtime", "host"),
    "identity": ("vendor", "soc", "aliases", "compatible"),
    "rootfs": ("packages",),
    "bundle": ("packages",),
    "uboot": ("source", "archive_prefix", "patches", "required_config", "copies"),
    "uboot.copies": _COPY_FIELDS,
    "linux": (
        "source_lock",
        "arch",
        "cross_compile",
        "analysis_cross_compile",
        "defconfig",
        "build_types",
        "config_script",
        "dts_directory",
        "platform_identity_header",
        "targets",
        "image_output",
        "dtb_output_directory",
        "patches",
        "copies",
        "appends",
        "dt_config_checks",
    ),
    "linux.build_types": ("debug", "release"),
    "linux.copies": _COPY_FIELDS,
    "linux.appends": _COPY_FIELDS,
    "linux.dt_config_checks": ("path", "compatible", "property", "config"),
    "bootstrap": (
        "vendor_source_lock",
        "vendor_cache_name",
        "archive_prefix",
        "source_destination",
        "vendor_destination",
        "output_destination",
        "lcd_config_destination",
        "kernel_destination",
        "files",
        "patches",
        "toolchain",
        "lto",
        "pack_reloc",
        "safety_target",
        "build_targets",
        "load_address",
        "payload_limit",
        "shared_copies",
        "linux_copies",
        "layout",
    ),
    "bootstrap.shared_copies": _COPY_FIELDS,
    "bootstrap.linux_copies": _COPY_FIELDS,
    "bootstrap.layout": (
        "ram_base",
        "ram_size",
        "kernel_load",
        "kernel_entry",
        "kernel_size",
        "fdt_load",
        "fdt_size",
        "framebuffer",
        "framebuffer_size",
        "timer_hz",
    ),
    "runtime": ("fdl1_load_address", "adapter", "usb"),
    "runtime.adapter": (
        "brightness",
        "rotation",
        "handoff_wait_seconds",
        "usb_release_wait_seconds",
    ),
    "runtime.usb": ("bootrom", "linux_gadget"),
    "runtime.usb.bootrom": ("vendor_id", "product_id", "wait_seconds"),
    "runtime.usb.linux_gadget": (
        "vendor_id",
        "product_id",
        "keyboard_interface",
        "wait_seconds",
    ),
    "host": ("runtime_tools", "tools"),
    "host.runtime_tools": ("loader", "bridge", "keyboard"),
    "host.tools": (
        "type",
        "name",
        "source_lock",
        "cache_name",
        "archive_prefix",
        "source_directory",
        "source",
        "binary",
        "link",
        "copies",
        "patches",
        "self_test",
        "members",
    ),
    "host.tools.members": ("path", "digest_key"),
    "host.tools.copies": _COPY_FIELDS,
}

_PROFILE_ORDER = {
    "": ("name", "linux", "rootfs", "bootstrap", "uboot", "fit", "layout", "storage", "runtime"),
    "linux": ("root",),
    "linux.root": ("kind", "filesystem", "wait_seconds"),
    "rootfs": ("packages",),
    "bootstrap": ("kind",),
    "uboot": ("kind",),
    "fit": ("kind", "filename"),
    "layout": (
        "resident_start",
        "resident_limit",
        "uboot_load",
        "uboot_size",
        "uboot_stack",
        "fit_load",
        "fit_size",
        "fdt_pad",
    ),
    "storage": (
        "filename",
        "disk_signature",
        "boot_partition",
        "boot_offset",
        "boot_size",
        "boot_label",
        "root_partition",
        "root_offset",
        "root_size",
        "root_filename",
        "root_label",
        "root_uuid",
        "block_size",
        "inode_size",
    ),
    "runtime": ("transport", "runnable"),
}

_ROOT_ORDERS: dict[str, dict[str, tuple[str, ...]]] = {
    "REUSE.toml": {
        "": ("version", "SPDX-PackageName", "SPDX-PackageSupplier", "annotations"),
        "annotations": ("path", "precedence", "SPDX-FileCopyrightText", "SPDX-License-Identifier"),
    },
    "_typos.toml": {
        "": ("files", "default"),
        "files": ("extend-exclude",),
        "default": ("extend-ignore-re", "extend-identifiers", "extend-words"),
    },
    "pyproject.toml": {
        "": ("tool",),
        "tool": ("ruff", "mypy"),
        "tool.ruff": (
            "target-version",
            "line-length",
            "cache-dir",
            "extend-exclude",
            "lint",
            "format",
        ),
        "tool.ruff.lint": ("select", "ignore", "per-file-ignores"),
        "tool.ruff.format": ("line-ending",),
        "tool.mypy": ("python_version", "strict", "mypy_path", "cache_dir"),
    },
    "container.lock.toml": {
        "": ("kern", "oci"),
        "kern": ("version", "archive_url", "archive_sha256", "binary_sha256"),
        "oci": (
            "repository",
            "platform",
            "base_repository",
            "base_release",
            "base_rootfs_url",
            "base_rootfs_sha256",
        ),
    },
    "sources.lock.toml": {
        "": ("linux", "fpdoom_bootstrap", "fpdoom_host", "spreadtrum_flash"),
        "linux": ("version", "url", "sha256", "license"),
        "fpdoom_bootstrap": _SOURCE_FIELDS,
        "fpdoom_host": _SOURCE_FIELDS,
        "spreadtrum_flash": _SOURCE_FIELDS,
    },
    "alpine.lock.toml": {
        "": (
            "release",
            "branch",
            "arch",
            "triplet",
            "repositories",
            "minirootfs",
            "runtime",
            "sysroot",
            "package",
        ),
        "repositories": ("community", "main"),
        "minirootfs": ("url", "sha256", "bytes"),
        "runtime": ("packages", "additions"),
        "sysroot": ("packages",),
        "package": ("repository", "file", "sha256", "bytes"),
    },
    "releases.lock.toml": {"": ("verified",)},
}

_LEXICAL_TABLES = {
    "_typos.toml": {"default.extend-identifiers", "default.extend-words"},
    "sources.lock.toml": {"fpdoom_host.files", "spreadtrum_flash.files"},
    "alpine.lock.toml": {"runtime.additions"},
    "releases.lock.toml": {"verified"},
}


@dataclass
class _Entry:
    key: Key
    value: Item
    comments: list[Item] = field(default_factory=list)


def _policy(relative: str) -> Mapping[str, Sequence[str]]:  # noqa: PLR0911
    # File families have different ordering contracts; keep selection explicit.
    path = PurePosixPath(relative)
    if relative in _ROOT_ORDERS:
        return _ROOT_ORDERS[relative]
    if path.name in {"target.toml", "target.toml.in"} or (
        relative.startswith("tests/fixtures/profile_config/") and path.name.startswith("target-")
    ):
        return _TARGET_ORDER
    if path.name == "platform.toml":
        return _PLATFORM_ORDER
    if (
        path.name == "profile.toml"
        or relative == "tests/fixtures/profile_config/default-with-feature-package.toml"
    ):
        return _PROFILE_ORDER
    if relative.endswith("/loader/assets.lock.toml"):
        return {
            "": ("source",),
            "source": ("id", "kind", "url", "sha256", "cache_name", "license", "output"),
            "source.output": ("role", "path", "member", "sha256"),
        }
    if relative.endswith("/release/manifest.toml"):
        return {"": ("image", "bundle_files", "runtime_files", "documents")}
    if path.name == "u-boot.lock.toml":
        return {
            "": (
                "version",
                "repository",
                "tag",
                "commit",
                "archive_url",
                "archive_sha256",
                "license",
            )
        }
    return {}


def _take_section_comments(value: Item) -> list[Item]:
    """Move comments immediately before the next header with that section."""
    if isinstance(value, AoT):
        return _take_section_comments(value.body[-1]) if value.body else []
    if not isinstance(value, Table):
        return []
    body = value.value.body
    trailing = len(body)
    while trailing and body[trailing - 1][0] is None:
        trailing -= 1
    suffix = [item for _, item in body[trailing:]]
    if any(isinstance(item, Comment) for item in suffix):
        del body[trailing:]
        return suffix
    if trailing:
        return _take_section_comments(body[trailing - 1][1])
    return []


def _entries(container: Container) -> tuple[list[Item], list[_Entry], list[Item]]:
    leading: list[Item] = []
    entries: list[_Entry] = []
    pending: list[Item] = []
    for key, value in container.body:
        if key is None:
            pending.append(value)
            continue
        if entries and isinstance(value, (Table, AoT)):
            pending = _take_section_comments(entries[-1].value) + pending
        entries.append(_Entry(key, value, pending))
        pending = []
    if entries:
        leading = entries[0].comments
        entries[0].comments = []
    return leading, entries, pending


def _merge_fragments(entries: list[_Entry]) -> list[_Entry]:
    """Combine repeated parent fragments while retaining each array's owners."""
    result: list[_Entry] = []
    tables: dict[str, _Entry] = {}
    for entry in entries:
        previous = tables.get(entry.key.key)
        if (
            previous is not None
            and isinstance(previous.value, Table)
            and isinstance(entry.value, Table)
        ):
            first = previous.value
            second = entry.value
            body = Container(parsed=True)
            for key, value in first.value.body:
                body.append(key, value, validate=False)
            for comment in entry.comments:
                body.append(None, comment)
            for key, value in second.value.body:
                body.append(key, value, validate=False)
            owner = second if first.is_super_table() and not second.is_super_table() else first
            previous.value = Table(
                body,
                owner.trivia,
                owner.is_aot_element(),
                owner.is_super_table(),
                owner.name,
                owner.display_name,
            )
        else:
            result.append(entry)
            if isinstance(entry.value, Table):
                tables[entry.key.key] = entry
    return result


def _comments(items: Sequence[Item]) -> list[Comment]:
    return [item for item in items if isinstance(item, Comment)]


def _header(leading: list[Item]) -> tuple[list[Item], list[Item]]:
    notices = [
        index
        for index, item in enumerate(leading)
        if isinstance(item, Comment) and "SPDX-" in item.as_string()
    ]
    if not notices:
        return [], leading
    for index, item in enumerate(leading):
        if index > notices[-1] and isinstance(item, Whitespace) and "\n" in item.s:
            return leading[:index], leading[index + 1 :]
    return leading, []


def _section_leader(value: Table | AoT) -> Table | None:
    if isinstance(value, AoT):
        return value.body[0] if value.body else None
    if not value.is_super_table():
        return value
    for _, child in value.value.body:
        if isinstance(child, (Table, AoT)):
            return _section_leader(child)
    return None


def _sort_package_array(array: Array) -> Array:
    if not all(isinstance(value, str) for value in array):
        return array
    # The pinned parser keeps element comments in groups rather than on values.
    # Rebuild from those groups so a sorted package retains its own comments.
    groups = array._value  # noqa: SLF001
    packages: list[tuple[str, list[Item]]] = []
    pending: list[Item] = []
    for group in groups:
        if isinstance(group.value, str):
            packages.append((str(group.value), [*pending, *group]))
            pending = []
        else:
            pending.extend(group)
    if [name for name, _ in packages] == sorted(name for name, _ in packages):
        return array
    raw: list[Item] = []
    for _, items in sorted(packages, key=lambda package: package[0]):
        raw.extend(item for item in items if not isinstance(item, Whitespace) or "," not in item.s)
        if items and isinstance(items[-1], Comment):
            raw.insert(len(raw) - 1, Whitespace(","))
        else:
            raw.append(Whitespace(","))
    raw.extend(pending)
    return Array(raw, array.trivia)


def _normalize_value(
    value: Item, *, path: str, relative: str, order: Mapping[str, Sequence[str]]
) -> Item:
    if isinstance(value, (Table, InlineTable)):
        is_super = value.is_super_table() if isinstance(value, Table) else False
        body = _normalize_container(
            value.value,
            path=path,
            relative=relative,
            order=order,
            inline=isinstance(value, InlineTable),
        )
        if isinstance(value, InlineTable):
            return InlineTable(body, value.trivia)
        return Table(
            body, value.trivia, value.is_aot_element(), is_super, value.name, value.display_name
        )
    if isinstance(value, AoT):
        tables = list(value.body)
        for index, table in enumerate(tables[:-1]):
            prefix = _take_section_comments(table)
            tables[index + 1].trivia.indent += "".join(
                comment.as_string() for comment in _comments(prefix)
            )
        if (
            relative == "alpine.lock.toml"
            and path == "package"
            and all(isinstance(table.get("file"), str) for table in tables)
        ):
            tables.sort(key=lambda table: str(table["file"]))
        normalized: list[Table] = []
        for table in tables:
            normalized_table = cast(
                "Table", _normalize_value(table, path=path, relative=relative, order=order)
            )
            normalized_table.trivia.indent = (
                "\n" if normalized else ""
            ) + normalized_table.trivia.indent.lstrip("\n")
            normalized.append(normalized_table)
        return AoT(normalized, name=value.name, parsed=True)
    if isinstance(value, Array):
        if (order is _TARGET_ORDER or order is _PLATFORM_ORDER) and path in {
            "rootfs.packages",
            "bundle.packages",
        }:
            return _sort_package_array(value)
        for index, item in enumerate(value):
            if isinstance(item, (InlineTable, Array)):
                value[index] = _normalize_value(item, path=path, relative=relative, order=order)
    return value


def _normalize_container(
    container: Container,
    *,
    path: str,
    relative: str,
    order: Mapping[str, Sequence[str]],
    inline: bool = False,
) -> Container:
    leading, entries, trailing = _entries(container)
    entries = _merge_fragments(entries)
    header: list[Item] = []
    if not path and not inline:
        header, leading = _header(leading)
    if entries:
        entries[0].comments = leading + entries[0].comments
    elif leading:
        trailing = leading + trailing
    ranks = {key: index for index, key in enumerate(order.get(path, ()))}
    lexical = path in _LEXICAL_TABLES.get(relative, set())
    entries.sort(
        key=lambda entry: (
            isinstance(entry.value, (Table, AoT)) and not entry.key.is_dotted(),
            entry.key.key
            if lexical
            else (
                len(ranks)
                if path == "host.tools"
                and entry.key.key == "copies"
                and isinstance(entry.value, AoT)
                else ranks.get(entry.key.key, len(ranks))
            ),
        )
    )
    normalized = Container(parsed=True)
    for item in header:
        normalized.append(None, item)
    if inline:
        normalized.append(None, Whitespace(" "))
    for index, entry in enumerate(entries):
        child_path = f"{path}.{entry.key.key}" if path else entry.key.key
        value = _normalize_value(entry.value, path=child_path, relative=relative, order=order)
        comments = _comments(entry.comments)
        if isinstance(value, (Table, AoT)) and not inline and not entry.key.is_dotted():
            leader = _section_leader(value)
            if leader is not None:
                leader.trivia.indent = (
                    ("\n" if index else "")
                    + "".join(comment.as_string() for comment in comments)
                    + leader.trivia.indent.lstrip("\n")
                )
        else:
            for comment in comments:
                normalized.append(None, comment)
            if inline:
                value.trivia.indent = " " if index else ""
            elif "\n" not in value.trivia.trail:
                value.trivia.trail += "\n"
        normalized.append(entry.key, value, validate=False)
    for comment in _comments(trailing):
        normalized.append(None, comment)
    if inline:
        normalized.append(None, Whitespace(" "))
    return normalized


def normalize_toml(relative: str, contents: bytes) -> bytes:
    """Return canonical field order, preserving values and ordered collections."""
    document = tomlkit.parse(contents.decode("utf-8"))
    normalized = _normalize_container(
        document, path="", relative=relative, order=_policy(relative)
    )
    return normalized.as_string().encode("utf-8")
