# FM radio

The Nokia 3210 4G (TA-1618), INOI 240 Modern 4G and INOI 244 Modern 4G receive
FM radio through their 3.5 mm headphone sockets. Plug in wired headphones or
another 3.5 mm cable before tuning: the cable acts as the FM antenna, and
connected headphones provide audio output. The receiver uses FM settings
prepared from a read-only NAND backup of that same phone before the image is
built.

Use `fplinux-fm` on the phone:

```sh
fplinux-fm scan
fplinux-fm play 100.0
fplinux-fm play 100.0 --seconds=30
```

`scan` seeks across 87.5–108.0 MHz with 0.1 MHz spacing and prints frequency
candidates. A candidate is not a guarantee of an audible station: listen to
confirm it. No calibrated signal level or station name is available. `play`
keeps the receiver and headphone route active until Ctrl-C or the optional
deadline, then mutes and closes them. FM playback cannot run alongside ordinary
PCM playback or phone microphone capture.

The receiver is also exposed as `/dev/radio0` through the standard V4L2 radio
interface. FM audio reaches the headphone codec internally; `/dev/radio0` does
not supply PCM samples and does not capture audio.
[Phone microphone recording](MICROPHONE_AUDIO.md) uses ALSA instead. If a tuner
operation times out, stop using FM and cold boot the phone before trying again.

FM audio plays only on wired headphones; it does not use the
[speaker output](SPEAKER_AUDIO.md). On a phone that vibrates through its
speaker, headphone FM audio is muted during
[vibration](VIBRATION.md#speaker-vibration) and returns about one second after
it.

Scan and headphone playback have been exercised in the default RAM profile on
all three phones. FM use with a microSD system root has not been qualified.
