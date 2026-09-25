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

Keeping ordinary playback at 48 kHz avoids reopening the enabled outputs when
successive files use different source rates. The direct `hw:0,0` boundary
supports stereo S16_LE at 24 kHz or 48 kHz on every
[output combination](#playback-outputs) while idle silence is disabled. With
idle silence enabled, direct playback is fixed at 48 kHz; applications use the
default device when conversion is required.
Simultaneous [microphone capture](MICROPHONE_AUDIO.md) also requires 48 kHz
playback.

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

## Playback outputs

`Headphone Playback Switch` and `Speaker Playback Switch` choose the outputs
that play PCM and [FM radio](FM_RADIO.md). The headphone switch is always
present and is `on` by default. The speaker switch exists only with the fitted
profile, like `Speaker Playback Volume`, and is `off` by default. To add the
[speaker](SPEAKER_AUDIO.md) to the headphones:

```sh
amixer -c 0 cset name='Speaker Playback Switch' on
amixer -c 0 cget name='Headphone Playback Switch'
```

The enabled combination selects the fitted levels for PCM and FM:

- Headphones alone use the fitted headset level of each channel, selected by
  `Headphone Playback Volume`.
- The speaker alone uses the fitted speaker level selected by
  `Speaker Playback Volume`.
- Both outputs use the phone's fitted combined-output level selected by
  `Speaker Playback Volume`. `Headphone Playback Volume` then only mutes a
  channel at `0`; every nonzero value plays at the same level.
  `Speaker Playback Volume` `0` silences only the speaker; the headphones keep
  playing at the lowest combined-output level.
- With both switches `off`, playback runs, but nothing is audible.

The speaker plays the sum of both channels, so PCM is scaled down whenever the
speaker is enabled. The headphones receive the same scaled-down PCM: with both
outputs at level `9`, they play more quietly than headphones alone at level `9`.

The switches can change at any time, including while a playback or capture
stream is open or running and while FM radio plays; the change applies to the
running stream. Turning the speaker on while no other output is enabled
produces one audible click; every other switch change is silent. To move from
the headphones to the speaker alone without the click, turn the speaker on
first and then turn the headphones off. Plugging in or removing wired
headphones does not change either switch. The switches persist across
playbacks and reset on a new boot.

## Idle silence

`Idle Silence Playback Switch` controls whether the driver keeps every enabled
[playback output](#playback-outputs) active with zero PCM while no playback
stream is running. It is `on` by default on the supported targets. Disable it
when lower idle power is more important than silent playback transitions:

```sh
amixer -c 0 cset name='Idle Silence Playback Switch' off
amixer -c 0 cset name='Idle Silence Playback Switch' on
amixer -c 0 cget name='Idle Silence Playback Switch'
```

The enabled idle path on headphones alone has this measured USB-input power
cost compared with `off`:

- INOI 240 Modern 4G: about `0.024–0.026 W`, or `8–9%`;
- INOI 244 Modern 4G: `0.34907 W` instead of `0.32519 W`, a difference of
  `0.02387 W`, or `7.34%`.
- Nokia 3210 4G (TA-1618): `0.32569 W` instead of `0.30202 W`, a difference of
  `0.02367 W`, or `7.84%`.

On the speaker alone at level `1`, the enabled idle path has this cost:

- INOI 240 Modern 4G: `0.73244 W` instead of `0.68682 W`, a difference of
  `0.04562 W`, or `6.64%`;
- INOI 244 Modern 4G: `0.75741 W` instead of `0.71380 W`, a difference of
  `0.04361 W`, or `6.11%`;
- Nokia 3210 4G (TA-1618): `0.51243 W` instead of `0.47482 W`, a difference of
  `0.03761 W`, or `7.92%`.

The switch does not alter PCM that is already playing. The enabled path keeps
the outputs quiet between streams and avoids reopening them for normal playback.
Disabling the switch allows the hardware path to stop after playback; starting
it again or changing the direct hardware rate can produce one audible
transition. On the speaker, each stream start and stop then clicks.

## Limits

[Phone microphone recording](MICROPHONE_AUDIO.md) uses the capture side of the
same ALSA card; the output switches do not affect it. [FM radio](FM_RADIO.md)
needs a cable in the headphone jack as its antenna and cannot run alongside PCM
playback. On a phone that vibrates through its speaker,
[vibration](VIBRATION.md#speaker-vibration) briefly mutes headphone audio.
Nokia PCM playback support is partial because its stock VBC EQ is not
implemented; the fitted gain steps do not provide stock tonal parity.
