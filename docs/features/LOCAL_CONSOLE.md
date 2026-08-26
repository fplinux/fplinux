# Local console

Every current target starts an interactive root shell on the phone itself. It
does not need the USB cable after Linux has started. Screen size, panel
orientation and the physical labels on a particular phone remain in that
phone's target document.

The console accepts the same normalized keypad controls on every current
target.

## Text entry

The console starts in T9 multi-tap mode. This is character cycling, not
dictionary prediction: press a digit repeatedly to choose its character. `1`
selects punctuation and `0` selects a space. Pause briefly or press another key
to commit the current character.

- Digits `0` through `9` enter text with T9 multi-tap.
- Tapping `*` in T9 cycles Ctrl, Alt, Shift or no modifier for the next key.
- Tapping `*` in QWERTY types `*`.
- Holding `*` switches between T9 and QWERTY input.
- The left soft key sends Tab.
- The right soft key cancels the pending character or sends Backspace.
- The centre or dial key sends Enter.
- The D-pad moves through shell input and programs.

In QWERTY mode the Linux console keymap translates keyboard input. This is the
mode to use with the [host keyboard bridge](HOST_KEYBOARD.md). The status row
shows the active input mode and an armed one-shot modifier.

## Scrollback

Press `#` to enter or leave the console's scrollback view. It does not stop the
shell. In that view, Up and Down move one line at a time; Left and Right move a
screen at a time. Press `#` or the right soft key to return to the live prompt.

## What the console is not

The local console is a normal Linux virtual terminal with `TERM=linux`, not a
phone-specific menu system. It has no separate application launcher, network
configuration screen or persistent user account. The RAM session, storage and
hardware limits still come from the selected target document.

For commands from the host, see [SSH sessions](SSH.md). The selected phone page
lists its display and keypad support.
