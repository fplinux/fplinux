# SPDX-License-Identifier: GPL-2.0-only
"""Render the minimal instructions delivered with a selected phone archive."""

from __future__ import annotations

import json
from typing import Any

from fplinux_cli import common
from fplinux_cli.common import fail
from fplinux_cli.manifests.identity import target_display_name
from fplinux_cli.manifests.values import relative_value


def phone_documentation_path(target: str) -> str:
    """Resolve the selected phone's existing route in the bundled offline site."""
    path = common.ROOT / "site/src/data/phones.json"
    try:
        phones = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        fail(f"phone documentation routes cannot be read: {error}")
    if not isinstance(phones, list):
        fail("phone documentation routes must be an array")
    matches = [phone for phone in phones if isinstance(phone, dict) and phone.get("id") == target]
    if not matches:
        return "docs/porting/checklist/index.html"
    if len(matches) != 1:
        fail(f"phone documentation route is duplicated: {target}")
    slug = relative_value(matches[0].get("slug"), "phone documentation route")
    return f"docs/{slug}/index.html"


def shutdown_instructions(*, external_root: bool, with_offline_documentation: bool) -> str:
    """Keep card-release prerequisites beside the archive's power-removal action."""
    instructions = (
        "Before removing power, stop programs using data cards, disable card-backed\n"
        "swap, flush writes and unmount all data-card filesystems. Do not unplug USB\n"
        "or remove the battery while a writable card filesystem is mounted.\n"
    )
    if external_root:
        instructions += (
            "\nWith a microSD system root, keep USB connected. Stop its writers and\n"
            "unmount other card filesystems, then run on the phone:\n\n"
            "  sync\n"
            "  mount -o remount,ro /\n"
            "  grep ' / ' /proc/mounts\n\n"
            "The root entry must show ro. If remounting fails, keep the phone powered\n"
            "and stop the remaining writers before trying again. After confirming ro:\n\n"
            "  poweroff\n"
        )
    instructions += (
        "\nOnly after releasing the cards, disconnect USB. If the phone stays powered,\n"
        "remove and reinsert its battery before booting normally. A fresh USB load\n"
        "is required after power-off.\n"
    )
    if with_offline_documentation:
        instructions += "Full shutdown instructions are included in docs/use/power/index.html.\n"
    return instructions


def ordinary_readme(target: str, config: dict[str, Any]) -> bytes:
    """Build source-runner instructions without repeating qualification summaries."""
    name = target_display_name(config["identity"])
    instructions = config["adapter"]["boot_instructions"]
    documentation = phone_documentation_path(target)
    external_root = config["linux"]["root"]["kind"] == "external"
    system_card_note = (
        "Before starting the runner, prepare the microSD system card from FPLINUX.img.xz:\n"
        "  docs/guides/microsd-system-card/index.html\n\n"
        "Insert the prepared card into the powered-off phone and keep it installed\n"
        "for the whole powered session.\n\n"
        if external_root
        else ""
    )
    bring_up_note = (
        "The configured BootROM key is a starting point for bring-up; confirm it\n"
        "on this phone before use. Configuration alone does not establish support.\n\n"
        if documentation == "docs/porting/checklist/index.html"
        else ""
    )
    text = (
        f"FPLinux for {name}\n\n"
        "Use an archive for this exact phone. On a Linux x86-64 PC, install Python 3.14,\n"
        "GNU coreutils, iproute2 and OpenSSH. The PC must configure a new USB network\n"
        "interface through IPv4 DHCP. Use a USB data cable.\n\n"
        "XDG_RUNTIME_DIR must name an existing absolute directory owned by your user;\n"
        "a normal Linux login session provides it.\n\n"
        "From the extracted archive directory, check its files and install USB rules:\n\n"
        "  sha256sum -c SHA256SUMS\n"
        "  sudo install -m 0644 ./60-fplinux.rules /etc/udev/rules.d/60-fplinux.rules\n"
        "  sudo udevadm control --reload-rules\n\n"
        f"{system_card_note}"
        "Keep the powered-off phone disconnected. Start the runner as your regular user:\n\n"
        "  ./runner/run.py\n\n"
        "Wait until the runner asks for the phone, then follow its instruction:\n\n"
        f"  {instructions}\n\n"
        "If connected too early, disconnect and restart this sequence. Exit the shell\n"
        "to return to the PC; Linux keeps running. Reconnect with:\n\n"
        "  ./runner/run.py --reconnect\n\n"
        f"Phone support, controls and limits: {documentation}\n"
        "Shared archive procedures: docs/guides/standalone-archive/index.html\n\n"
        f"{bring_up_note}"
        f"{shutdown_instructions(external_root=external_root, with_offline_documentation=True)}"
    )
    return text.encode("utf-8")
