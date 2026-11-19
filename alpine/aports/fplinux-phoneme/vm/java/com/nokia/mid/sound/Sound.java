// SPDX-License-Identifier: GPL-2.0-only
package com.nokia.mid.sound;

import java.io.ByteArrayInputStream;
import java.io.IOException;
import javax.microedition.media.Manager;
import javax.microedition.media.MediaException;
import javax.microedition.media.Player;
import javax.microedition.media.PlayerListener;
import javax.microedition.media.control.VolumeControl;

public class Sound {
    public static final int FORMAT_TONE = 1;
    public static final int FORMAT_WAV = 5;
    public static final int SOUND_PLAYING = 0;
    public static final int SOUND_STOPPED = 1;
    public static final int SOUND_UNINITIALIZED = 3;

    private byte[] data;
    private boolean tone;
    private boolean endless;
    private boolean started;
    private int gain = 255;
    private Player player;
    private SoundListener listener;
    private final PlayerListener relay = new PlayerListener() {
        public void playerUpdate(Player source, String event, Object value) {
            SoundListener callback;
            int state;
            synchronized (Sound.this) {
                if (source != player) {
                    return;
                }
                if (PlayerListener.STARTED.equals(event)) {
                    state = SOUND_PLAYING;
                } else if (PlayerListener.STOPPED.equals(event) ||
                           PlayerListener.END_OF_MEDIA.equals(event) ||
                           PlayerListener.ERROR.equals(event)) {
                    state = SOUND_STOPPED;
                    if (PlayerListener.ERROR.equals(event)) {
                        player = null;
                        started = false;
                    }
                } else {
                    return;
                }
                callback = listener;
            }
            if (callback != null) {
                callback.soundStateChanged(Sound.this, state);
            }
        }
    };

    public Sound(byte[] bytes, int type) {
        init(bytes, type);
    }

    public Sound(int frequency, long duration) {
        init(frequency, duration);
    }

    public synchronized void init(byte[] bytes, int type) {
        release();
        if (bytes == null) {
            throw new NullPointerException();
        }
        if (type == FORMAT_TONE) {
            OtaTone decoded = OtaTone.decode(bytes);
            data = decoded.sequence();
            endless = decoded.endless();
            tone = true;
        } else if (type == FORMAT_WAV) {
            if (bytes.length < 12 || bytes[0] != 'R' || bytes[1] != 'I' ||
                    bytes[2] != 'F' || bytes[3] != 'F' || bytes[8] != 'W' ||
                    bytes[9] != 'A' || bytes[10] != 'V' || bytes[11] != 'E') {
                throw new IllegalArgumentException("invalid WAV sound");
            }
            data = new byte[bytes.length];
            System.arraycopy(bytes, 0, data, 0, bytes.length);
            tone = false;
        } else {
            throw new IllegalArgumentException("unsupported sound format");
        }
    }

    public synchronized void init(int frequency, long duration) {
        release();
        data = OtaTone.singleNote(frequency, duration);
        tone = true;
    }

    public static int[] getSupportedFormats() {
        return new int[] { FORMAT_TONE, FORMAT_WAV };
    }

    public static int getConcurrentSoundCount(int type) {
        if (type != FORMAT_TONE && type != FORMAT_WAV) {
            throw new IllegalArgumentException("unsupported sound format");
        }
        return 1;
    }

    public synchronized int getState() {
        if (data == null) {
            return SOUND_UNINITIALIZED;
        }
        return player != null && player.getState() == Player.STARTED ?
            SOUND_PLAYING : SOUND_STOPPED;
    }

    public synchronized int getGain() {
        return gain;
    }

    public synchronized void setGain(int level) {
        gain = Math.max(0, Math.min(255, level));
        applyGain();
    }

    public synchronized void setSoundListener(SoundListener target) {
        listener = target;
    }

    public synchronized void play(int loop) {
        if (loop < 0) {
            throw new IllegalArgumentException("loop count");
        }
        if (data == null) {
            return;
        }
        try {
            if (player != null && player.getState() == Player.CLOSED) {
                closePlayer();
            }
            if (player == null) {
                createPlayer();
            }
            player.stop();
            player.setMediaTime(0);
            player.setLoopCount(loop == 0 || endless ? -1 : loop);
            player.start();
            started = true;
        } catch (IOException unavailable) {
            closePlayer();
        } catch (MediaException unavailable) {
            closePlayer();
        }
    }

    public synchronized void stop() {
        if (player == null || player.getState() == Player.CLOSED) {
            return;
        }
        try {
            player.stop();
        } catch (MediaException failure) {
            closePlayer();
        }
    }

    public synchronized void resume() {
        if (player == null || !started) {
            return;
        }
        int state = player.getState();
        if (state == Player.CLOSED || state == Player.STARTED) {
            return;
        }
        try {
            if (tone) {
                player.setMediaTime(0);
            }
            player.start();
        } catch (MediaException unavailable) {
            closePlayer();
        }
    }

    public synchronized void release() {
        closePlayer();
        data = null;
        endless = false;
    }

    private void createPlayer() throws IOException, MediaException {
        player = Manager.createPlayer(new ByteArrayInputStream(data),
            tone ? "audio/x-tone-seq" : "audio/x-wav");
        player.realize();
        applyGain();
        player.addPlayerListener(relay);
    }

    private void applyGain() {
        if (player != null && player.getState() != Player.CLOSED) {
            VolumeControl volume = (VolumeControl) player.getControl("VolumeControl");
            int percentage = (gain * 100 + 127) / 255;
            volume.setLevel(gain == 0 ? 0 : Math.max(1, percentage));
        }
    }

    private void closePlayer() {
        Player previous = player;
        player = null;
        started = false;
        if (previous != null) {
            previous.close();
        }
    }
}
