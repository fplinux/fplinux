# SPDX-License-Identifier: MIT
# ruff: noqa: ANN001, ANN201, ANN202, ANN204, D102, D107, INP001, N802
# mypy: ignore-errors
"""Numeric keypad multi-tap test activity for FPLinux phones."""

import time

import lvgl as lv
from mpos import Activity
from mpos.fplinux_multitap import MultiTapEngine
from mpos.ui.fplinux_small_screen_layout import keypad_test_layout

TARGET_TEXT = "hello"


class KeypadTest(Activity):
    """Show and exercise the phone keypad's multi-tap text entry path."""

    def __init__(self):
        super().__init__()
        self._engine = MultiTapEngine(elapsed=time.ticks_diff)
        self._timer = None

    def onCreate(self):
        display = lv.display_get_default()
        width = display.get_horizontal_resolution()
        height = display.get_vertical_resolution()
        layout = keypad_test_layout(width, height)
        padding = layout["padding"]
        content_width = width - (padding * 2)
        text_height = layout["text_height"]

        self._screen = lv.obj()
        self._screen.remove_flag(lv.obj.FLAG.SCROLLABLE)
        self._screen.add_event_cb(self._on_key, lv.EVENT.KEY, None)

        title = lv.label(self._screen)
        title.set_text("Keypad test")
        title.set_width(content_width)
        title.set_style_text_align(lv.TEXT_ALIGN.CENTER, 0)
        title.set_pos(padding, padding)

        text_box = lv.obj(self._screen)
        text_box.set_size(content_width, text_height)
        text_box.set_pos(padding, layout["text_top"])
        text_box.remove_flag(lv.obj.FLAG.SCROLLABLE)

        self._text_label = lv.label(text_box)
        self._text_label.set_size(content_width - 8, text_height - 8)
        self._text_label.set_pos(4, 4)
        self._text_label.set_long_mode(lv.label.LONG_MODE.WRAP)

        self._state_label = lv.label(self._screen)
        self._state_label.set_size(content_width, layout["state_height"])
        self._state_label.set_pos(*layout["state_pos"])

        controls = lv.label(self._screen)
        controls.set_text("2-9: type/cycle\n*: erase  #: case\nEnter: commit")
        controls.set_size(content_width, layout["controls_height"])
        controls.set_long_mode(lv.label.LONG_MODE.WRAP)
        controls.set_pos(*layout["controls_pos"])

        group = lv.group_get_default()
        if group is None:
            message = "LVGL default input group is unavailable"
            raise RuntimeError(message)
        group.add_obj(self._screen)
        self.setContentView(self._screen)
        lv.group_focus_obj(self._screen)
        self._timer = lv.timer_create(self._on_timer, 50, None)
        self._refresh()

    def onDestroy(self, screen):
        del screen
        if self._timer:
            self._timer.delete()
            self._timer = None

    def _on_key(self, event):
        key = event.get_key()
        now_ms = time.ticks_ms()
        if key == lv.KEY.ENTER:
            self._engine.commit()
        elif key < 0 or key > 0x7F or not self._engine.handle(chr(key), now_ms):
            return
        self._refresh()

    def _on_timer(self, timer):
        del timer
        if self._engine.expire(time.ticks_ms()):
            self._refresh()

    def _refresh(self):
        text = self._engine.display_text or " "
        candidate = self._engine.candidate or "-"
        key = self._engine.pending_key or "-"
        result = "PASS: hello" if self._engine.text == TARGET_TEXT else "Target: hello"
        self._text_label.set_text(text)
        self._state_label.set_text(
            "Mode: " + self._engine.mode + "  Key: " + key + "  " + candidate + "\n" + result
        )
