# SPDX-License-Identifier: MIT
# ruff: noqa: ANN001, ANN002, ANN003, ANN201, ANN202, ANN204, ANN205, BLE001, D102, D107, EM101, FBT003, I001, INP001, PLC0415
# mypy: ignore-errors
"""Generic FPLinux framebuffer and keypad board adaptation."""

import time

import lvgl as lv
import mpos.ui
from mpos import DeviceInfo, InputManager, fplinux_multitap
from mpos.ui import focus_direction

import fplinux_keypad
from mpos import fplinux_storage


KEY_1 = 2
KEY_2 = 3
KEY_3 = 4
KEY_4 = 5
KEY_5 = 6
KEY_6 = 7
KEY_7 = 8
KEY_8 = 9
KEY_9 = 10
KEY_0 = 11
KEY_BACKSPACE = 14
KEY_TAB = 15
KEY_ENTER = 28
KEY_KPASTERISK = 55
KEY_KPDOT = 83
KEY_UP = 103
KEY_LEFT = 105
KEY_RIGHT = 106
KEY_DOWN = 108

_DIGITS = {
    KEY_0: ord("0"),
    KEY_1: ord("1"),
    KEY_2: ord("2"),
    KEY_3: ord("3"),
    KEY_4: ord("4"),
    KEY_5: ord("5"),
    KEY_6: ord("6"),
    KEY_7: ord("7"),
    KEY_8: ord("8"),
    KEY_9: ord("9"),
    KEY_KPASTERISK: ord("*"),
    KEY_KPDOT: ord("#"),
}

_DEVICE_MODEL_PATHS = (
    "/proc/device-tree/model",
    "/sys/firmware/devicetree/base/model",
)


def _device_model():
    """Read the running board's presentation name without target policy."""
    for path in _DEVICE_MODEL_PATHS:
        try:
            with open(path, "rb") as device_tree:  # noqa: PTH123
                model = device_tree.read().decode("utf-8").strip("\x00 \t\r\n")
        except (OSError, UnicodeError):
            continue
        if model:
            return model
    return "FPLinux"


class FPLinuxDisplay:
    """LVGL display backed by the active Linux framebuffer geometry."""

    def __init__(self, path="/dev/fb0"):
        self._display = lv.linux_fbdev_create()
        lv.linux_fbdev_set_file(self._display, path)
        width = self._display.get_horizontal_resolution()
        height = self._display.get_vertical_resolution()
        if width <= 0 or height <= 0:
            raise RuntimeError("framebuffer has no valid geometry")
        self._display.set_dpi(130)

    def init(self, *args, **kwargs):
        del args, kwargs

    def set_backlight(self, percent):
        del percent
        return False

    def set_power(self, enabled):
        del enabled
        return False

    def set_rotation(self, rotation):
        self._display.set_rotation(rotation)

    @property
    def lv_display(self):
        return self._display


class FPLinuxKeypad:
    """Map FPLinux's normalized evdev keypad contract onto LVGL."""

    def __init__(self, display):
        self._last_key = lv.KEY.ENTER
        self.device = fplinux_keypad.open(True)
        self._indev = lv.indev_create()
        self._indev.set_type(lv.INDEV_TYPE.KEYPAD)
        self._indev.set_read_cb(self._read)
        self._indev.set_group(lv.group_get_default())
        self._indev.set_display(display)
        self._indev.set_long_press_time(400)
        self._indev.set_long_press_repeat_time(100)
        self._indev.enable(True)

    @property
    def indev(self):
        return self._indev

    @staticmethod
    def _focused_widget_navigation_key(code):
        group = lv.group_get_default()
        focused = group.get_focused() if group else None
        navigation_key = {
            KEY_UP: lv.KEY.UP,
            KEY_RIGHT: lv.KEY.RIGHT,
            KEY_DOWN: lv.KEY.DOWN,
            KEY_LEFT: lv.KEY.LEFT,
        }.get(code)
        if navigation_key is None or focused is None:
            return None
        if isinstance(focused, lv.keyboard):
            return navigation_key
        if isinstance(focused, lv.textarea) and fplinux_multitap.is_active():
            return navigation_key
        if isinstance(focused, lv.dropdown):
            try:
                return navigation_key if focused.is_open() else None
            except Exception:
                return None
        return None

    @staticmethod
    def _dispatch_navigation(code):
        if code == KEY_UP:
            focus_direction.move_focus_direction(0)
        elif code == KEY_RIGHT:
            focus_direction.move_focus_direction(90)
        elif code == KEY_DOWN:
            focus_direction.move_focus_direction(180)
        elif code == KEY_LEFT:
            focus_direction.move_focus_direction(270)
        elif code == KEY_TAB:
            from mpos.ui import topmenu

            topmenu.toggle_drawer()
        elif code == KEY_BACKSPACE:
            mpos.ui.back_screen()
        else:
            return False
        return True

    def _read(self, indev, data):  # noqa: PLR0911
        del indev
        data.continue_reading = False
        event = fplinux_keypad.read()
        if event is None:
            data.key = self._last_key
            data.state = lv.INDEV_STATE.RELEASED
            return

        code, value = event
        pressed = value != 0
        if code in (KEY_UP, KEY_RIGHT, KEY_DOWN, KEY_LEFT):
            widget_key = self._focused_widget_navigation_key(code)
            if widget_key is not None:
                self._last_key = widget_key
                data.key = widget_key
                data.state = lv.INDEV_STATE.PRESSED if pressed else lv.INDEV_STATE.RELEASED
                return
            if pressed:
                self._dispatch_navigation(code)
            data.key = self._last_key
            data.state = lv.INDEV_STATE.RELEASED
            return
        if code == KEY_BACKSPACE and fplinux_multitap.is_active():
            if value == 1:
                fplinux_multitap.dismiss_active()
            data.key = self._last_key
            data.state = lv.INDEV_STATE.RELEASED
            return
        if code in (KEY_TAB, KEY_BACKSPACE):
            if pressed:
                if code == KEY_TAB:
                    fplinux_multitap.dismiss_active()
                self._dispatch_navigation(code)
            data.key = self._last_key
            data.state = lv.INDEV_STATE.RELEASED
            return

        text_key = _DIGITS.get(code)
        if text_key is not None and fplinux_multitap.is_active():
            if value == 1:
                fplinux_multitap.dispatch(chr(text_key), time.ticks_ms())
            data.key = self._last_key
            data.state = lv.INDEV_STATE.RELEASED
            return

        if code == KEY_ENTER:
            if fplinux_multitap.is_active():
                if value == 1:
                    fplinux_multitap.submit_active()
                data.key = self._last_key
                data.state = lv.INDEV_STATE.RELEASED
                return
            key = lv.KEY.ENTER
        else:
            key = text_key
            if key is None:
                data.key = self._last_key
                data.state = lv.INDEV_STATE.RELEASED
                return

        self._last_key = key
        data.key = key
        data.state = lv.INDEV_STATE.PRESSED if pressed else lv.INDEV_STATE.RELEASED


fplinux_storage.install()
mpos.ui.main_display = FPLinuxDisplay()
keypad = FPLinuxKeypad(mpos.ui.main_display.lv_display)
InputManager.register_indev(keypad.indev)
DeviceInfo.set_hardware_id(_device_model())
