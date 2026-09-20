# Vibration

The vibrator is exposed as a Linux input force-feedback device with physical
path `fplinux/vibrator0` and input name `sc27xx:vibrator`. The selected
[target's documentation](../../targets/README.md) states its physical support.
A standalone archive carries that status in `README.txt`. Applications should
locate the device by its name and physical path rather than assuming a fixed
`/dev/input/eventN` number. An accepted effect does not by itself establish
that the motor vibrates on the selected phone.

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
entering [s2idle](SUSPEND.md) stops an active pulse and restores the inherited
SC2720 state. A pulse interrupted by s2idle is not resumed after wake.

Repeated effects may form a pulse train. The driver does not define a
duty-cycle or thermal policy, so applications should leave an off interval
between activations.
