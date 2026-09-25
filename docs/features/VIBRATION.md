# Vibration

The vibrator is exposed as a Linux input force-feedback device with physical
path `fplinux/vibrator0`. The input name states the implementation:
`sc27xx:vibrator` for a vibration motor and `UMS9117 speaker vibrator` for a
phone whose speaker also vibrates it. The selected
[target's documentation](../../targets/README.md) states its physical support.
A standalone archive carries that status in `README.txt`. Applications should
locate the device by its physical path rather than assuming a fixed
`/dev/input/eventN` number or name. An accepted effect does not by itself
establish that the phone vibrates.

## Interface

The device accepts standard `FF_RUMBLE` effects through the Linux input API.
Both nonzero magnitude fields request the same binary output; vibration
strength is not adjustable.

Finite effects run for their requested duration. Every activation also has an
automatic driver cutoff after about five seconds, so zero-length and longer
requests cannot leave the output enabled indefinitely. Timing is not a precise
duration API. A new activation is accepted only after the previous output has
been confirmed off.

Closing the last input handle, removing the driver, shutting Linux down or
entering [s2idle](SUSPEND.md) stops an active pulse and restores the audio or
motor state it changed. A pulse interrupted by s2idle is not resumed after
wake.

Repeated effects may form a pulse train. The driver does not define a
duty-cycle or thermal policy, so applications should leave an off interval
between activations.

## Speaker vibration

A phone without a separate motor vibrates through its speaker, as its stock
firmware does. The speaker plays the phone's fitted vibrate tone, a low tone
near the speaker's resonance, with the level, fade and duration limit prepared
from that phone's NAND backup with the other fitted audio data. Without that
prepared audio profile the device is still present, but it does not vibrate.

A single continuous pulse stops after about three seconds because of the fitted
duration limit, before the driver cutoff; a new activation starts it again.

Vibration shares the audio output:

- Speaker playback keeps playing together with the vibration.
- Headphone and FM audio is muted while the phone vibrates and for about one
  second after the last pulse, then returns. Playback streams keep running
  without errors, and the sound of that interval is lost. The mixer controls
  do not change.
- With the speaker muted, the speaker plays only the vibration.
- Microphone recording captures the vibration tone.
