# Local console

Every current target starts an interactive root shell on the phone itself. It
does not need the USB cable after Linux has started. Screen size, panel
orientation and the physical labels on a particular phone remain in that
phone's target document.

The console accepts the same phone keypad controls on every current target.
A keyboard, including the [host keyboard bridge](HOST_KEYBOARD.md), types at
the same time through the Linux console keymap; its keys, `*` and `#` included,
are ordinary keyboard input.

## Phone keypad

The phone keypad enters text with multi-tap: press a digit repeatedly to choose
its character. It does not predict words. `1` selects punctuation and `0`
selects a space. Pause briefly or press another key to commit the current
character.

- Digits `0` through `9` enter text with multi-tap.
- `*` cycles Ctrl, Alt, Shift or no modifier for the next multi-tap character.
  Ctrl applies to letters, `@`, `^`, `_` and `?`; other characters are
  refused. Enter, Backspace and the D-pad ignore the modifier and leave it
  armed.
- `#` opens the scrollback view.
- The left soft key sends Tab, or Esc followed by Tab with Shift; Ctrl and Alt
  are refused.
- The right soft key cancels the pending character or sends Backspace.
- The centre or dial key sends Enter.
- The D-pad moves through shell input and programs.
- The power key is not console input; see [power-off](POWER_OFF.md).

The pending character is shown at the cursor. When no character is pending, an
armed one-shot modifier appears there as `C`, `A` or `S`. A refused key shows
`!` there briefly.

## Scrollback

Press `#` to enter the console's scrollback view. It does not stop the shell.
In that view, Up and Down move one line at a time; Left and Right move a screen
at a time, and the centre or dial key returns to the newest line. Press `#` or
the right soft key to return to the live prompt. Keyboard input is ignored
while the view is open.

## What the console is not

The local console is a normal Linux virtual terminal with `TERM=linux`, not a
phone-specific menu system. It has no separate application launcher, network
configuration screen or persistent user account. The RAM session, storage and
hardware limits still come from the selected target document.

For commands from the host, see [SSH sessions](SSH.md). The selected phone page
lists its display and keypad support.
