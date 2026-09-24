# SPDX-License-Identifier: GPL-2.0-only
"""Host tests for the MicroPythonOS phone keypad and keyboard adapter.

The shipped adapter runs against fake LVGL, MicroPythonOS and native keypad modules. The
tests observe the LVGL key data, navigation calls and text-entry calls that it produces;
they do not load LVGL, run MicroPython, read an input device or exercise a phone. The fake
native module publishes the phone key codes as literals, so these tests do not show that
the compiled module exports the same values.
"""

from __future__ import annotations

import importlib.util
import sys
import time
import unittest
from pathlib import Path
from types import ModuleType, SimpleNamespace
from typing import TYPE_CHECKING
from unittest.mock import patch

if TYPE_CHECKING:
    from collections.abc import Callable

ROOT = Path(__file__).resolve().parents[2]
ADAPTER = ROOT / "alpine/aports/fplinux-micropythonos/fplinux.py"

# Source identifiers exported by the fake native module.
KEYPAD = 1
KEYBOARD = 2

# Phone key codes from include/fplinux/fplinux-keypad.h, under the names that the
# native module exports.
PHONE_KEY_CODES = {
    "KEY_0": 0x1C4,
    "KEY_1": 0x1C5,
    "KEY_2": 0x1C6,
    "KEY_3": 0x1C7,
    "KEY_4": 0x1C8,
    "KEY_5": 0x1C9,
    "KEY_6": 0x1CA,
    "KEY_7": 0x1CB,
    "KEY_8": 0x1CC,
    "KEY_9": 0x1CD,
    "KEY_STAR": 0x1CE,
    "KEY_POUND": 0x1CF,
    "KEY_UP": 0x233,
    "KEY_DOWN": 0x234,
    "KEY_LEFT": 0x235,
    "KEY_RIGHT": 0x236,
    "KEY_OK": 0x237,
    "KEY_SOFT_LEFT": 0x238,
    "KEY_SOFT_RIGHT": 0x239,
    "KEY_CALL": 0x23A,
}
PHONE_POWER = 0x23B

# Linux input-event-codes.h values of keyboard keys.
KEY_ESC = 1
KEY_7 = 8
KEY_BACKSPACE = 14
KEY_TAB = 15
KEY_ENTER = 28
KEY_KPENTER = 96
KEY_UP = 103
KEY_LEFT = 105
KEY_RIGHT = 106
KEY_DOWN = 108

PRESSED = "pressed"
RELEASED = "released"

type Event = tuple[int, int, int, str]


class FakeIndev:
    """Keep the read callback that the adapter registers with LVGL."""

    def __init__(self) -> None:
        """Start without a registered callback."""
        self.read_callback: Callable[[FakeIndev, SimpleNamespace], None] | None = None

    def set_read_cb(self, callback: Callable[[FakeIndev, SimpleNamespace], None]) -> None:
        """Store the adapter's read callback."""
        self.read_callback = callback

    def __getattr__(self, name: str) -> Callable[..., None]:
        """Accept the remaining LVGL input-device configuration calls."""
        del name
        return lambda *_arguments: None


class FakeDisplay:
    """Report a valid framebuffer geometry to the adapter."""

    @staticmethod
    def get_horizontal_resolution() -> int:
        """Return a positive width."""
        return 240

    @staticmethod
    def get_vertical_resolution() -> int:
        """Return a positive height."""
        return 320

    @staticmethod
    def set_dpi(dpi: int) -> None:
        """Accept the display density."""
        del dpi


class FakeGroup:
    """LVGL default group with no focused widget."""

    def __init__(self) -> None:
        """Start with no data sent to the group."""
        self.sent: list[int] = []
        self.focused: object | None = None

    def get_focused(self) -> object | None:
        """Return the widget currently receiving keyboard input."""
        return self.focused

    def send_data(self, value: int) -> None:
        """Record key data sent to the focused widget."""
        self.sent.append(value)


class FakeNavigation:
    """Stand in for MicroPythonOS focus, drawer and back navigation and record its use."""

    def __init__(self) -> None:
        """Start with no navigation."""
        self.actions: list[str] = []

    def move_focus_direction(self, angle: int) -> None:
        """Record a directional focus move."""
        self.actions.append(f"focus {angle}")

    def toggle_drawer(self) -> None:
        """Record opening or closing the application drawer."""
        self.actions.append("drawer")

    def back_screen(self) -> None:
        """Record returning to the previous screen."""
        self.actions.append("back")


class FakeTextEntry:
    """Stand in for the MicroPythonOS multi-tap owner and record what reaches it."""

    def __init__(self) -> None:
        """Start with text entry inactive."""
        self.active = False
        self.dispatched: list[str] = []
        self.inserted: list[str] = []
        self.submissions = 0
        self.dismissals = 0

    def is_active(self) -> bool:
        """Report whether a text field owns physical keys."""
        return self.active

    def dispatch(self, key: str, now_ms: int) -> bool:
        """Record a phone key sent to multi-tap composition."""
        del now_ms
        self.dispatched.append(key)
        return True

    def insert_active(self, text: str) -> bool:
        """Record literal keyboard text."""
        self.inserted.append(text)
        return True

    def erase_active(self, now_ms: int) -> bool:
        """Accept an erase request."""
        del now_ms
        return True

    def submit_active(self) -> None:
        """Record submitting the text entry."""
        self.submissions += 1

    def dismiss_active(self) -> None:
        """Record leaving the text entry."""
        self.dismissals += 1


class FakeNativeKeypad:
    """Queue input events for the adapter as the native fplinux_keypad module would."""

    def __init__(self) -> None:
        """Start with no queued events."""
        self.events: list[Event] = []

    def open(self) -> None:
        """Accept the input-session open call."""

    def read(self) -> Event | None:
        """Return the next queued event."""
        return self.events.pop(0) if self.events else None

    def pending(self) -> bool:
        """Report whether another event is queued."""
        return bool(self.events)


def fake_lvgl(indev: FakeIndev, group: FakeGroup) -> ModuleType:
    """Build the LVGL surface that the adapter uses."""
    module = ModuleType("lvgl")
    module.__dict__.update(
        {
            "KEY": SimpleNamespace(
                ENTER="enter", UP="up", DOWN="down", LEFT="left", RIGHT="right", BACKSPACE="bs"
            ),
            "INDEV_STATE": SimpleNamespace(PRESSED=PRESSED, RELEASED=RELEASED),
            "INDEV_TYPE": SimpleNamespace(KEYPAD="keypad"),
            "LOG_LEVEL": SimpleNamespace(ERROR="error"),
            "log_register_print_cb": lambda _callback: None,
            "linux_fbdev_create": FakeDisplay,
            "linux_fbdev_set_file": lambda _display, _path: None,
            "indev_create": lambda: indev,
            "group_get_default": lambda: group,
            "keyboard": type("keyboard", (), {}),
            "textarea": type("textarea", (), {}),
            "dropdown": type("dropdown", (), {}),
        }
    )
    return module


def fake_runtime_modules(
    keypad: FakeNativeKeypad,
    text_entry: FakeTextEntry,
    navigation: FakeNavigation,
    lvgl: ModuleType,
) -> dict[str, ModuleType]:
    """Build the MicroPythonOS and native modules that the adapter imports."""
    mpos_ui = ModuleType("mpos.ui")
    mpos_ui.__dict__.update(
        {
            "focus_direction": SimpleNamespace(
                move_focus_direction=navigation.move_focus_direction
            ),
            "topmenu": SimpleNamespace(toggle_drawer=navigation.toggle_drawer),
            "back_screen": navigation.back_screen,
        }
    )
    mpos = ModuleType("mpos")
    mpos.__dict__.update(
        {
            "ui": mpos_ui,
            "DeviceInfo": SimpleNamespace(set_hardware_id=lambda _name: None),
            "InputManager": SimpleNamespace(register_indev=lambda _device: None),
            "fplinux_multitap": text_entry,
            "fplinux_storage": SimpleNamespace(install=lambda: None),
        }
    )
    native = ModuleType("fplinux_keypad")
    native.__dict__.update(
        {
            "KEYPAD": KEYPAD,
            "KEYBOARD": KEYBOARD,
            **PHONE_KEY_CODES,
            "open": keypad.open,
            "read": keypad.read,
            "pending": keypad.pending,
        }
    )
    return {"lvgl": lvgl, "mpos": mpos, "mpos.ui": mpos_ui, "fplinux_keypad": native}


def load_adapter() -> None:
    """Execute the shipped adapter with the runtime modules already in sys.modules."""
    specification = importlib.util.spec_from_file_location("fplinux_board_adapter", ADAPTER)
    if specification is None or specification.loader is None:
        message = "cannot load the FPLinux MicroPythonOS adapter"
        raise RuntimeError(message)
    adapter = importlib.util.module_from_spec(specification)
    # The adapter reads the host's Devicetree model only as a display name.
    with patch("builtins.open", side_effect=OSError):
        specification.loader.exec_module(adapter)


class MicroPythonOsKeypadAdapterTests(unittest.TestCase):
    """Drive the adapter's LVGL read callback with phone keypad and keyboard events."""

    def setUp(self) -> None:
        """Load a fresh adapter with inactive text entry."""
        self.keypad = FakeNativeKeypad()
        self.text_entry = FakeTextEntry()
        self.navigation = FakeNavigation()
        self.indev = FakeIndev()
        self.group = FakeGroup()
        modules = fake_runtime_modules(
            self.keypad, self.text_entry, self.navigation, fake_lvgl(self.indev, self.group)
        )
        # The adapter imports the drawer module when the left soft key is pressed.
        runtime = patch.dict(sys.modules, modules)
        runtime.start()
        self.addCleanup(runtime.stop)
        ticks = patch.object(time, "ticks_ms", create=True, return_value=1000)
        ticks.start()
        self.addCleanup(ticks.stop)
        load_adapter()

    def read(self, source: int, code: int, value: int, text: str = "") -> SimpleNamespace:
        """Pass one input event through the adapter and return the LVGL key data."""
        self.keypad.events.append((source, code, value, text))
        data = SimpleNamespace(key=None, state=None, continue_reading=None)
        if self.indev.read_callback is None:
            self.fail("the adapter did not register an LVGL read callback")
        self.indev.read_callback(self.indev, data)
        return data

    def tap(self, source: int, code: int) -> tuple[SimpleNamespace, SimpleNamespace]:
        """Press and release one key and return both LVGL key data results."""
        return self.read(source, code, 1), self.read(source, code, 0)

    def test_phone_digit_star_and_pound_press_their_characters(self) -> None:
        """The phone keypad's text keys reach LVGL as the character printed on the key."""
        cases = (
            (0x1C4, "0"),
            (0x1C6, "2"),
            (0x1C9, "5"),
            (0x1CD, "9"),
            (0x1CE, "*"),
            (0x1CF, "#"),
        )
        for code, character in cases:
            with self.subTest(code=hex(code)):
                pressed, released = self.tap(KEYPAD, code)

                self.assertEqual((pressed.key, pressed.state), (ord(character), PRESSED))
                self.assertEqual((released.key, released.state), (ord(character), RELEASED))

    def test_phone_digit_composes_multi_tap_in_active_text_entry(self) -> None:
        """A phone digit in a text field goes to multi-tap instead of LVGL."""
        self.text_entry.active = True

        pressed, _released = self.tap(KEYPAD, 0x1CB)

        self.assertEqual(self.text_entry.dispatched, ["7"])
        self.assertEqual(pressed.state, RELEASED)

    def test_keyboard_digit_is_literal_text_in_active_text_entry(self) -> None:
        """A keyboard digit in a text field is typed as text and never starts multi-tap."""
        self.text_entry.active = True

        self.read(KEYBOARD, KEY_7, 1, "7")
        self.read(KEYBOARD, KEY_7, 0)

        self.assertEqual(self.text_entry.inserted, ["7"])
        self.assertEqual(self.text_entry.dispatched, [])

    def test_keyboard_text_only_goes_to_focused_text_widget(self) -> None:
        """Typing over a non-text widget cannot insert characters into its LVGL group."""
        self.group.focused = object()
        self.read(KEYBOARD, KEY_7, 1, "7")
        self.assertEqual(self.group.sent, [])

        self.group.focused = sys.modules["lvgl"].textarea()
        self.read(KEYBOARD, KEY_7, 1, "7")
        self.assertEqual(self.group.sent, [ord("7")])

    def test_phone_and_keyboard_navigation_keys_share_their_actions(self) -> None:
        """The D-pad and soft keys navigate, and their keyboard counterparts do the same."""
        cases = (
            ("phone up", KEYPAD, 0x233, "focus 0"),
            ("keyboard up", KEYBOARD, KEY_UP, "focus 0"),
            ("phone right", KEYPAD, 0x236, "focus 90"),
            ("keyboard right", KEYBOARD, KEY_RIGHT, "focus 90"),
            ("phone down", KEYPAD, 0x234, "focus 180"),
            ("keyboard down", KEYBOARD, KEY_DOWN, "focus 180"),
            ("phone left", KEYPAD, 0x235, "focus 270"),
            ("keyboard left", KEYBOARD, KEY_LEFT, "focus 270"),
            ("phone left soft", KEYPAD, 0x238, "drawer"),
            ("keyboard Tab", KEYBOARD, KEY_TAB, "drawer"),
            ("phone right soft", KEYPAD, 0x239, "back"),
            ("keyboard Esc", KEYBOARD, KEY_ESC, "back"),
            ("keyboard Backspace outside a text field", KEYBOARD, KEY_BACKSPACE, "back"),
        )
        for name, source, code, action in cases:
            with self.subTest(name):
                self.navigation.actions.clear()

                pressed, _released = self.tap(source, code)

                self.assertEqual(self.navigation.actions, [action])
                self.assertEqual(pressed.state, RELEASED)

    def test_phone_ok_call_and_keyboard_enter_press_lvgl_enter(self) -> None:
        """The centre key, the green call key and keyboard Enter all select."""
        cases = (
            ("phone centre", KEYPAD, 0x237),
            ("phone call", KEYPAD, 0x23A),
            ("keyboard Enter", KEYBOARD, KEY_ENTER),
            ("keyboard keypad Enter", KEYBOARD, KEY_KPENTER),
        )
        for name, source, code in cases:
            with self.subTest(name):
                pressed, released = self.tap(source, code)

                self.assertEqual((pressed.key, pressed.state), ("enter", PRESSED))
                self.assertEqual((released.key, released.state), ("enter", RELEASED))

    def test_phone_keys_submit_or_leave_active_text_entry(self) -> None:
        """Centre and call submit the text entry; the right soft key leaves it."""
        self.text_entry.active = True

        self.tap(KEYPAD, 0x237)
        self.tap(KEYPAD, 0x23A)
        self.tap(KEYPAD, 0x239)

        self.assertEqual(self.text_entry.submissions, 2)
        self.assertEqual(self.text_entry.dismissals, 1)
        self.assertEqual(self.navigation.actions, [])

    def test_phone_power_key_has_no_action(self) -> None:
        """The red key neither presses an LVGL key, navigates nor edits text."""
        self.text_entry.active = True

        pressed, _released = self.tap(KEYPAD, PHONE_POWER)

        self.assertEqual(pressed.state, RELEASED)
        self.assertEqual(self.navigation.actions, [])
        self.assertEqual(self.text_entry.dispatched, [])
        self.assertEqual(self.text_entry.submissions, 0)
        self.assertEqual(self.text_entry.dismissals, 0)


if __name__ == "__main__":
    unittest.main()
