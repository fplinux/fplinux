# Phone microphone

Nokia TA-1618, INOI 240 Modern 4G and INOI 244 Modern 4G record from their
built-in microphones or
the microphone of a connected four-contact wired headset. ALSA capture supports
mono signed 16-bit PCM at 48 kHz. `Capture Source` defaults to `Internal`;
select `Headset` before opening a recording stream to use the wired microphone:

```sh
amixer -c 0 cset name='Capture Source' Headset
arecord -D hw:0,0 -f S16_LE -c 1 -r 48000 -d 10 /tmp/recording.wav
```

To return to the built-in microphone, select `Internal` between recordings:

```sh
amixer -c 0 cset name='Capture Source' Internal
```

Plugging or unplugging the headset does not change this control automatically.
It cannot be changed while a capture stream is open. Use [file transfer](FILE_TRANSFER.md)
to copy the recording from the phone; files in `/tmp` disappear on reboot.

Capture can run alongside 48-kHz PCM playback through the enabled
[playback outputs](HEADPHONE_AUDIO.md#playback-outputs). Playback at 24 kHz
remains available only without capture. FM radio cannot run alongside either
PCM direction. This audio duplex does not provide live microphone monitoring or
cellular calling.

The newly opened headset input can have a settling transient during roughly
the first second of recorded audio; start recording before the sound you want
to capture. There is no automatic gain or noise suppression.
