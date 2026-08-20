# Porting FPLinux

A phone port consists of the smallest complete source closure required to build,
run and document the target. Keep temporary artifacts and unused implementation
paths out of target and platform directories.

## Layers

```text
bootstrap/<component>/ reusable platform-neutral freestanding pre-Linux code
common/               reusable post-kernel userspace and host tools
platforms/<soc>/      reusable SoC support
targets/<phone>/      concrete phone support
```

Top-level bootstrap components own generic freestanding behavior and callback
contracts only. A target composes them with its framebuffer and refresh adapter,
device identity, stage and checkpoint text, error policy and hardware sequence.
SoC registers and board-specific bootstrap actions stay in the platform or target
layer.

A platform owns CPU integration, interrupt controllers, clocks, timers,
reusable controller drivers and its fixed host-loader sequence. A target owns
the board DTS, memory map, panel, keypad wiring, bootstrap, root filesystem
profile, board assets and validated adapter values. The shared console consumes
standard `fbcon`, evdev and gadget-tty interfaces; it does not own phone register
or scan-code knowledge.

Start with:

- [Phone target template](TARGET.md)
- [Platform template](PLATFORM.md)
- [Shared console contract](CONSOLE.md)

## Verification requirements

1. Build the target from a source-only checkout.
2. Create a `--candidate` package.
3. Run that exact runtime closure on the intended hardware variant.
4. Verify every feature marked **Supported**.
5. Record the printed runtime SHA-256 in `releases.lock.toml` only for a runtime
   closure that passes the complete phone gate.
6. Create the release package without `--candidate`.

A target support table records feature-level hardware validation and
limitations. Release qualification is stricter: it applies to the complete exact
runtime closure and exists only when the matching digest is present in
`releases.lock.toml`.

Drivers required by a phone profile are built into its kernel (`=y`). Optional
modules are not part of that profile unless the target documentation and package
contract explicitly include them.
