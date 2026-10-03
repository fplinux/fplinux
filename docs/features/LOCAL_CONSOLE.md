# Local console

The phone starts `fplinux-terminal`, a graphical terminal with an interactive
root Bash shell, when a DRM display is available. It runs under OpenRC and does
not need USB after Linux starts. The selected [target document](../../targets/README.md)
states the display's hardware support and limitations.

The terminal uses `TERM=xterm-256color`. Bash provides command editing,
completion, history and reverse search. The terminal keeps scrollback and
renders Unicode text with a Terminus font. The system root and its persistence
follow the selected boot profile.

The image installs one Terminus size for its display: 6×12 below 200 pixels
wide, or 8×16 on wider displays. `fplinux-terminal --font PATH` selects a PSF2
font already present on the phone; you can also copy another PSF2 file there.
The terminal, brightness control and showcase share this font. The two sizes
are packaged as `fplinux-font-terminus-6x12` and
`fplinux-font-terminus-8x16`, with paths
`/usr/share/fplinux/fonts/ter-u12n.psf` and `ter-u16n.psf`. Each package supplies
`/usr/share/fplinux/fonts/default.psf` pointing to its selected size. Applications
reject a default whose cell size does not match the display width. Only one
size is installed; the application bundle includes no font APKs.

## Phone keypad

Digits enter text with multi-tap: press a digit repeatedly to choose a
character, then pause or press another digit to commit it. `1` selects
punctuation and `0` selects a space. The pending character appears at the
cursor; the bottom line shows the language, input mode and soft-key actions.

| Key            | Action                                                             |
| -------------- | ------------------------------------------------------------------ |
| Digits         | Multi-tap text, or digits in `123` mode                            |
| `*`            | Switch between `abc` and `ABC`                                     |
| Hold `*`       | Switch between `123` and the remembered letter case                |
| `#`            | Cancel a pending character, otherwise Backspace                    |
| Hold `#`       | Clear the entire command line while Bash is editing its prompt     |
| Left soft      | Open the menu, or select its highlighted item                      |
| Hold left soft | Open the modifier strip                                            |
| Right soft     | Back in menus; otherwise clear modifiers or enter/leave scrollback |
| Centre         | Enter, or select a menu item                                       |
| Dial           | Tab completion                                                     |
| D-pad          | Arrow keys; navigate the menu or scrollback when open              |

Holding `#` does not clear a foreground application's input. The power key is
reserved for [power-off](POWER_OFF.md).

A short `*` changes the pending character's case without sending it. In `123`
mode it returns to letters with the opposite of the remembered case. Hold `*`
for about 650 ms to change between digits and letters without changing that
remembered case.

The menu opens with **Modifiers** selected. Up and Down move between actions.
**Symbols...**, **Special keys...** and **Language: EN/RU...** open choosers
for literal symbols, Escape and F1–F12, or English and Russian multi-tap.
The language item shows the current selection. Right soft (**Back**) returns
to the parent menu or closes the main menu.

**Interrupt Ctrl+C**, **End input Ctrl+D** and **Search Ctrl+R** send their
named shortcuts. **Help** has three sections: Typing, Modifiers and History.
Left and Right switch sections and return to the top of their text. Text wraps
automatically to the display and selected font, including the space used by
armed modifiers. Up and Down scroll one line when a section does not fit; the
toolbar shows the visible line range. Scrolling stops at the start and end of
the section. Right soft returns to the menu. An external keyboard can use the
arrow keys and Escape. While Help is open,
other keys do not type into the shell or switch to the diagnostic console.
Pending text and armed modifiers remain intact.

## Modifiers

Hold the left soft key for about 500 ms, then release it, to open the strip
above the toolbar. Two short left-soft presses also open **Modifiers**, the
first menu item. Left and Right select Ctrl, Alt or Shift; Centre toggles the
selection. The `1`, `2` and `3` keys toggle Ctrl, Alt and Shift directly.
Multiple modifiers may be selected together.

The left soft key (**Done**) arms the selected combination for one phone key.
The strip stays visible until that key is sent. The right soft key is **Cancel**
while choosing and **Clear** when armed outside a menu; both clear the whole
combination without opening scrollback. In a menu it is **Back**, which leaves
the armed combination intact.

Modifiers apply when a multi-tap character is committed, or to the next phone
digit, arrow, Enter, Backspace, Tab or menu-selected key such as F1–F12. Menu
navigation and case changes leave them armed. A pending character waits while
the menu or modifier strip is being used. The input row remains visible above
the strip. An external keyboard uses its own modifiers; typing on it does not
consume an armed phone combination unless it first commits a pending phone
character. Leaving the graphical terminal clears pending input and modifiers.

## Scrollback and keyboards

In scrollback, Up and Down move one line and Left and Right move one page.
The right soft key returns to live output. Typing resumes live input.
An external keyboard uses Shift+PageUp and Shift+PageDown for scrollback.

Keyboards, including the [host keyboard bridge](HOST_KEYBOARD.md), type
alongside the phone keypad through the [shared XKB layouts](../reference/INPUT.md#keyboard-layouts).
Keyboard F13 and F14 remain function keys; they do not invoke phone soft-key
actions. Alt+Shift switches installed keyboard layouts. Phone multi-tap language
is selected separately in the menu.

## Diagnostic console

The menu's **Diagnostic console** action, or Ctrl+Alt+F1 on an external keyboard,
switches to the kernel's diagnostic virtual terminal. Press and release the
phone's right soft key to return to the same graphical shell, with its command
line preserved. The diagnostic console also shows this return shortcut.
From a host [SSH session](SSH.md), `chvt 2` returns to the terminal when it
occupies VT 2, as in the normal startup configuration.

For host shell access and commands, use [SSH sessions](SSH.md).
