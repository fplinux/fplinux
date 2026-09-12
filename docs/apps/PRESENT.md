# Native image presentation

`fplinux-present` accepts native-size NV16 frames for Nokia TA-1618, INOI 240
Modern 4G, and INOI 244 Modern 4G. It can send NV16 directly to the LCD
controller or convert the same input on the CPU and send little-endian RGB565.
It is included in the normal root filesystem and does not run as a service.
Both presentation modes complete LCD-controller transfers on all three
targets. Visible image fidelity remains a separate, target-specific limit;
see the [target documentation](../../targets/README.md).

First load the normal image using the target's instructions. A standalone
archive includes this page; start with its top-level `README.txt` and the shared
[archive guide](../guides/STANDALONE.md). Use the shared
[SSH](../features/SSH.md) and [file transfer](../features/FILE_TRANSFER.md)
instructions. Commands below run on the phone. Prefer `/run` or `/tmp` for input
and output unless the target documentation explicitly supports a mounted
writable filesystem.

## Run

Present NV16 through the LCD controller's native YCbCr path and hold the final
frame for two seconds:

```sh
fplinux-present --input /run/frame.nv16 --mode nv16 --hold-ms 2000
```

`--mode nv16` is the default. To perform full-range BT.601 conversion on the
CPU, present RGB565, and retain the converted frame:

```sh
fplinux-present --input /run/frame.nv16 --mode cpu-rgb565 \
  --output /run/frame.rgb565 --hold-ms 2000
```

`--output` is accepted only with `--mode cpu-rgb565`. It is written after an
uninterrupted presentation, final hold, and successful framebuffer and console
restore. The direct NV16 mode does not write a converted output.

The complete options are:

- `--repeat N`: completed presentations from 1 through 4096; default 1;
- `--fps N`: absolute-deadline pacing from 1 through 30 frames per second;
  default 10;
- `--hold-ms N`: keep the final frame for 0 through 60000 milliseconds after
  the repeated presentations; default 0;
- `--framebuffer PATH`: framebuffer device; default `/dev/fb0`;
- `--tty PATH`: console TTY; default `/dev/tty0`;
- `--output FILE`: optional CPU-converted RGB565 output;
- `--help`: print the command synopsis.

The first presentation begins without a pacing delay. Later iterations use
deadlines measured from the beginning of the series rather than sleeping for a
fixed interval after each frame. `--fps` therefore sets requested pacing; it is
not a measured refresh rate or a guarantee that an overloaded series meets
every deadline. The same input frame is used for every iteration.

## Input and output formats

The input must be one regular file matching the active framebuffer's native
geometry. It is headerless, tight, full-range BT.601 NV16:

1. a Y plane with one byte per pixel;
2. an equally sized interleaved Cb,Cr plane with the same row count and stride.

| Target                            | Geometry | Bytes per plane | Input or RGB565 output bytes |
| --------------------------------- | -------- | --------------- | ---------------------------- |
| Nokia TA-1618, INOI 244 Modern 4G | 240×320  | 76800           | 153600                       |
| INOI 240 Modern 4G                | 128×160  | 20480           | 40960                        |

Each Cb,Cr pair applies to two adjacent horizontal luma samples. There are no
dimensions, strides, metadata, or row padding in the file.

In `nv16` mode the LCD controller performs native YCbCr-to-RGB conversion. In
`cpu-rgb565` mode the application uses its fixed full-range BT.601 conversion,
clamps each channel, and packs the high bits as little-endian RGB565. The
optional RGB565 output has two bytes per pixel, no row padding, and no header.
The command reads the geometry from the framebuffer; it has no size override.

The command does not resize, crop, rotate, decode, or capture an image. Prepare
the exact native geometry first with the appropriate image tool.

## Console and completion boundary

The command requires the target's native RGB565 framebuffer without row padding
and an active console in text mode. It saves all framebuffer
pages, the framebuffer mode and selected page, the console mode, and the active
virtual terminal before switching to graphics mode. Normal exit and handled
`SIGINT` or `SIGTERM` restore that state. Restore failure makes an otherwise
completed command fail.

Each presentation blocks until the driver reports LCDC DONE and returns a
strictly increasing sequence number and a nonzero LCDC transfer duration. A
successful call confirms that the controller completed the transfer and that
the guarded staging buffer remained intact. It is not an optical measurement
or independent confirmation of what was visible on the panel.

The native path has no pixel readback. Pixel-for-pixel equivalence between
LCDC conversion, CPU conversion, and the visible panel output is not guaranteed.

## Reported measurements

The command always prints a result line, stage timings, LCDC transfer totals,
and the first and last completion sequences. The timing stages are:

- `codec_convert`: CPU NV16-to-RGB565 conversion; it has zero calls in direct
  NV16 mode;
- `present_ioctl`: the blocking presentation ioctl, including the userspace
  copy and wait for LCDC DONE;
- `whole_loop`: the complete repeated series, including pacing, conversions,
  and presentation ioctls.

The final hold, input read, framebuffer and console setup or restore, and
optional RGB565 output write are outside `whole_loop`. Wall and process user and
system time are reported in nanoseconds together with process peak resident
memory and page-fault counters. The LCDC transfer values are controller
completion durations, not optical latency.

## Framebuffer ioctl

The shared `ums9117-present.h` header defines `UMS9117_FBIO_PRESENT` and
`struct ums9117_present` for native-size userspace presentation. In a source
checkout it is maintained at `platforms/ums9117/common/ums9117-present.h`.
`pixels` points to one native-size frame, `format` is
`UMS9117_PRESENT_NV16` or `UMS9117_PRESENT_RGB565`, and `bytes` must equal the
framebuffer width multiplied by its height and by two.
On success the driver returns the nonzero completion `sequence` and
`transfer_ns` measured from LCDC start through LCDC DONE.

The ioctl is enabled for the three target framebuffers listed above. Callers
using the ioctl directly must manage their own console and framebuffer session.
The `fplinux-present` command is the supported owner of backup, graphics-mode
switching, pacing, interruption, and restoration for ordinary use.
