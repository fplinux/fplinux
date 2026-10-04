# Display

The phone exposes a fixed native-size DRM/KMS display. The selected
[target document](../../targets/README.md) states its panel geometry, supported
formats and hardware limitations. [Display brightness](DISPLAY_BACKLIGHT.md)
has a separate control interface.

## Framebuffers and damage

RGB565 framebuffers contain the complete native image with two bytes per pixel
and a tight native-width row stride. Atomic clients can provide
`FB_DAMAGE_CLIPS` in framebuffer coordinates. The driver merges the visible
clips into one bounding rectangle and expands its horizontal edges outward to
even pixel boundaries for the controller's four-byte source alignment.
Pixels included by that expansion come from the complete framebuffer.

Each transfer sets the panel's address window. A commit without damage clips,
the first frame after display enable and the first frame after wake transfer
the complete image. A commit whose supplied clips do not intersect the visible
image completes without a pixel transfer. Changing framebuffer objects does
not remove the client's obligation to supply a complete image and damage
relative to the preceding displayed frame.

Where native NV16 presentation is available, its updates transfer the complete
image. The KMS plane requires native geometry; scaling and rotation are not
supported.

## Completion

The page-flip event and output fence complete after the controller finishes the
transfer, or after the bounded error path stops it. A failed transfer reports
an error through the output fence and leaves the backlight off. Completion
allows buffer ownership to advance; it does not report the panel's scan phase
or guarantee an optical presentation timestamp. The driver does not expose a
periodic hardware vblank counter.
