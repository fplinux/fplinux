# Native image presentation

`fplinux-present` accepts NV16 frames at the native size of the phone's
display. It can send NV16 directly to the LCD controller or convert the same
input on the CPU and send little-endian RGB565. It is included in the normal
root filesystem and does not run as a service. See the
[target documentation](../../targets/README.md) for its support and
visible-result limits on the selected phone.

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
uninterrupted presentation, final hold, and successful DRM and virtual-terminal
restore. The direct NV16 mode does not write a converted output.

Scalar value options may be repeated. Every occurrence must be valid, and the
last one selects the effective value.

The complete options are:

- `--input NV16`: the NV16 input frame; required;
- `--mode nv16|cpu-rgb565`: presentation path; default `nv16`;
- `--repeat N`: completed presentations from 1 through 4096; default 1;
- `--fps N`: absolute-deadline pacing from 1 through 30 frames per second;
  default 10;
- `--hold-ms N`: keep the final frame for 0 through 60000 milliseconds after
  the repeated presentations; default 0;
- `--drm PATH`: DRM device; default `/dev/dri/card0`;
- `--tty PATH`: console TTY; default `/dev/tty0`;
- `--output FILE`: optional CPU-converted RGB565 output;
- `--help`: print the command synopsis.

The first presentation begins without a pacing delay. Later iterations use
deadlines measured from the beginning of the series rather than sleeping for a
fixed interval after each frame. `--fps` therefore sets requested pacing; it is
not a measured refresh rate or a guarantee that an overloaded series meets
every deadline. The same input frame is used for every iteration.

## Input and output formats

The input must be one regular file matching the active DRM mode's native
geometry. It is headerless, tight, full-range BT.601 NV16:

1. a Y plane with one byte per pixel;
2. an equally sized interleaved Cb,Cr plane with the same row count and stride.

Each plane holds width × height bytes, so the input file and the optional
RGB565 output are both twice that size. For example, a `240×320` display takes
`76800`-byte planes and a `153600`-byte file.

Each Cb,Cr pair applies to two adjacent horizontal luma samples. There are no
dimensions, strides, metadata, or row padding in the file.

In `nv16` mode the LCD controller performs native YCbCr-to-RGB conversion. In
`cpu-rgb565` mode the application uses its fixed full-range BT.601 conversion,
clamps each channel, and packs the high bits as little-endian RGB565. The
optional RGB565 output has two bytes per pixel, no row padding, and no header.
The command reads the geometry from the DRM mode; it has no size override.

The command does not resize, crop, rotate, decode, or capture an image. Prepare
the exact native geometry first with the appropriate image tool.

## Display session and completion boundary

The command uses atomic DRM/KMS with an RGB565 or NV16 plane at the native
panel size. It acquires a graphics virtual terminal and DRM master, and returns
to the previous virtual terminal on normal exit or handled `SIGINT` or
`SIGTERM`. VT switching suspends presentation until the session becomes active
again. A failed session restore makes an otherwise completed command fail.

A completed atomic commit is a kernel display result, not an optical
measurement. The native path has no pixel readback, and pixel-for-pixel
equivalence between LCDC conversion, CPU conversion and visible output is not
guaranteed. See the selected target's support status before relying on a
physical display result.

## Reported measurements

After the presentation series, the command prints its completed frame count
and these timing stages:

- `codec_convert`: CPU NV16-to-RGB565 conversion; zero calls in direct NV16 mode;
- `atomic_commit`: submission and completion of the DRM atomic presentation;
- `whole_loop`: the repeated series, including pacing and conversions.

The final hold, input read, display-session setup or restore and optional output
write are outside `whole_loop`. Wall and process user and system time are
reported in nanoseconds, alongside peak resident memory and page-fault counters.
These measurements do not establish panel latency or refresh rate.
