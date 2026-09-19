# JPEG codec and scaling

`fplinux-jpeg` uses the UMS9117 image accelerator to decode
supported JPEG files, encode fixed-size NV16 frames, or half-scale either of
two fixed NV16 frame sizes. `fplinux-jpeg-cpu` is a separate software reference
tool. The hardware command never falls back to the CPU tool, and the CPU tool
never uses the accelerator.

Both commands are included in the shared UMS9117 root filesystem for the
`default` and `microsd-uboot` profiles and do not run as services. See the
[target documentation](../../targets/README.md) for hardware support on the
selected phone. Load its image using the shared [loading guide](../guides/LOADING.md).
A standalone archive includes this page; start with its top-level `README.txt`
and the shared [archive guide](../guides/STANDALONE.md). Use the shared
[SSH](../features/SSH.md) and [file transfer](../features/FILE_TRANSFER.md)
instructions. Commands below run on the phone. Prefer `/run` or `/tmp` for input
and output unless the target documentation explicitly supports a mounted
writable filesystem.

Both commands allow scalar value options to be repeated. Every occurrence must
be valid, and the last one selects the effective value.

## Hardware command

The complete command synopsis is:

```text
fplinux-jpeg --input FILE --output FILE [--operation decode|encode|scale] [--device /dev/videoN] [--repeat N] [--timing] [--scale 1|4] [--width N --height N]
```

`--operation decode` is the default. The valid geometry and scale options
depend on the selected operation as described below. The application discovers
the corresponding `ums9117-jpeg` V4L2 device automatically. `--device
/dev/videoN` selects a particular node and still requires the matching decoder,
encoder, or scaler device.

`--repeat N` submits the same input from 1 through 4096 times, verifies that
every result is identical, and writes the final result; the default is one. A
job or source-change wait is limited to ten seconds. Parsing, device, timeout,
I/O, or inconsistent-repeat failures make the command exit nonzero.

## Decode JPEG

Decode at the JPEG's full dimensions:

```sh
fplinux-jpeg --input /run/input.jpg --output /run/output.raw
```

`--input` must name a non-empty regular file no larger than 1 MiB. On success
the command prints the capture format, visible width and height, driver strides
and plane payload sizes, and repeat count. The printed strides and payload
sizes describe the driver's padded two-plane buffers; the output file itself is
tightly packed as described below.

Use the accelerator's quarter-resolution decode for a compatible JPEG:

```sh
fplinux-jpeg --input /run/input.jpg --output /run/quarter.nv16 --scale 4
```

`--scale 4` accepts only the horizontal 4:2:2 syntax described on this page.
The encoded width and height must already equal their complete MCU geometry:
the width is a multiple of 16 and the height is a multiple of 8, with no padded
right or bottom edge. The output is tight NV16 at exactly `W / 4` by `H / 4`.
This is reduced-resolution JPEG decoding; it is separate from the fixed
half-size raw scaler operation. `--scale 1` is the default and retains the full
visible dimensions.

### Decoder output format

At full size, the output is a headerless, full-range YCbCr 4:2:0 or 4:2:2
image. It does not contain JPEG metadata, dimensions, or strides.

| Input sampling   | Reported V4L2 format | Output file                                                 |
| ---------------- | -------------------- | ----------------------------------------------------------- |
| 4:2:0, H2V2 luma | `NV12M`              | `W × H` Y bytes, then interleaved Cb,Cr rows at half height |
| Horizontal 4:2:2 | `NV16M`              | `W × H` Y bytes, then interleaved Cb,Cr rows at full height |

Rows in the file have no padding. The Y row is `W` bytes. The Cb,Cr row is
`2 × ceil(W / 2)` bytes, so an odd width produces `W + 1` UV bytes and the final
Cb,Cr pair represents the final unpaired luma column. NV12 contains
`ceil(H / 2)` chroma rows; NV16 contains `H` chroma rows. The file therefore
consists of the complete visible Y plane followed by the complete visible UV
plane, even though the V4L2 API exposes them as two separate planes.

The driver identifies the output as JPEG-colorspace, full-range BT.601 YCbCr.
The application does not apply EXIF orientation, colour conversion, or display
presentation.

Pixel-exact equality with software JPEG decoders is not part of the API.
Different IDCT rounding can change low sample bits; no universal per-sample
error bound is specified for every accepted JPEG.

### Supported JPEG syntax

The decoder accepts self-contained, eight-bit baseline Huffman YCbCr with one
interleaved scan. Width and height may each be from 1 through 2048 pixels. The
complete JPEG file must not exceed 1 MiB.

The current input contract is:

- exactly one baseline SOF0 frame and one SOS scan, with three components;
- luma sampling H2V2 for 4:2:0 or H2V1 for horizontal 4:2:2; both chroma
  components must be H1V1;
- consecutive component IDs in frame and scan order. The first ID may be 0, 1
  or 2, giving `0,1,2`, `1,2,3` or `2,3,4`;
- scan parameters `Ss=0`, `Se=63` and `Ah=Al=0`;
- APP14 absent or declaring YCbCr transform 1;
- every referenced quantization table explicitly present as 64 nonzero
  eight-bit values. Table IDs 0 through 3 are accepted, and the Cb and Cr tables
  must contain identical values;
- every referenced DC and AC Huffman table explicitly present, canonical and
  valid for baseline symbols. DC tables contain 1 through 12 unique categories
  from 0 through 11, with no more than four symbols of length 16. AC tables
  contain 1 through 162 unique symbols, use coefficient sizes through 10, and
  reserve size zero for EOB and ZRL. The canonical tree must leave the all-ones
  padding code unused. DC and AC table IDs may each be 0 or 1; Cb and Cr must
  use identical DC tables and identical AC tables;
- an optional restart interval with RST markers in `RST0` through `RST7` order
  and the exact count implied by the image's MCU count;
- byte-stuffed entropy followed immediately by the final EOI marker. An
  entropy `0xff` data byte must use exactly `ff 00`; no other scan, marker, or
  trailing data is accepted after SOS except the declared restart markers and
  final EOI.

Normal and optimized Huffman tables, custom quantization tables, and restart
markers are supported when they satisfy those rules. Quantization and Huffman
table IDs are selectors, not fixed luma/chroma roles, provided every definition
and reference is consistent.

Progressive and extended sequential JPEG, grayscale, 4:4:4, 4:1:1, vertical
4:2:2, RGB/CMYK/YCCK transforms, arithmetic coding, multiple scans, sixteen-bit
quantization tables, and JPEGs that rely on omitted default tables are not
supported.

Header validation is deliberately strict, but it is not a complete software
decode of the entropy stream. Corrupt coefficient data can pass the header
stage and then be reported as a bounded decode error. Do not assume every
malformed or unsupported JPEG is rejected at the same stage or with the same
error number.

## Encode NV16 as JPEG

Encode a supported tight NV16 frame:

```sh
fplinux-jpeg --operation encode --width 320 --height 240 \
  --input /run/input.nv16 --output /run/output.jpg
```

The supported input geometries and exact file sizes are:

| Width × height | Input bytes |
| -------------- | ----------: |
| `1200×32`      |      76,800 |
| `320×240`      |     153,600 |
| `640×480`      |     614,400 |

The input is headerless, tight, full-range BT.601 NV16: first `W × H` Y bytes,
then `W × H` interleaved Cb,Cr bytes. Each chroma pair belongs to two adjacent
horizontal luma samples. Other geometries and camera capture are outside this
command's supported interface.

The output is a self-contained, baseline Huffman YCbCr 4:2:2 JPEG using fixed
quality-85 quantization and restart markers. The command reserves at most 1 MiB
for the encoded JPEG and fails instead of writing a larger result. Quality,
tables, sampling, and restart settings are not configurable on the hardware
command.

## Half-scale NV16

The raw scaler supports exactly two source-to-destination pairs:

| Source    | Destination | Input bytes | Output bytes |
| --------- | ----------- | ----------: | -----------: |
| `640×480` | `320×240`   |     614,400 |      153,600 |
| `320×240` | `160×120`   |     153,600 |       38,400 |

Select the pair with the source dimensions:

```sh
fplinux-jpeg --operation scale --width 640 --height 480 \
  --input /run/input.nv16 --output /run/half.nv16
```

Input and output are headerless, tight, full-range BT.601 NV16 with the Y plane
followed by the full-height interleaved Cb,Cr plane. The scale operation always
uses the accelerator's fixed factor-two filter. Omit `--scale`; `--scale 2` is
not hardware-command syntax. Arbitrary dimensions, ratios, cropping, and format
conversion are not supported by this operation.

## CPU reference tool

`fplinux-jpeg-cpu` is selected by invoking it explicitly. It has no `--device`
option and is not an automatic recovery or fallback path for `fplinux-jpeg`.
Its command forms are:

```text
fplinux-jpeg-cpu --operation encode --input NV16 --output JPEG --width W --height H [--quality 1..100] [--restart 0..65535] [--dqt DQT128] [--repeat 1..4096]
fplinux-jpeg-cpu --operation decode --input JPEG --output NV12_OR_NV16 [--scale 1|2|4] [--decode-mode reduced|box] [--repeat 1..4096]
fplinux-jpeg-cpu --operation scale --input NV16 --output NV16 --width 640 --height 480 [--scale 2] [--repeat 1..4096]
fplinux-jpeg-cpu --operation scale --input NV16 --output NV16 --width 320 --height 240 [--scale 2] [--repeat 1..4096]
```

CPU encode uses horizontal 4:2:2 sampling. Its default quality is 75 and its
default restart interval is zero; `--dqt` accepts exactly 128 nonzero bytes as
explicit luma and chroma quantization tables. CPU decode uses the bundled JPEG
library. `--decode-mode reduced`, the default, requests its reduced IDCT for
scales 2 and 4; `--decode-mode box` performs a full IDCT followed by an explicit
box reduction. These software inputs do not broaden the hardware decoder's
supported JPEG syntax.

CPU `--operation scale` uses the same two fixed geometries as the hardware
scaler and an explicit factor-two box filter with nearest-up rounding. It is a
separate algorithm, so its pixels are not a specification for every hardware
filter output.

## Timing output

Add `--timing` to `fplinux-jpeg` to print nanosecond wall-clock components,
total process user and system CPU time in microseconds, peak resident memory,
and page-fault counts. The wall fields have these boundaries:

- `wall_input_ns` reads the input file;
- `wall_prepare_ns` opens and negotiates the device, prepares buffers and
  queues, and allocates the result buffer;
- `wall_loops_ns` covers all job-loop iterations, while
  `wall_warm_loops_ns` excludes the first iteration;
- `wall_queue_ns` measures queue calls and `wall_wait_ns` measures completed-job
  waits; their `wall_warm_*` forms exclude the first iteration;
- `wall_output_ns` writes the result file;
- `wall_total_ns` spans input reading through the completed output write,
  including the stream-off, unmapping, and device close before that write.

Queue and wait measurements are components inside the loop measurements, so
they must not be added to `wall_loops_ns`. The warm fields are meaningful only
when `--repeat` is greater than one. `user_total_us` and `sys_total_us` are this
process's CPU time, not total system CPU load.

The CPU tool always prints wall, user, system, and combined process CPU
milliseconds for its input load, codec or filter work, batch, output write, and
`measured_stages`. Operation-specific codec/conversion values are components of
the batch and must not be added to it.

## V4L2 API

The `ums9117-jpeg` driver exposes separate V4L2 mem2mem multiplanar streaming
nodes for decoding, encoding, and fixed raw scaling. The supplied application
discovers the node with the matching card identity.

The decoder contract is:

- OUTPUT is one MMAP plane with compressed `JPEG` data and dynamic resolution;
- the OUTPUT allocation is negotiated from 4096 bytes through 1 MiB; its
  compressed payload is non-empty and at most 1 MiB;
- CAPTURE is two MMAP planes, `NV12M` for 4:2:0 or `NV16M` for horizontal
  4:2:2;
- a source-change event publishes the parsed dimensions and capture format;
- capture stride is the width padded to 16 pixels. Allocated height is padded to
  16 rows for NV12M and 8 rows for NV16M, while the selection rectangle reports
  the original visible dimensions;
- completed buffers carry the padded plane payload sizes. Consumers that need
  a tight file must crop each row to the visible selection, including the
  rounded-up chroma sample for an odd width;
- quarter decode is selected through the CAPTURE compose rectangle and is
  available only for MCU-aligned horizontal 4:2:2 input.

The encoder accepts two-plane `NV16M` OUTPUT and produces one-plane `JPEG`
CAPTURE for the three listed geometries. The scaler accepts two-plane `NV16M`
OUTPUT and produces two-plane `NV16M` CAPTURE for the two listed fixed pairs.
Both raw interfaces use full-range BT.601 colour metadata and tight plane
strides.

The supplied application performs negotiation, raw-plane packing, and decoder
cropping. If recovery cannot confirm that the accelerator is idle, the driver
refuses further jobs; reopening the video node is not a recovery guarantee.
