# Shared pre-Linux components

`bootstrap/` contains freestanding, platform-neutral code that phone targets may
compile into their pre-Linux payloads. It is separate from the post-kernel code
under [`common/`](../common/README.md) and has no Linux userspace dependency.

## Boot screen

`fplinux-boot-screen/` provides the shared FPLinux boot-screen font, rasterizer,
portrait-aware layout, progress states, handoff status and bounded error view.
It uses fixed-size state, performs no allocation and draws only through canvas
callbacks supplied by the caller.

A target remains responsible for:

- initializing its panel and framebuffer;
- implementing bounded rectangle and presentation callbacks;
- supplying model, variant, boot-mode, stage and checkpoint text;
- sequencing hardware operations, diagnostics and the kernel handoff.

The shared renderer must not contain phone addresses, SoC registers, panel setup
or loader policy. A target stages the library into its pinned bootstrap build
closure.
