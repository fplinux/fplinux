# Speaker audio

Nokia TA-1618 plays PCM through its single front speaker. INOI 240 and INOI 244
Modern 4G play through their rear loudspeaker. Each phone requires its own
fitted audio profile; without it, `Speaker Playback Switch` and
`Speaker Playback Volume` are absent. The speaker plays alone or together with
wired headphones, as described in
[Playback outputs](HEADPHONE_AUDIO.md#playback-outputs).

To play through the speaker alone without the
[switching click](HEADPHONE_AUDIO.md#playback-outputs), enable the speaker
before disabling the headphones and start at volume level `1`:

```sh
amixer -c 0 cset name='Speaker Playback Switch' on
amixer -c 0 cset name='Headphone Playback Switch' off
amixer -c 0 cset name='Speaker Playback Volume' 1
aplay /tmp/audio.wav
```

Level `1` is the default, and level `0` mutes the speaker. Levels `1` through
`9` use the selected phone's fitted stock gains, so each step follows the stock
volume table. FPLinux does not apply the stock speaker limiter or equalizer.
On the speaker alone, Nokia TA-1618 plays a near full-scale 1 kHz tone at levels
`7` through `9` with about 2% harmonic distortion. The INOI speakers reach 4–7%
with that tone, and INOI 240 already reaches about 3% from level `6` at an
ordinary level. With both outputs and a 1 kHz tone at an ordinary level, the
INOI 240 speaker stays below 1% through level `6` and reaches about 3.5% at
level `9`. Its headphone output then reaches about 1% at level `5` and about 5%
at level `9`, while headphones alone stay below 1%. The Nokia TA-1618 speaker
stays below 1% at every level with that tone and both outputs.
[Idle silence](HEADPHONE_AUDIO.md#idle-silence) keeps the
enabled speaker open between streams, so stream starts and stops do not
click.

To return to wired headphones, reverse the switches:

```sh
amixer -c 0 cset name='Speaker Playback Switch' off
amixer -c 0 cset name='Headphone Playback Switch' on
```

[FM radio](FM_RADIO.md) also plays through the speaker while it is enabled.
The INOI phones have no separate earpiece output, and their speaker also carries
[vibration](VIBRATION.md#speaker-vibration), which plays together with speaker
playback. On INOI 240, speaker playback below about 300 Hz makes the phone
vibrate because FPLinux does not apply the stock speaker equalizer that removes
this range.
