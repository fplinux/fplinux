# Software video decoding

`fplinux-ffmpeg.apk` supplies the `ffmpeg` command as an optional package. It
decodes MPEG-4 Part 2, H.263 and H.264 using the CPU, including ARM NEON routines.
It does not use the phone's video accelerator.

Install and remove the package with the shared
[APK workflow](../guides/APK_PACKAGES.md). The installed package name is
`fplinux-ffmpeg`. It is not preinstalled in the RAM image.

## Decode a file

Supported input containers are MP4/3GP, AVI and Matroska. Elementary MPEG-4,
H.263 and H.264 streams are also accepted. The video codec must be one of the
three decoders above.

Decode a supported file without retaining its frames:

```sh
ffmpeg -threads 1 -i /path/to/clip.mp4 -an -f null -
```

Export the first decoded frame:

```sh
ffmpeg -threads 1 -i /path/to/clip.mp4 -an -frames:v 1 -f rawvideo /run/frame.yuv
```

Raw output has no dimensions, timestamps or pixel-format header. The command's
output-stream description identifies its dimensions and native pixel format.
For an 8-bit `yuv420p` frame, the file contains the Y plane followed by U and V;
each chroma plane has half the width and height of the luma plane, rounded up
for odd dimensions. A 320×240
frame occupies 115200 bytes. This format is different from the
[JPEG command's NV16 output](JPEG.md#decoder-output-format).

## Limits

The package supplies file and pipe input/output. It has no network protocols,
camera input, audio decoding, audio recording or playback window. Another
process can provide a stream through a pipe; `ffmpeg` itself does not establish
that connection.

Video encoding, resizing, rotation and pixel-format conversion are not included.
Do not select an output pixel format that would require conversion. The package
does not replace the [hardware JPEG tools](JPEG.md) or [rotation tool](ROTATE.md).

Keep raw output bounded when writing into the RAM filesystem. A complete raw
video can consume much more memory than its compressed input. Send frames to a
consumer through standard output or retain only the frames needed by the task.
