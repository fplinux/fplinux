# SPDX-License-Identifier: GPL-2.0-only
"""Host tests for the MicroPythonOS Python multi-tap adapter.

These tests execute the shipped Python policy with a fake native engine. They do not
test the packaged keypad-test application, import the compiled MicroPython module,
load LVGL, or exercise a physical keypad.
"""

from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path
from types import ModuleType
from typing import TYPE_CHECKING, Protocol, cast

if TYPE_CHECKING:
    from collections.abc import Callable

ROOT = Path(__file__).resolve().parents[2]
ENGINE_PATH = ROOT / "alpine/aports/fplinux-micropythonos/fplinux-multitap.py"
NATIVE_MODULE_NAME = "fplinux_multitap_native"


class MultiTapAdapter(Protocol):
    """Python policy surface provided by the packaged adapter."""

    text: str
    uppercase: bool

    @property
    def candidate(self) -> str:
        """Return the displayed composition candidate."""
        ...

    @property
    def display_text(self) -> str:
        """Return committed text followed by the candidate."""
        ...

    def press(self, key: str, now_ms: int) -> bool:
        """Pass a numeric key to the shared composition engine."""
        ...

    def handle(self, key: str, now_ms: int) -> bool:
        """Apply the shared physical digit, erase, and case policy."""
        ...

    def reset(self, text: str = "") -> None:
        """Replace committed text and clear pending composition."""
        ...

    def expire(self, now_ms: int) -> bool:
        """Commit a candidate whose shared deadline has elapsed."""
        ...

    def erase(self, now_ms: int) -> bool:
        """Cancel composition or erase committed text."""
        ...

    def toggle_case(self, now_ms: int) -> None:
        """Commit the candidate and flip alphabetic case."""
        ...


class FakeNativeEngine:
    """Record adapter calls without reimplementing multi-tap behavior."""

    def __init__(self) -> None:
        """Initialize an independently scriptable native-engine double."""
        self.calls: list[tuple[object, ...]] = []
        self.candidate_value = ""
        self.pending_key_value = ""
        self.press_value: str | None = None
        self.expire_value: str | None = None
        self.commit_value: str | None = None

    def press(self, key: str, elapsed_ms: int) -> str | None:
        """Return the prearranged raw core emission."""
        self.calls.append(("press", key, elapsed_ms))
        return self.press_value

    def expire(self, elapsed_ms: int) -> str | None:
        """Return the prearranged raw timeout emission."""
        self.calls.append(("expire", elapsed_ms))
        if self.expire_value:
            self.candidate_value = ""
            self.pending_key_value = ""
        return self.expire_value

    def commit(self) -> str | None:
        """Return the prearranged raw explicit emission."""
        self.calls.append(("commit",))
        self.candidate_value = ""
        return self.commit_value

    def cancel(self) -> None:
        """Record cancellation requested by the Python text policy."""
        self.calls.append(("cancel",))
        self.candidate_value = ""
        self.pending_key_value = ""

    def candidate(self) -> str:
        """Expose the C core's current candidate as test data."""
        return self.candidate_value

    def pending_key(self) -> str:
        """Expose the C core's current candidate owner as test data."""
        return self.pending_key_value


class FakeNativeModule:
    """Construct independent fake native Engine objects for one adapter module."""

    def __init__(self) -> None:
        """Start with no Engine instances."""
        self.engines: list[FakeNativeEngine] = []

    def engine(self) -> FakeNativeEngine:
        """Construct one fake native engine."""
        native = FakeNativeEngine()
        self.engines.append(native)
        return native

    @staticmethod
    def handles(key: str) -> bool:
        """Accept the numeric key range the C core owns."""
        return len(key) == 1 and "0" <= key <= "9"


def load_engine_module(native: FakeNativeModule) -> ModuleType:
    """Load the adapter against a fake compiled module without host copies."""
    module = ModuleType(NATIVE_MODULE_NAME)
    module.__dict__.update({"Engine": native.engine, "handles": native.handles})
    previous = sys.modules.get(NATIVE_MODULE_NAME)
    sys.modules[NATIVE_MODULE_NAME] = module
    try:
        specification = importlib.util.spec_from_file_location(
            "fplinux_multitap_policy_test", ENGINE_PATH
        )
        if specification is None or specification.loader is None:
            message = "cannot load the FPLinux multi-tap adapter"
            raise RuntimeError(message)
        adapter_module = importlib.util.module_from_spec(specification)
        specification.loader.exec_module(adapter_module)
        return adapter_module
    finally:
        if previous is None:
            del sys.modules[NATIVE_MODULE_NAME]
        else:
            sys.modules[NATIVE_MODULE_NAME] = previous


def make_adapter() -> tuple[MultiTapAdapter, FakeNativeEngine]:
    """Create an adapter and capture its one native Engine instance."""
    native = FakeNativeModule()
    module = load_engine_module(native)
    adapter_type = cast("Callable[..., MultiTapAdapter]", module.__dict__["MultiTapEngine"])
    adapter = adapter_type(elapsed=lambda later, earlier: later - earlier)
    return adapter, native.engines[0]


class MicroPythonOsMultitapAdapterTests(unittest.TestCase):
    """Exercise Python adapter policy while replacing only its native dependency."""

    def test_fake_native_emissions_drive_text_case_and_erase_policy(self) -> None:
        """Apply adapter policy to explicitly scripted native-engine emissions."""
        adapter, native = make_adapter()

        native.candidate_value = "a"
        native.pending_key_value = "2"
        self.assertEqual(adapter.candidate, "a")
        self.assertEqual(adapter.display_text, "a")

        adapter.uppercase = True
        native.press_value = "a"
        native.candidate_value = ""
        self.assertTrue(adapter.press("2", 10))
        self.assertEqual(adapter.text, "A")

        native.candidate_value = "b"
        self.assertTrue(adapter.erase(11))
        self.assertEqual(adapter.text, "A")
        self.assertEqual(adapter.candidate, "")

        self.assertTrue(adapter.erase(12))
        self.assertEqual(adapter.text, "")
        self.assertFalse(adapter.erase(13))

        adapter.uppercase = False
        native.candidate_value = "c"
        native.commit_value = "c"
        adapter.toggle_case(14)
        self.assertEqual(adapter.text, "c")
        self.assertTrue(adapter.uppercase)
        native.candidate_value = "d"
        self.assertEqual(adapter.candidate, "D")

    def test_adapter_applies_digit_erase_case_policy_and_reset_state(self) -> None:
        """Apply digit, erase, case and reset behavior through one adapter instance."""
        adapter, native = make_adapter()

        native.candidate_value = "a"
        native.pending_key_value = "2"
        self.assertTrue(adapter.handle("2", 10))
        self.assertTrue(adapter.handle("*", 11))
        self.assertFalse(adapter.handle("x", 12))
        self.assertTrue(adapter.handle("#", 13))
        self.assertTrue(adapter.uppercase)

        adapter.text = "old"
        adapter.reset("new")
        self.assertEqual(adapter.text, "new")
        self.assertEqual(adapter.candidate, "")

    def test_adapter_routes_only_to_the_active_fake_input_owner(self) -> None:
        """Replace active owners without allowing a stale fake owner to deactivate one."""
        module = load_engine_module(FakeNativeModule())

        class InputTarget:
            """Record active-editor routing without duplicating text policy."""

            def __init__(self) -> None:
                """Initialize routing observations."""
                self.keys: list[tuple[str, int]] = []
                self.submits = 0
                self.dismissals = 0

            def handle_physical_key(self, key: str, now_ms: int) -> bool:
                """Record one routed text key."""
                self.keys.append((key, now_ms))
                return True

            def submit_physical_input(self) -> None:
                """Record an explicit input submission."""
                self.submits += 1

            def dismiss_physical_input(self) -> None:
                """Record leaving physical-input mode."""
                self.dismissals += 1

        first = InputTarget()
        second = InputTarget()
        module.activate(first)
        self.assertTrue(module.is_active(first))
        self.assertTrue(module.dispatch("2", 10))
        module.activate(second)
        module.deactivate(first)
        self.assertTrue(module.is_active(second))
        module.submit_active()
        module.dismiss_active()
        self.assertEqual(first.keys, [("2", 10)])
        self.assertEqual(second.submits, 1)
        self.assertEqual(second.dismissals, 1)
        module.deactivate(second)
        self.assertFalse(module.dispatch("3", 20))

    def test_adapter_passes_elapsed_time_without_resetting_it_during_polling(self) -> None:
        """Repeated polls pass time since the original press to the fake engine."""
        adapter, native = make_adapter()
        native.candidate_value = "a"
        native.pending_key_value = "2"
        self.assertTrue(adapter.press("2", 0))

        for now_ms in range(50, 700, 50):
            self.assertFalse(adapter.expire(now_ms))
        native.expire_value = "a"
        self.assertTrue(adapter.expire(700))
        self.assertEqual(adapter.text, "a")
        self.assertEqual(adapter.candidate, "")
        expire_elapsed = [call[1] for call in native.calls if call[0] == "expire"]
        self.assertEqual(expire_elapsed, list(range(50, 701, 50)))


if __name__ == "__main__":
    unittest.main()
