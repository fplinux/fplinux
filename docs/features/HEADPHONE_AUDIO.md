# Headphone audio

FPLinux plays stereo PCM through the 3.5 mm headphone socket on the exact
`inoi-240-modern-4g`, `inoi-244-modern-4g` and `nokia-ta1618` targets. Their
images include `aplay` and `amixer` and expose the shared ALSA card and PCM
name `UMS9117 Headphones`.

After loading the phone and opening its session, use the supported
[file-transfer](FILE_TRANSFER.md) workflow to copy a known WAV file to the
phone. The default ALSA device converts input to two-channel signed 16-bit
little-endian PCM at 48 kHz. Run these commands in the phone session:

```sh
amixer -c 0 cset name='Headphone Playback Volume' 3,3
aplay /tmp/audio.wav
```

Keeping ordinary playback at 48 kHz avoids reopening the headphone path when
successive files use different source rates. The direct `hw:0,0` boundary
supports stereo S16_LE at 24 kHz or 48 kHz while idle silence is disabled. With
idle silence enabled, direct playback is fixed at 48 kHz; applications use the
default device when conversion is required.
Simultaneous [microphone capture](MICROPHONE_AUDIO.md) also requires 48 kHz
headphone playback.

## Fitted profile and volume

Each supported target has an optional fitted gain profile extracted from the
exact phone's stock data. In a source checkout, prepare it before building:

```sh
./fplinux device-data prepare <target>
```

Do not reuse a profile from another physical phone or model.

A standalone archive uses the profile already included by its build and cannot
prepare different fitted data.

With the fitted profile, `Headphone Playback Volume` has separate left and
right integer levels from `0` through `9`. Without the complete profile, audio
remains available with the generic range `0` through `6`. The initial level is
`1` on both channels, and level `0` mutes that channel.

```sh
amixer -c 0 cset name='Headphone Playback Volume' 0,0
amixer -c 0 cset name='Headphone Playback Volume' 1,1
amixer -c 0 cget name='Headphone Playback Volume'
```

A setting persists across playbacks and resets on a new boot. The fitted profile
provides the stock PGA level and digital-gain steps, not a complete stock audio
pipeline. Both INOI phones use source data with EQ bypassed. Nokia source data
enables a VBC EQ that FPLinux does not implement; Linux deliberately uses a flat
VBC path, so its Nokia tonal response is not stock-equivalent.

Headphone DC and depop calibration still runs on the phone when the codec is
prepared. The fitted NAND-derived profile neither supplies nor replaces that
hardware calibration.

## Idle silence

`Headphone Idle Silence Switch` controls whether the driver keeps the headphone
path active with zero PCM while no playback stream is running. It is `on` by
default on the supported targets. Disable it when lower idle power is more
important than silent playback transitions:

```sh
amixer -c 0 cset name='Headphone Idle Silence Switch' off
amixer -c 0 cset name='Headphone Idle Silence Switch' on
amixer -c 0 cget name='Headphone Idle Silence Switch'
```

The enabled idle path has this measured USB-input power cost compared with
`off`:

- INOI 240 Modern 4G: about `0.024–0.026 W`, or `8–9%`;
- INOI 244 Modern 4G: `0.34907 W` instead of `0.32519 W`, a difference of
  `0.02387 W`, or `7.34%`.
- Nokia 3210 4G (TA-1618): `0.32569 W` instead of `0.30202 W`, a difference of
  `0.02367 W`, or `7.84%`.

The switch does not alter PCM that is already playing. The enabled path keeps
the output quiet between streams and avoids reopening it for normal playback.
Disabling the switch allows the hardware path to stop after playback; starting
it again or changing the direct hardware rate can produce one audible
transition.

## Limits

[Phone microphone recording](MICROPHONE_AUDIO.md) uses the capture side of the
same ALSA card. Every current target also has a selectable
[speaker output](SPEAKER_AUDIO.md).
[FM radio](FM_RADIO.md) uses the headphone jack but has a separate playback
path. Nokia PCM playback support is partial because its stock VBC EQ is not
implemented; the fitted gain steps do not provide stock tonal parity.
