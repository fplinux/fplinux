# SPDX-License-Identifier: MIT
# ruff: noqa: ANN001, ANN002, ANN003, ANN201, ANN202, ANN204, ANN205, D102, D107, EM101, FBT003, I001, INP001, PLC0415
# mypy: ignore-errors
"""Generic FPLinux framebuffer, keypad and keyboard board adaptation."""

import logging
import sys
import time

import lvgl as lv
import mpos.ui
from mpos import DeviceInfo, InputManager, fplinux_multitap
from mpos.ui import focus_direction

import fplinux_keypad
from mpos import fplinux_storage

_logger = logging.getLogger(__name__)


def _lvgl_log(level, message):
    """Report LVGL failures without enabling its continuous performance output."""
    if level == lv.LOG_LEVEL.ERROR:
        _logger.error("%s", message.rstrip())


# Linux key codes of the keyboard keys that the adapter handles itself.
KEY_ESC = 1
KEY_BACKSPACE = 14
KEY_TAB = 15
KEY_ENTER = 28
KEY_KPENTER = 96
KEY_UP = 103
KEY_LEFT = 105
KEY_RIGHT = 106
KEY_DOWN = 108

# Phone keypad keys that enter the character printed on them.
_DIGITS = {
    fplinux_keypad.KEY_0: ord("0"),
    fplinux_keypad.KEY_1: ord("1"),
    fplinux_keypad.KEY_2: ord("2"),
    fplinux_keypad.KEY_3: ord("3"),
    fplinux_keypad.KEY_4: ord("4"),
    fplinux_keypad.KEY_5: ord("5"),
    fplinux_keypad.KEY_6: ord("6"),
    fplinux_keypad.KEY_7: ord("7"),
    fplinux_keypad.KEY_8: ord("8"),
    fplinux_keypad.KEY_9: ord("9"),
    fplinux_keypad.KEY_STAR: ord("*"),
    fplinux_keypad.KEY_POUND: ord("#"),
}

# Phone D-pad keys and the LVGL key that an open widget receives from each.
_DIRECTIONS = {
    fplinux_keypad.KEY_UP: lv.KEY.UP,
    fplinux_keypad.KEY_RIGHT: lv.KEY.RIGHT,
    fplinux_keypad.KEY_DOWN: lv.KEY.DOWN,
    fplinux_keypad.KEY_LEFT: lv.KEY.LEFT,
}

# Keyboard keys that act as a phone key: Tab is the left and Esc the right soft key.
_KEYBOARD_PHONE_KEYS = {
    KEY_UP: fplinux_keypad.KEY_UP,
    KEY_RIGHT: fplinux_keypad.KEY_RIGHT,
    KEY_DOWN: fplinux_keypad.KEY_DOWN,
    KEY_LEFT: fplinux_keypad.KEY_LEFT,
    KEY_ENTER: fplinux_keypad.KEY_OK,
    KEY_KPENTER: fplinux_keypad.KEY_OK,
    KEY_TAB: fplinux_keypad.KEY_SOFT_LEFT,
    KEY_ESC: fplinux_keypad.KEY_SOFT_RIGHT,
    KEY_BACKSPACE: fplinux_keypad.KEY_SOFT_RIGHT,
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
        lv.log_register_print_cb(_lvgl_log)
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
    """Map the phone keypad and physical keyboards onto LVGL."""

    def __init__(self, display):
        self._last_key = lv.KEY.ENTER
        fplinux_keypad.open()
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
        navigation_key = _DIRECTIONS.get(code)
        if navigation_key is None or focused is None:
            return None
        if isinstance(focused, lv.keyboard):
            return navigation_key
        if isinstance(focused, lv.textarea) and fplinux_multitap.is_active():
            return navigation_key
        if isinstance(focused, lv.dropdown):
            try:
                return navigation_key if focused.is_open() else None
            except Exception:  # noqa: BLE001 -- binding errors fall back to directional focus.
                return None
        return None

    @staticmethod
    def _dispatch_navigation(code):
        if code == fplinux_keypad.KEY_UP:
            focus_direction.move_focus_direction(0)
        elif code == fplinux_keypad.KEY_RIGHT:
            focus_direction.move_focus_direction(90)
        elif code == fplinux_keypad.KEY_DOWN:
            focus_direction.move_focus_direction(180)
        elif code == fplinux_keypad.KEY_LEFT:
            focus_direction.move_focus_direction(270)
        elif code == fplinux_keypad.KEY_SOFT_LEFT:
            from mpos.ui import topmenu

            topmenu.toggle_drawer()
        elif code == fplinux_keypad.KEY_SOFT_RIGHT:
            mpos.ui.back_screen()
        else:
            return False
        return True

    @staticmethod
    def _is_editing_text():
        if fplinux_multitap.is_active():
            return True
        group = lv.group_get_default()
        return group is not None and isinstance(group.get_focused(), lv.textarea)

    @staticmethod
    def _insert_text(text):
        if fplinux_multitap.is_active():
            fplinux_multitap.insert_active(text)
            return
        group = lv.group_get_default()
        if group is None:
            return
        for character in text:
            # LVGL key data holds one UTF-8 character in native byte order.
            group.send_data(int.from_bytes(character.encode(), sys.byteorder))

    @staticmethod
    def _erase_character():
        if fplinux_multitap.is_active():
            fplinux_multitap.erase_active(time.ticks_ms())
            return
        group = lv.group_get_default()
        if group is not None:
            group.send_data(lv.KEY.BACKSPACE)

    def _read(self, indev, data):
        del indev
        event = fplinux_keypad.read()
        if event is None:
            data.continue_reading = False
            data.key = self._last_key
            data.state = lv.INDEV_STATE.RELEASED
            return

        source, code, value, text = event
        if source == fplinux_keypad.KEYBOARD:
            self._read_keyboard(code, value, text, data)
        else:
            self._read_phone_key(code, value, data)
        data.continue_reading = fplinux_keypad.pending()

    def _read_keyboard(self, code, value, text, data):
        """Insert text literally and let other keys act as their phone key."""
        if text:
            if self._is_editing_text():
                self._insert_text(text)
        elif code == KEY_BACKSPACE and self._is_editing_text():
            if value == 1:
                self._erase_character()
        else:
            phone_code = _KEYBOARD_PHONE_KEYS.get(code)
            if phone_code is not None:
                self._read_phone_key(phone_code, value, data)
                return
        data.key = self._last_key
        data.state = lv.INDEV_STATE.RELEASED

    def _read_phone_key(self, code, value, data):  # noqa: PLR0911
        pressed = value != 0
        if code in _DIRECTIONS:
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
        if code == fplinux_keypad.KEY_SOFT_RIGHT and fplinux_multitap.is_active():
            if value == 1:
                fplinux_multitap.dismiss_active()
            data.key = self._last_key
            data.state = lv.INDEV_STATE.RELEASED
            return
        if code in (fplinux_keypad.KEY_SOFT_LEFT, fplinux_keypad.KEY_SOFT_RIGHT):
            if pressed:
                if code == fplinux_keypad.KEY_SOFT_LEFT:
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

        # The green call key selects like the centre key.
        if code in (fplinux_keypad.KEY_OK, fplinux_keypad.KEY_CALL):
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
