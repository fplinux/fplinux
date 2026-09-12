# Image rotation

`fplinux-rotate` rotates raw images using the CPU or the UMS9117 ROTA
accelerator. It is included in the shared UMS9117 root filesystem for the
`default` and `microsd-uboot` profiles and does not run as a service. See the
[target documentation](../../targets/README.md) for hardware support on the
selected phone.

First load the image using the shared [loading guide](../guides/LOADING.md).
For a standalone archive, start with its top-level `README.txt` and the shared
[archive guide](../guides/STANDALONE.md). Use the shared [SSH](../features/SSH.md)
and [file transfer](../features/FILE_TRANSFER.md) instructions. Commands below run
on the phone. Keep input and output files in RAM; `--output` accepts paths
under `/run` or `/tmp` only.

## Run

For a 240x320 display, rotate a 320x240 RGB565 image into a 240x320 result:

```sh
fplinux-rotate --engine rota --format rgb565 --width 320 --height 240 \
  --rotate 90 --input /run/input.raw --output /run/rotated.raw \
  --verify --display
```

Select `--engine cpu` explicitly to use the software path. The ROTA path does
not fall back to CPU rotation when a format, geometry or operation is rejected.
Without `--input`, the application generates a deterministic image. Without
`--output`, it writes `/run/fplinux-rotate.raw`.

`--verify` compares the result with the CPU reference. `--display` converts the
result to RGB565 and presents it through the existing framebuffer. The result
must match the active framebuffer dimensions: preview does not scale the image
or change console orientation. For a 128x160 display, use a 160x128 input with
`--rotate 90`. `--display-ms N` sets the preview hold time; the default is 2000 ms.
The console is restored when the preview ends.

## Formats and raw files

| CLI format | V4L2 fourcc | Raw layout                                               |
| ---------- | ----------- | -------------------------------------------------------- |
| `rgb565`   | `RGBP`      | Little-endian RGB565, two bytes per pixel                |
| `xrgb32`   | `BX24`      | Four bytes per pixel in X, R, G, B order; X is preserved |
| `grey`     | `GREY`      | One unsigned luma byte per pixel                         |
| `nv12`     | `NM12`      | Y plane followed by interleaved U,V; 4:2:0               |
| `nv16`     | `NM16`      | Y plane followed by interleaved U,V; 4:2:2               |

`xrgb32` is not packed three-byte RGB888. YUV files contain the entire strided
Y plane followed by the entire strided UV plane, without a header. NV12 has
half as many UV rows as Y rows; NV16 has one UV row per Y row.

`--stride N` specifies input row bytes, including padding; the default is a
packed row. For YUV, this option applies the same stride to both planes. Output
files contain packed rows and, for YUV, Y followed by UV. `--crop X,Y,W,H`
selects the source rectangle before rotation.

NV12 rotates its luma and chroma grids. NV16 quarter turns preserve horizontal
4:2:2 packing by selecting source chroma rows at crop-relative offsets 0, 2,
4, and so on, and duplicating chroma vertically. This loses chroma
detail; it is not equivalent to rotating a full-resolution colour image and
resampling it with a general-purpose image converter.

## Hardware API and limits

ROTA exposes V4L2 mem2mem OUTPUT/CAPTURE multiplanar queues, with MMAP and
DMA-BUF buffers. The application discovers the matching device automatically;
`--device PATH` selects it explicitly. Other V4L2 consumers may use independent
source-plane strides, each a multiple of four bytes. Capture rows are packed.

Supported controls are rotation by 90, 180 or 270 degrees, or horizontal
mirroring with rotation set to zero. Set rotation and HFLIP together when
switching between these modes. Vertical flipping and rotation-plus-flip
combinations are rejected by both CLI engines.

Each selected source plane's first byte must be four-byte aligned, including
its crop offset, and every packed capture row must contain a multiple of four
bytes. This does not require both source dimensions to be even for RGB or
grayscale. NV12 requires even source/crop widths and heights and even crop left
and top offsets. NV16 requires even source/crop widths and an even crop left
offset. Its source height and crop top may be odd; an odd crop height is allowed
when the selected operation leaves a valid packed output row. Unsupported final
geometry is rejected with `EINVAL` at STREAMON before a plane starts;
intermediate format/crop/control negotiation is allowed.

Each dimension is limited to 4092 pixels. This is not a guarantee that a
4092x4092 image can be allocated: the phone has 64 MiB RAM, and source,
destination, verification and device buffers all consume memory.

Queues support repeated jobs, multiple contexts, STREAMOFF and closing with
unfinished work. Failures are bounded. If DMA cannot be stopped safely, its
buffers are retained and the device refuses further work until a cold boot;
closing and reopening the video node is not recovery.

## Timing

`--iterations N` repeats the selected engine on the same input.
`--benchmark N` runs four same-input batches in CPU/ROTA/ROTA/CPU order, with
N iterations per batch:

```sh
fplinux-rotate --engine rota --format rgb565 --width 320 --height 240 \
  --rotate 90 --input /run/input.raw --benchmark 20 --display
```

Reported microsecond values are batch totals. Stages distinguish device setup,
input copying, queue submission, waiting, output copying, teardown, CPU rotation
and preview conversion/presentation. `total_us` measures the application engine
call, including an optional preview, but excludes the separately reported
guard-check time; input-file reading and final output-file writing are outside
that call. Preview conversion is included within framebuffer time, so those
two fields must not be added together. Benchmark mode suppresses preview holds.

`process_user_us` and `process_system_us` are process CPU time, not total system
CPU load. The framebuffer path includes conversion and page publication; its
timings are not a display-completion fence or an optical latency/FPS measurement.
