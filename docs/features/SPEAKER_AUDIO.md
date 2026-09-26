# Speaker audio

The Nokia 3210 4G (TA-1618) plays PCM through its single front speaker. The
INOI 240 Modern 4G and INOI 244 Modern 4G play through their rear loudspeaker.
Each phone requires its own fitted audio profile; without it,
`Speaker Playback Switch` and `Speaker Playback Volume` are absent. The speaker
plays alone or together with wired headphones, as described in
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
volume table. The phone's stock speaker equalizer and ALC also apply by
default; see [Playback processing](HEADPHONE_AUDIO.md#playback-processing).
The INOI speaker set applies a low-cut filter, a narrow notch near 200 Hz and
a presence boost before the amplifier, so bass reaches the speaker attenuated.
The Nokia TA-1618 speaker set carries that phone's own multi-band tuning.
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
playback. With the equalizer off, INOI 240 speaker playback below about 300 Hz
makes the phone vibrate.
