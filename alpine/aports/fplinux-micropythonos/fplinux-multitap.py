# SPDX-License-Identifier: MIT
# ruff: noqa: ANN001, ANN201, ANN202, ANN204, D107, INP001
# mypy: ignore-errors
"""MicroPythonOS text policy around FPLinux's shared multi-tap core."""

import time

import fplinux_multitap_native

_active_input = [None]


class MultiTapEngine:
    """Compose text from a physical numeric keypad."""

    def __init__(self, elapsed=None):
        self._engine = fplinux_multitap_native.Engine()
        self._elapsed = elapsed or time.ticks_diff
        self.text = ""
        self.uppercase = False
        self._last_press_ms = None

    @property
    def candidate(self):
        """Return the visible, not-yet-committed character."""
        return self._apply_case(self._engine.candidate())

    @property
    def pending_key(self):
        """Return the numeric key that owns the pending character."""
        return self._engine.pending_key()

    @property
    def mode(self):
        """Return the current alphabetic case mode for the UI."""
        return "ABC" if self.uppercase else "abc"

    @property
    def display_text(self):
        """Return committed text followed by the current candidate."""
        return self.text + self.candidate

    def _apply_case(self, character):
        return character.upper() if self.uppercase and character.isalpha() else character

    def reset(self, text=""):
        """Replace committed text and discard any pending candidate."""
        self._engine.cancel()
        self._last_press_ms = None
        self.text = text

    def _elapsed_ms(self, now_ms):
        if self._last_press_ms is None:
            return 0
        return max(0, self._elapsed(now_ms, self._last_press_ms))

    def _commit_character(self, character):
        if not character:
            return ""
        character = self._apply_case(character)
        self.text += character
        return character

    def _sync_last_press(self, now_ms):
        self._last_press_ms = now_ms if self._engine.candidate() else None

    def press(self, key, now_ms):
        """Start or cycle a numeric key at ``now_ms``."""
        if not fplinux_multitap_native.handles(key):
            return False
        self._commit_character(self._engine.press(key, self._elapsed_ms(now_ms)))
        self._sync_last_press(now_ms)
        return True

    def handle(self, key, now_ms):
        """Apply the canonical digit, erase, and case key policy."""
        if fplinux_multitap_native.handles(key):
            return self.press(key, now_ms)
        if key == "*":
            return self.erase(now_ms)
        if key == "#":
            self.toggle_case(now_ms)
            return True
        return False

    def expire(self, now_ms):
        """Commit a candidate that has waited at least ``timeout_ms``."""
        if self._last_press_ms is None:
            return False
        character = self._commit_character(self._engine.expire(self._elapsed_ms(now_ms)))
        if character:
            self._last_press_ms = None
        return bool(character)

    def commit(self):
        """Commit the candidate immediately and return it."""
        character = self._commit_character(self._engine.commit())
        self._sync_last_press(None)
        return character

    def erase(self, now_ms):
        """Cancel a candidate, or erase the last committed character."""
        self.expire(now_ms)
        if self._engine.candidate():
            self._engine.cancel()
            self._last_press_ms = None
            return True
        if not self.text:
            return False
        self.text = self.text[:-1]
        return True

    def toggle_case(self, now_ms):
        """Commit the current candidate and toggle lower/upper case."""
        self.expire(now_ms)
        self.commit()
        self.uppercase = not self.uppercase


def activate(input_target):
    """Route physical text keys to ``input_target`` until deactivated."""
    _active_input[0] = input_target


def deactivate(input_target):
    """Stop routing only when ``input_target`` still owns text entry."""
    if _active_input[0] is input_target:
        _active_input[0] = None


def is_active(input_target=None):
    """Return whether text entry is active, optionally for one owner."""
    if input_target is None:
        return _active_input[0] is not None
    return _active_input[0] is input_target


def dispatch(key, now_ms):
    """Send a physical text key to the active input owner."""
    if _active_input[0] is None:
        return False
    return _active_input[0].handle_physical_key(key, now_ms)


def submit_active():
    """Commit text and submit the active physical input session."""
    if _active_input[0] is not None:
        _active_input[0].submit_physical_input()


def dismiss_active():
    """Commit and leave the active physical input session."""
    if _active_input[0] is not None:
        _active_input[0].dismiss_physical_input()
