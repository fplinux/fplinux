# Camera

Camera capture is supported through standard V4L2 in the default RAM profile
on the Nokia 3210 4G (TA-1618), INOI 240 Modern 4G and INOI 244 Modern 4G.
The images provide the raw capture interface without a dedicated photo,
video-recording or camera-preview application. Camera capture with a microSD
system root has not been qualified.

## Capture interface

The camera appears as a `/dev/videoN` capture device named `ums9117-dcam`.
Its number is not fixed; other video nodes provide image-codec interfaces.
Use `VIDIOC_QUERYCAP` to identify the camera and `VIDIOC_ENUM_FMT` and
`VIDIOC_ENUM_FRAMESIZES` to discover its formats and sizes.

| Phone              | Capture sizes          | Format |
| ------------------ | ---------------------- | ------ |
| Nokia TA-1618      | `800×600`, `1600×1200` | NV16   |
| INOI 240 Modern 4G | `240×320`              | NV16   |
| INOI 244 Modern 4G | `240×320`              | NV16   |

All modes use single-planar, uncompressed YUV 4:2:2 NV16 through MMAP buffers.
A tight frame contains `width × height` Y bytes followed by the same number
of interleaved Cb,Cr bytes. `bytesperline` is the width and `sizeimage` is
`2 × width × height`. Consumers must use the returned color metadata and
monotonic frame timestamps.

Nokia's frames are sideways relative to the phone's portrait orientation;
the consumer handles rotation when needed. INOI frames are upright. Saved
image dimensions, compression and microphone recording belong to the
consumer. The [microphone](MICROPHONE_AUDIO.md) is a separate ALSA interface.

## Resource limits

Stopping streaming releases the image block. Sensor power and clock are
disabled through runtime PM; autosuspend can delay this after the stream
has stopped. MMAP buffers remain
allocated until the consumer unmaps and frees them with `VIDIOC_REQBUFS`
(count `0`), or closes the capture device. A second capture cannot change
the format while another process owns a streaming queue.

The camera and [hardware JPEG codec](../apps/JPEG.md) share an image block.
Hardware JPEG buffer allocation or streaming returns `EBUSY` while the
camera owns it; JPEG works again after capture stops.

On Nokia, do not run ROTA while camera capture is active: a concurrent
rotation can cause a camera frame error. Complete capture before rotating.
