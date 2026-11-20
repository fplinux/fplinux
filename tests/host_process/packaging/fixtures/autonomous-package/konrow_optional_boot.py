# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the delivered Konrow launcher to the native loader command."""

from __future__ import annotations

import json
import runpy
import sys
from contextlib import ExitStack
from pathlib import Path
from types import SimpleNamespace
from typing import TYPE_CHECKING
from unittest import mock

if TYPE_CHECKING:
    from types import ModuleType


class LoaderReachedError(Exception):
    """The platform adapter reached the test-owned loader boundary."""


def main() -> None:
    archive, work = (Path(value).resolve() for value in sys.argv[1:])
    sys.path.insert(0, str(archive / "scripts"))
    from fplinux_cli.runtime import boot, phone_data, workflow  # noqa: PLC0415
    from fplinux_cli.runtime.bundle import load_adapter  # noqa: PLC0415
    from fplinux_cli.runtime.ssh import session as ssh_session  # noqa: PLC0415

    fake_usb = work / "bootrom-node"
    fake_usb.touch()

    def record_loader(arguments: list[str], **_options: object) -> None:
        (work / "loader-arguments.json").write_text(json.dumps(arguments), encoding="ascii")
        raise LoaderReachedError

    def load_isolated_adapter(path: Path) -> ModuleType:
        adapter = load_adapter(path)
        assert adapter.__file__ is not None
        assert Path(adapter.__file__).resolve().is_relative_to(work.parent)
        adapter_patches.enter_context(
            mock.patch.multiple(
                adapter,
                usb_device_path=lambda _vendor, _product: fake_usb,
                shutil=SimpleNamespace(which=lambda _name: str(work / "stdbuf")),
                subprocess=SimpleNamespace(run=record_loader, STDOUT=None),
            )
        )
        return adapter

    launcher = archive / "fplinux.py"
    sys.argv = [
        str(launcher),
        "run",
        "konrow-senior-280-plus",
        "--phone-data",
        str(work / "phone-store"),
        "--events",
        str(work / "events.jsonl"),
    ]
    with (
        ExitStack() as adapter_patches,
        mock.patch.object(workflow, "host_preflight"),
        mock.patch.object(boot, "host_preflight"),
        mock.patch.object(boot, "USB_RULES_FILE", fake_usb),
        mock.patch.object(boot, "load_adapter", side_effect=load_isolated_adapter),
        mock.patch.object(
            ssh_session,
            "prepare_session",
            side_effect=lambda image, *_args, **_kwargs: {
                "image": image,
                "session_id": "a" * 64,
            },
        ),
        mock.patch.object(ssh_session, "finish_session"),
        mock.patch.object(ssh_session, "remove_personalized_image"),
        mock.patch.object(
            phone_data,
            "prepare_phone_data",
            side_effect=AssertionError("ordinary Konrow boot requested phone-data preparation"),
        ),
    ):
        try:
            runpy.run_path(str(launcher), run_name="__main__")
        except LoaderReachedError:
            return
    message = "the shipped launcher never called its loader"
    raise AssertionError(message)


if __name__ == "__main__":
    main()
