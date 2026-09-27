# Console port contract

FPLinux's local terminal is shared userspace packaged through the
[Alpine layer](../../alpine/README.md). A phone port supplies standard Linux
display and input interfaces; the terminal has no panel-register or scan-code
knowledge. User controls belong to the [local console guide](../features/LOCAL_CONSOLE.md).

## Required interfaces

The graphical terminal needs:

- a DRM/KMS card with an atomic RGB565 scanout plane and dumb buffers;
- Linux virtual terminals, including `VT_PROCESS` switching;
- `/dev/ptmx` and `devpts` for the interactive Bash shell;
- evdev input following the [input contract](../reference/INPUT.md).

The OpenRC service starts only when `/dev/dri/card0` exists. It runs a separate
terminal process and shell, using `TERM=xterm-256color`. Keyboard text uses
libxkbcommon; terminal escape sequences and scrollback use libtsm.

The display session acquires its own virtual terminal and DRM master. On a VT
switch it releases display and input ownership, then reacquires them on return.
Graphical applications use the same session contract, so the terminal can resume
after an application exits. Kernel `fbcon` remains available for diagnostics;
it is not the graphical terminal renderer.

## Target responsibilities

A target provides, itself or through its platform:

- its DRM driver, panel geometry and board-specific display sequencing;
- keypad scan, wiring and the standard Linux phone-key mapping;
- built-in configuration and device nodes for DRM, virtual terminals, evdev and PTYs;
- when supported, the host-keyboard bridge's uinput and generic-serial dependencies.

Applications identify the phone keypad by `phys=fplinux/keypad0`, not by its
event number or by a key code shared with external keyboards. Input ownership
must follow the active graphical session.

The target's support document states which interfaces have been exercised on
physical hardware. See the [target template](TARGET.md) and the
[porting overview](README.md) for the complete layer boundary.
