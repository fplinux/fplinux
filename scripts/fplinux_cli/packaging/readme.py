# SPDX-License-Identifier: GPL-2.0-only
"""Render the minimal instructions delivered with a selected phone archive."""

from __future__ import annotations

import json
from textwrap import dedent
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
    sections = [
        dedent("""\
            Before removing power, stop programs using data cards, disable card-backed
            swap, flush writes and unmount all data-card filesystems. Do not unplug USB
            or remove the battery while a writable card filesystem is mounted.
            """).removesuffix("\n")
    ]
    if external_root:
        sections.append(
            dedent("""\
                With a microSD system root, keep USB connected. Stop its writers and
                unmount other card filesystems, then run on the phone:

                  sync
                  mount -o remount,ro /
                  grep ' / ' /proc/mounts

                The root entry must show ro. If remounting fails, keep the phone powered
                and stop the remaining writers before trying again. After confirming ro:

                  poweroff
                """).removesuffix("\n")
        )
    sections.append(
        dedent("""\
            Only after releasing the cards, disconnect USB. If the phone stays powered,
            remove and reinsert its battery before booting normally. A fresh USB load
            is required after power-off.
            """).removesuffix("\n")
    )
    instructions = "\n\n".join(sections) + "\n"
    if with_offline_documentation:
        instructions += "Full shutdown instructions are included in docs/use/power/index.html.\n"
    return instructions


def ordinary_readme(target: str, config: dict[str, Any]) -> bytes:
    """Build source-runner instructions without repeating qualification summaries."""
    name = target_display_name(config["identity"])
    instructions = config["adapter"]["boot_instructions"]
    documentation = phone_documentation_path(target)
    external_root = config["linux"]["root"]["kind"] == "external"
    sections = [
        dedent("""\
            FPLinux for {name}

            Use an archive for this exact phone. On a Linux x86-64 PC, install Python 3.14,
            GNU coreutils, iproute2 and OpenSSH. The PC must configure a new USB network
            interface through IPv4 DHCP. Use a USB data cable.

            XDG_RUNTIME_DIR must name an existing absolute directory owned by your user;
            a normal Linux login session provides it.

            From the extracted archive directory, check its files and install USB rules:

              sha256sum -c SHA256SUMS
              sudo install -m 0644 ./60-fplinux.rules /etc/udev/rules.d/60-fplinux.rules
              sudo udevadm control --reload-rules
            """)
        .removesuffix("\n")
        .format(name=name)
    ]
    if external_root:
        sections.append(
            dedent("""\
                Before starting the runner, prepare the microSD system card from FPLINUX.img.xz:
                  docs/guides/microsd-system-card/index.html

                Insert the prepared card into the powered-off phone and keep it installed
                for the whole powered session.
                """).removesuffix("\n")
        )
    sections.append(
        dedent("""\
            Keep the powered-off phone disconnected. Start the runner as your regular user:

              ./runner/run.py

            Wait until the runner asks for the phone, then follow its instruction:

              {instructions}

            If connected too early, disconnect and restart this sequence. Exit the shell
            to return to the PC; Linux keeps running. Reconnect with:

              ./runner/run.py --reconnect

            Phone support, controls and limits: {documentation}
            Shared archive procedures: docs/guides/standalone-archive/index.html
            """)
        .removesuffix("\n")
        .format(instructions=instructions, documentation=documentation)
    )
    if documentation == "docs/porting/checklist/index.html":
        sections.append(
            dedent("""\
                The configured BootROM key is a starting point for bring-up; confirm it
                on this phone before use. Configuration alone does not establish support.
                """).removesuffix("\n")
        )
    sections.append(
        shutdown_instructions(
            external_root=external_root, with_offline_documentation=True
        ).removesuffix("\n")
    )
    text = "\n\n".join(sections) + "\n"
    return text.encode("utf-8")


def append_source_companion(readme: bytes, source_stem: str) -> bytes:
    """Append the matching source archive after its name has been determined."""
    footer = dedent("""\
        Matching source and build material:
          {source_stem}.zip
        Keep it available alongside this archive. Licenses remain included here.
        """).format(source_stem=source_stem)
    return readme + b"\n" + footer.encode("utf-8")
