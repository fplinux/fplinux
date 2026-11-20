# SPDX-License-Identifier: GPL-2.0-only
"""Typed requests shared by the terminal entry points and the bundle runtime."""

from __future__ import annotations

import re
from dataclasses import dataclass
from typing import TYPE_CHECKING, Any, Literal

from fplinux_cli.common import fail

from .bundle import MICROSD_BOOT_MODE
from .card_access import CARD_CID

if TYPE_CHECKING:
    from collections.abc import Callable
    from pathlib import Path

    from .bundle import ValidatedBundle
    from .card import SdCard


@dataclass(frozen=True, kw_only=True)
class _PhoneInputs:
    """Inputs shared by workflows that can prepare the phone or start a loader."""

    phone_data: Path | None = None
    from_dump: Path | None = None
    events: Path | None = None
    loader_trace: bool = False


@dataclass(frozen=True, kw_only=True)
class RunRequest(_PhoneInputs):
    """Start the selected model in RAM or from an existing system card."""

    target: str | None = None
    boot: str | None = None


@dataclass(frozen=True, kw_only=True)
class LiveRequest:
    """Verify or use the selected authenticated live session."""

    target: str | None = None
    boot: str | None = None
    card_cid: str | None = None
    phone_data: Path | None = None
    verify_only: bool = False
    allow_other_build: bool = False
    shutdown: bool = False
    exec_command: str | None = None
    upload: list[str] | None = None
    pull: list[str] | None = None
    screenshot: Path | None = None


@dataclass(frozen=True, kw_only=True)
class PrepareRequest(_PhoneInputs):
    """Prepare persistent phone data from the selected source."""

    target: str | None = None
    replace: bool = False


@dataclass(frozen=True, kw_only=True)
class NandRequest:
    """Read the fitted NAND chip identity or a complete image."""

    action: Literal["identify", "backup"]
    target: str | None = None
    phone_data: Path | None = None
    output: Path | None = None


@dataclass(frozen=True, kw_only=True)
class CardRequest(_PhoneInputs):
    """Inspect or change a system card through the RAM reader."""

    target: str | None = None
    action: CardAction = "install"
    erase_card: str | None = None
    card_cid: str | None = None
    build_id: str | None = None


type CardAction = Literal["inspect", "install", "update", "rollback"]
type Operation = RunRequest | LiveRequest | PrepareRequest | NandRequest | CardRequest


@dataclass(frozen=True)
class BundlePaths:
    """The selected artifacts and the installation paths they use."""

    bundle: Path
    data: Path
    logs: Path
    connection: tuple[Any, dict[str, Any]] | None = None
    card_bundle: Path | None = None


@dataclass(frozen=True)
class SelectedConnection:
    """A saved binding and the expected build policy supplied by the checkout."""

    connection: tuple[Any, dict[str, Any]] | None
    data: Path
    logs: Path
    admitted: ValidatedBundle | None
    revalidate: Callable[[], None] | None = None


def session_selection_error(
    request: RunRequest | LiveRequest, *, system_card_profile: bool = False
) -> str | None:
    """Validate the selection before a frontend acquires state or a USB session."""
    if isinstance(request, LiveRequest):
        actions = int(request.shutdown) + sum(
            action is not None
            for action in (request.exec_command, request.upload, request.pull, request.screenshot)
        )
        if request.verify_only and (request.allow_other_build or actions):
            return "Verification requires a selected build and no remote action"
        if actions > 1:
            return "Select one console action"
        if request.shutdown and (request.allow_other_build or request.phone_data is not None):
            return "--shutdown cannot be combined with --allow-other-build or --phone-data"
        if request.allow_other_build and (
            request.boot is not None or request.phone_data is not None
        ):
            return "--allow-other-build cannot be combined with --boot or --phone-data"
    card_cid = request.card_cid if isinstance(request, LiveRequest) else None
    if (request.boot == MICROSD_BOOT_MODE or system_card_profile or card_cid is not None) and (
        request.phone_data is not None
        or (isinstance(request, RunRequest) and request.from_dump is not None)
    ):
        return "system microSD boot uses the phone data already on the card"
    return _enrolled_card_selection_error(request.target, card_cid)


def _enrolled_card_selection_error(target: str | None, cid: str | None) -> str | None:
    if cid is None:
        return None
    if target is None:
        return "--card requires an explicit TARGET"
    if CARD_CID.fullmatch(cid) is None:
        return "--card must be a 32-digit lowercase SD card CID"
    return None


def card_selection_error(
    action: CardAction,
    erase_card: str | None,
    card_cid: str | None,
    build_id: str | None,
    *,
    may_confirm: bool,
) -> str | None:
    """Require the same explicit card identity or terminal confirmation at each entry."""
    if action == "rollback":
        if build_id is None or re.fullmatch(r"[a-f0-9]{12}", build_id) is None:
            return "card rollback requires --build-id with 12 lowercase hexadecimal digits"
    elif build_id is not None:
        return "--build-id is only valid for card rollback"
    if action == "inspect":
        return None if erase_card is None and card_cid is None else "card inspect takes no CID"
    install = action == "install"
    option = "--erase-card" if install else "--card"
    if (card_cid if install else erase_card) is not None:
        return f"card {action} uses {option} CID"
    cid = erase_card if install else card_cid
    if cid is None:
        return None if may_confirm else f"card {action} requires {option} CID"
    return (
        None if CARD_CID.fullmatch(cid) else f"{option} must be a 32-digit lowercase SD card CID"
    )


def _absolute(path: Path | None) -> Path | None:
    return path.resolve() if path is not None else None


def execute_operation(
    request: Operation,
    paths: BundlePaths | SelectedConnection,
    *,
    confirm_card: Callable[[SdCard], bool] | None = None,
) -> int:
    """Execute an operation against artifacts or a live session selected by its frontend."""
    if isinstance(request, RunRequest | LiveRequest) and (
        error := session_selection_error(request)
    ):
        fail(error)
    if isinstance(request, LiveRequest):
        from . import live  # noqa: PLC0415 -- selected runtime action.

        if request.card_cid is not None:
            return live.console_card(request, paths)
        if isinstance(paths, SelectedConnection):
            if paths.connection is None:
                fail("No saved phone connection was selected")
            return live.console_connection(
                paths.connection,
                paths.data,
                paths.logs,
                admitted=paths.admitted,
                revalidate=paths.revalidate,
                phone_data=request.phone_data,
                exec_command=request.exec_command,
                upload=request.upload,
                pull=request.pull,
                screenshot=request.screenshot,
                verify_only=request.verify_only,
                shutdown=request.shutdown,
            )
        return live.console_bundle(
            paths.bundle,
            paths.data,
            paths.logs,
            phone_data=_absolute(request.phone_data),
            allow_other_build=request.allow_other_build,
            exec_command=request.exec_command,
            upload=request.upload,
            pull=request.pull,
            screenshot=request.screenshot,
            verify_only=request.verify_only,
            shutdown=request.shutdown,
            connection=paths.connection,
        )
    if isinstance(paths, SelectedConnection):
        fail("This operation requires a selected runtime bundle")
    if isinstance(request, CardRequest):
        from .card import run_card  # noqa: PLC0415 -- selected runtime action.

        return run_card(
            paths.bundle,
            paths.data,
            paths.logs,
            card_bundle=paths.card_bundle,
            action=request.action,
            erase_card=request.erase_card,
            card_cid=request.card_cid,
            build_id=request.build_id,
            confirm=confirm_card,
            phone_data=_absolute(request.phone_data),
            from_dump=_absolute(request.from_dump),
            events=request.events,
            loader_trace=request.loader_trace,
        )
    from .workflow import run_phone  # noqa: PLC0415 -- selected runtime action.

    if isinstance(request, NandRequest):
        return run_phone(
            paths.bundle,
            paths.data,
            paths.logs,
            phone_data=_absolute(request.phone_data),
            nand_action=request.action,
            nand_output=_absolute(request.output),
        )
    return run_phone(
        paths.bundle,
        paths.data,
        paths.logs,
        phone_data=_absolute(request.phone_data),
        from_dump=_absolute(request.from_dump),
        prepare_only=isinstance(request, PrepareRequest),
        replace=isinstance(request, PrepareRequest) and request.replace,
        events=request.events,
        loader_trace=request.loader_trace,
    )
