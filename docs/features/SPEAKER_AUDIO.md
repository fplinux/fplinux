# Speaker audio

Nokia TA-1618 plays PCM through its single front speaker. INOI 240 and INOI 244
Modern 4G play through their rear loudspeaker. Each phone requires its own
fitted audio profile; the speaker output is selected separately from wired
headphones.

Select the output before opening a playback or capture stream, then start at
volume level `1`:

```sh
amixer -c 0 cset name='PCM Playback Output' Speaker
amixer -c 0 cset name='Speaker Playback Volume' 1
aplay /tmp/audio.wav
```

Level `0` disconnects the speaker and is the default. Levels `1` through `9`
use the selected phone's fitted gains. On Nokia TA-1618, only level `1` has been
checked acoustically; higher levels may be louder.
[Idle silence](HEADPHONE_AUDIO.md#idle-silence) keeps the selected speaker open
between streams, so stream starts and stops do not click. The direct `hw:0,0` playback
device accepts stereo S16_LE at 48 kHz on this route. The default ALSA device
converts other input formats.

To return to wired headphones, close the PCM stream and select `Headphones`:

```sh
amixer -c 0 cset name='PCM Playback Output' Headphones
```

The output cannot be changed while a playback or microphone capture stream or
FM radio is open. Plugging in wired headphones does not switch it automatically.
Microphone capture can run alongside 48-kHz speaker playback. FM radio always
uses wired headphones, not this output. The INOI phones have no separate
earpiece output, and their speaker also carries
[vibration](VIBRATION.md#speaker-vibration), which plays together with speaker
playback. On INOI 240, speaker playback below about 300 Hz makes the phone
vibrate because FPLinux does not apply the stock speaker equalizer that removes
this range.
