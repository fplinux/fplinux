# SPDX-License-Identifier: GPL-2.0-only
"""Render the minimal instructions delivered with a selected phone archive."""

from __future__ import annotations

import json
from textwrap import dedent
from typing import Any

from fplinux_cli import common
from fplinux_cli.build.runtime import device_data_descriptor
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


def preparation_instructions(*, emmc: bool) -> str:
    """Describe the selected read and its safe-disconnect message before the normal boot."""
    if emmc:
        return dedent("""\
            When phone settings are needed

            The launcher reads only the selected sound settings from the phone's
            read-only internal storage and prepares the files FPLinux needs.
            Keep USB connected until the launcher tells you: "Disconnect USB."
            The earlier message "Your phone data is ready" is not the end of this step.
            If the launcher cannot check that storage is safe to disconnect, keep the
            phone powered and follow its error message.

            When asked, disconnect USB. If the phone stays on, remove and reinsert its
            battery. Start the launcher again with the phone powered off and disconnected.
            Wait for the connection instructions again, then connect to start FPLinux.

            Compatible saved settings are reused on later launches and by new archives
            for this same phone. They can be prepared again without another phone read.
            """)
    return dedent("""\
        When a backup is needed

        The launcher copies the phone's internal memory to your computer and prepares
        the files FPLinux needs. It does not change the phone's internal memory.
        Keep USB connected until the launcher tells you: "Disconnect USB."
        The earlier message "Your phone data is ready" is not the end of this step.
        If the launcher cannot check that storage is safe to disconnect, keep the
        phone powered and follow its error message.

        When asked, disconnect USB. If the phone stays on, remove and reinsert its
        battery. Start the launcher again with the phone powered off and disconnected.
        Wait for the connection instructions again, then connect to start FPLinux.

        If a compatible backup is already saved, the launcher uses it and continues
        to start FPLinux without this extra restart. New archives can reuse the backup
        of this same phone; a new backup is made when the saved one cannot be used.
        """)


def shutdown_instructions(*, external_root: bool, with_offline_documentation: bool) -> str:
    """Keep card-release prerequisites beside the archive's power-removal action."""
    sections = [
        dedent("""\
            Shut down safely

            Save any files you need from RAM to your computer or a supported memory
            card. Files in RAM are lost when the phone turns off.

            If you use a microSD card for files, stop programs using it and turn off
            any swap stored on the card. Save pending writes and unmount the card's
            filesystems so Linux finishes using them. Do not unplug USB or remove
            the battery until this is done. Pulling the card out is not a substitute
            for unmounting it.
            """).removesuffix("\n")
    ]
    if external_root:
        sections.append(
            dedent("""\
                If Linux itself runs from microSD, keep USB connected. Stop programs
                and services writing to the card, turn off any swap on it, and unmount
                all card filesystems except the system filesystem (/). Then run these
                commands in the phone's terminal:

                  sync
                  mount -o remount,ro /
                  grep ' / ' /proc/mounts

                The line for / must show ro, which means read-only. If remounting fails,
                keep the phone powered and stop the remaining programs writing to the
                card before trying again. Only after confirming ro, run:

                  poweroff
                """).removesuffix("\n")
        )
    sections.append(
        dedent("""\
            After unmounting data cards and making any system card read-only, disconnect USB.
            If the phone stays on, remove and reinsert its battery. Start the phone
            normally, without holding the USB boot key, to use its original system.
            To use FPLinux again after power-off, repeat the startup instructions.
            """).removesuffix("\n")
    )
    instructions = "\n\n".join(sections) + "\n"
    if with_offline_documentation:
        instructions += "Full shutdown instructions are included in docs/use/power/index.html.\n"
    return instructions


def ordinary_readme(target: str, config: dict[str, Any]) -> bytes:
    """Describe the selected phone's shared launch workflow and local state."""
    name = target_display_name(config["identity"])
    instructions = config["adapter"]["boot_instructions"]
    documentation = phone_documentation_path(target)
    external_root = config["linux"]["root"]["kind"] == "external"
    source_loaders = config.get("platform") == "sc9820e"
    preparation = device_data_descriptor(target, config) is not None
    emmc = config.get("emmc") is not None
    sections = [
        dedent("""\
            FPLinux for {name}

            Before you start

            Use this archive only with this exact phone. On a Linux x86-64 PC, install Python 3.14,
            GNU coreutils, iproute2 and OpenSSH. The PC must configure a new USB network
            interface through IPv4 DHCP. Use a USB data cable.

            Extract the whole ZIP into a folder. Open a terminal in that folder,
            check the files and install the USB access rules:

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
                Before starting FPLinux, prepare the microSD system card from FPLINUX.img.xz:
                  docs/guides/microsd-system-card/index.html

                Insert the prepared card into the powered-off phone and leave it in
                until FPLinux has shut down safely.
                """).removesuffix("\n")
        )
    if source_loaders:
        loading = "All files needed to load FPLinux into RAM are included."
        if emmc:
            loading += " Internal storage is read only."
        else:
            loading += " Loading does\nnot read or write the phone's internal storage."
        sections.append(loading)
    sections.append(
        dedent("""\
            Start FPLinux

            Turn the phone off and leave USB disconnected. Run this command as your
            regular user, not as root:

              python3.14 -I fplinux.py run

            Wait until the launcher asks you to connect the phone. Then follow its instruction:

              {instructions}

            If you connected too early, disconnect USB, turn the phone off and start again.
            """)
        .removesuffix("\n")
        .format(instructions=instructions)
    )
    if preparation:
        sections.append(preparation_instructions(emmc=emmc).removesuffix("\n"))
    sections.extend(
        [
            dedent("""\
                Reconnect to a running phone

                After loading, the launcher opens the phone's command-line terminal
                on your computer using SSH. Type exit or press Ctrl+D to close this
                terminal; FPLinux keeps running on the phone.

                While the phone is still running FPLinux, connect USB and run the same
                command from the same extracted folder. Do not turn the phone off or
                hold its boot key. To reconnect without starting a new boot, use:

                  python3.14 -I fplinux.py run --reconnect

                Ctrl+C cancels loading or waiting; it does not turn the phone off.
                """).removesuffix("\n"),
            shutdown_instructions(
                external_root=external_root, with_offline_documentation=True
            ).removesuffix("\n"),
            dedent("""\
                Help and saved files

                Each launch saves a runtime.log file and shows its full path.
                Include that log when reporting a startup problem.
                Keep this extracted folder to reconnect to its running phone. Launcher
                state, private connection keys and logs are saved inside .data here.
                FPLINUX_DATA_DIR can select another data directory; a relative value
                is resolved from this extracted folder. Keep these data private.
                Loading progress appears in the terminal. For machine-readable progress,
                add --events /path/to/events.jsonl when starting a new boot.
                """).removesuffix("\n"),
            dedent("""\
                Phone support, controls and limits: {documentation}
                Shared archive procedures: docs/guides/standalone-archive/index.html
                """)
            .removesuffix("\n")
            .format(documentation=documentation),
        ]
    )
    if preparation:
        saved_data = "original settings" if emmc else "backup"
        prepared_from = "them" if emmc else "it"
        saved_files = (
            f"Your phone's {saved_data} and the files prepared from {prepared_from} "
            "are saved on your computer:"
        )
        sections.append(
            dedent("""\
                {saved_files}
                  ~/.local/share/fplinux/phones/{target}
                If XDG_DATA_HOME is set, its directory replaces ~/.local/share.
                This folder is separate from the extracted ZIP, so new archives can
                reuse compatible saved data. Use it with only this physical phone.
                For another phone of the same model, add --phone-data PATH to the
                startup command and choose a different folder. PATH is the exact
                folder to use, without adding a model subfolder.
                """)
            .removesuffix("\n")
            .format(target=target, saved_files=saved_files)
        )
    if documentation == "docs/porting/checklist/index.html":
        sections.append(
            dedent("""\
                This phone has no published support page. The boot key above comes
                from its configuration; confirm it on this phone before use. A
                configuration file alone does not mean the phone has been tested.
                """).removesuffix("\n")
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
