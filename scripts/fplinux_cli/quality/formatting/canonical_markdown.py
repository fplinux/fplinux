# SPDX-License-Identifier: GPL-2.0-only
"""Keep phone documentation sections and whole status rows in their shared order."""

from __future__ import annotations

import re
from pathlib import PurePosixPath

_SECTIONS = (
    "Identity",
    "Status",
    "Features",
    "Applications",
    "Hardware interfaces",
    "Fitted device data",
    "Device data",
    "Load into RAM",
    "Target-specific use",
    "Development diagnostics",
    "End the RAM session",
    "Release boundary",
)
_FEATURES = (
    "RAM boot",
    "Persistent boot",
    "Local console",
    "LCD backlight",
    "Keypad backlight",
    "USB networking",
    "SSH access",
    "File transfer",
    "Host keyboard bridge",
    "CPU clock reporting",
    "Manual CPU frequency selection",
    "AP DMAengine",
    "Image rotation",
    "JPEG codec and scaling",
    "Native image presentation",
    "USB host mode",
    "Removable storage",
    "Removable system root",
    "Internal phone storage",
    "Headphone audio",
    "Speaker audio",
    "Phone microphone",
    "FM radio",
    "Modem and mobile service",
    "Bluetooth",
    "Wi-Fi",
    "Camera",
    "Charger status",
    "Battery telemetry",
    "SoC temperature",
    "Auxiliary ADC",
    "Real-time clock",
    "Other battery functions",
    "Vibration",
    "Indicator LEDs",
    "Power-off",
    "Suspend",
    "Reboot",
)
_APPLICATIONS = (
    "FPLinux: ARMADA",
    "Brightness control",
    "TyrQuake",
    "Image rotation",
    "JPEG codec and scaling",
    "Native image presentation",
)
_IDENTITY = ("Target", "Device", "Hardware code", "Platform", "Boot")
_LINK_LABEL = re.compile(r"^\[([^]]+)\]\([^)]*\)$")


def _fenced_lines(lines: list[str]) -> set[int]:
    protected: set[int] = set()
    fence: str | None = None
    for index, line in enumerate(lines):
        marker = re.match(r"^ {0,3}(`{3,}|~{3,})", line)
        if fence is not None:
            protected.add(index)
            if (
                marker
                and marker[1][0] == fence[0]
                and len(marker[1]) >= len(fence)
                and not line[marker.end() :].strip()
            ):
                fence = None
        elif marker:
            protected.add(index)
            fence = marker[1]
    return protected


def align_markdown_tables(text: str) -> str:
    """Align simple pipe tables used by rendered target templates."""
    lines = text.split("\n")
    protected = _fenced_lines(lines)
    aligned: list[str] = []
    index = 0
    while index < len(lines):
        if index in protected or not lines[index].startswith("|"):
            aligned.append(lines[index])
            index += 1
            continue
        end = index
        while end < len(lines) and end not in protected and lines[end].startswith("|"):
            end += 1
        rows = [
            [cell.strip() for cell in line.strip().strip("|").split("|")]
            for line in lines[index:end]
        ]
        if len(rows) < 2 or any(len(row) != len(rows[0]) for row in rows):
            aligned.extend(lines[index:end])
            index = end
            continue
        if not all(re.fullmatch(r":?-+:?", cell) for cell in rows[1]):
            aligned.extend(lines[index:end])
            index = end
            continue
        content = [row for number, row in enumerate(rows) if number != 1]
        widths = [max(3, *(len(row[column]) for row in content)) for column in range(len(rows[0]))]
        for number, row in enumerate(rows):
            cells: list[str] = []
            for cell, width in zip(row, widths, strict=True):
                if number == 1:
                    left = ":" if cell.startswith(":") else ""
                    right = ":" if cell.endswith(":") else ""
                    cells.append(left + "-" * (width - len(left) - len(right)) + right)
                else:
                    cells.append(cell.ljust(width))
            aligned.append("| " + " | ".join(cells) + " |")
        index = end
    return "\n".join(aligned)


def _row_label(line: str) -> str:
    cell = line.strip().strip("|").split("|", 1)[0].strip()
    match = _LINK_LABEL.fullmatch(cell)
    return match[1] if match else cell


def _order_table(section: str, order: tuple[str, ...]) -> str:
    lines = section.splitlines(keepends=True)
    protected = _fenced_lines(lines)
    start = next(
        (
            index
            for index, line in enumerate(lines)
            if index not in protected and line.startswith("|")
        ),
        None,
    )
    if start is None:
        return section
    end = start
    while end < len(lines) and lines[end].startswith("|"):
        end += 1
    rows = lines[start + 2 : end]
    ranks = {name: index for index, name in enumerate(order)}
    lines[start + 2 : end] = sorted(rows, key=lambda line: ranks.get(_row_label(line), len(order)))
    return "".join(lines)


def is_phone_readme(relative: str) -> bool:
    """Identify target documents and their platform-owned README templates."""
    path = PurePosixPath(relative.removesuffix(".in"))
    return (len(path.parts) == 3 and path.parts[0] == "targets" and path.name == "README.md") or (
        "target-template" in path.parts and path.name == "README.md"
    )


def normalize_markdown(relative: str, contents: bytes) -> bytes:
    """Order phone sections and rows without altering their claims, links or notes."""
    if not is_phone_readme(relative):
        return contents
    text = contents.decode("utf-8")
    lines = text.splitlines(keepends=True)
    protected = _fenced_lines(lines)
    heading_offsets: set[int] = set()
    offset = 0
    for index, line in enumerate(lines):
        if index not in protected and line.startswith("## "):
            heading_offsets.add(offset)
        offset += len(line)
    matches = [
        match
        for match in re.finditer(r"(?m)^## ([^\n]+)\n", text)
        if match.start() in heading_offsets
    ]
    if not matches:
        return contents
    prefix = text[: matches[0].start()]
    sections: list[tuple[str, str]] = []
    for index, match in enumerate(matches):
        end = matches[index + 1].start() if index + 1 < len(matches) else len(text)
        title = match[1]
        body = text[match.start() : end]
        order = {"Identity": _IDENTITY, "Features": _FEATURES, "Applications": _APPLICATIONS}.get(
            title
        )
        if order is not None:
            body = _order_table(body, order)
        sections.append((title, body))
    ranks = {name: index for index, name in enumerate(_SECTIONS)}
    sections.sort(key=lambda section: ranks.get(section[0], len(_SECTIONS)))
    normalized = prefix + "\n\n".join(body.rstrip("\n") for _title, body in sections) + "\n"
    return align_markdown_tables(normalized).encode("utf-8")
