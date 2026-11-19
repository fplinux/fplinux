// SPDX-License-Identifier: GPL-2.0-only
package com.nokia.mid.sound;

import java.io.ByteArrayInputStream;
import java.io.IOException;
import java.util.Vector;
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

    private static final int PLAY = 1;
    private static final int STOP = 2;
    private static final int RESUME = 3;

    private static final Vector pending = new Vector();
    private static Thread worker;

    private Playback clip;
    private int gain = 255;
    private SoundListener listener;

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
            clip = new Playback(decoded.sequence(), true, decoded.endless());
        } else if (type == FORMAT_WAV) {
            if (bytes.length < 12 || bytes[0] != 'R' || bytes[1] != 'I' ||
                    bytes[2] != 'F' || bytes[3] != 'F' || bytes[8] != 'W' ||
                    bytes[9] != 'A' || bytes[10] != 'V' || bytes[11] != 'E') {
                throw new IllegalArgumentException("invalid WAV sound");
            }
            byte[] data = new byte[bytes.length];
            System.arraycopy(bytes, 0, data, 0, bytes.length);
            clip = new Playback(data, false, false);
        } else {
            throw new IllegalArgumentException("unsupported sound format");
        }
    }

    public synchronized void init(int frequency, long duration) {
        release();
        clip = new Playback(OtaTone.singleNote(frequency, duration), true, false);
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
        return clip == null ? SOUND_UNINITIALIZED : clip.state;
    }

    public synchronized int getGain() {
        return gain;
    }

    public synchronized void setGain(int level) {
        gain = Math.max(0, Math.min(255, level));
        if (clip != null) {
            schedule(clip);
        }
    }

    public synchronized void setSoundListener(SoundListener target) {
        listener = target;
    }

    public synchronized void play(int loop) {
        if (loop < 0) {
            throw new IllegalArgumentException("loop count");
        }
        if (clip != null) {
            clip.loops = loop == 0 || clip.endless ? -1 : loop;
            clip.command = PLAY;
            schedule(clip);
        }
    }

    public synchronized void stop() {
        if (clip != null) {
            clip.command = STOP;
            clip.state = SOUND_STOPPED;
            schedule(clip);
        }
    }

    public synchronized void resume() {
        if (clip != null && clip.started && clip.state != SOUND_PLAYING &&
                clip.command != PLAY) {
            clip.command = RESUME;
            schedule(clip);
        }
    }

    public synchronized void release() {
        if (clip != null) {
            Playback old = clip;
            clip = null;
            schedule(old);
        }
    }

    /* A game tick may request an effect while MMAPI is waiting for the audio
     * server. Keep one pending update per sound and do the waits off that tick. */
    private static void schedule(Playback clip) {
        synchronized (pending) {
            if (!pending.contains(clip)) {
                pending.addElement(clip);
            }
            if (worker == null) {
                worker = new Thread(new Runnable() {
                    public void run() {
                        processSounds();
                    }
                });
                worker.start();
            }
            pending.notify();
        }
    }

    private static void processSounds() {
        for (;;) {
            Playback next;
            synchronized (pending) {
                while (pending.isEmpty()) {
                    try {
                        pending.wait();
                    } catch (InterruptedException ignored) {
                    }
                }
                next = (Playback)pending.elementAt(0);
                pending.removeElementAt(0);
            }
            next.update();
        }
    }

    private final class Playback implements PlayerListener {
        private final byte[] data;
        private final boolean tone;
        private final boolean endless;
        private int state = SOUND_STOPPED;
        private int command;
        private int loops;
        private boolean started;
        private Player player;
        private int appliedGain = 255;

        Playback(byte[] bytes, boolean isTone, boolean repeats) {
            data = bytes;
            tone = isTone;
            endless = repeats;
        }

        private void update() {
            int action;
            int loopCount;
            int newGain;
            boolean retired;
            synchronized (Sound.this) {
                retired = clip != this;
                action = command;
                command = 0;
                loopCount = loops;
                newGain = gain;
            }
            if (retired) {
                closePlayer();
                return;
            }
            try {
                if (player != null && player.getState() == Player.CLOSED) {
                    closePlayer();
                }
                boolean fresh = action == PLAY && player == null;
                if (fresh) {
                    Player prepared = Manager.createPlayer(
                        new ByteArrayInputStream(data),
                        tone ? "audio/x-tone-seq" : "audio/x-wav");
                    synchronized (Sound.this) {
                        player = prepared;
                    }
                    prepared.realize();
                    prepared.addPlayerListener(this);
                }
                if (player == null) {
                    return;
                }
                applyGain(newGain);
                if (action == STOP) {
                    player.stop();
                } else if (action == PLAY) {
                    if (!fresh) {
                        player.stop();
                        player.setMediaTime(0);
                    }
                    player.setLoopCount(loopCount);
                    startPlayer();
                } else if (action == RESUME) {
                    if (player.getState() != Player.STARTED) {
                        if (tone) {
                            player.setMediaTime(0);
                        }
                        startPlayer();
                    } else {
                        playing();
                    }
                }
            } catch (IOException unavailable) {
                closePlayer();
            } catch (MediaException unavailable) {
                closePlayer();
            } catch (IllegalStateException unavailable) {
                closePlayer();
            }
        }

        private void applyGain(int requested) {
            if (appliedGain == requested) {
                return;
            }
            VolumeControl volume = (VolumeControl)player.getControl("VolumeControl");
            int percentage = (requested * 100 + 127) / 255;
            volume.setLevel(requested == 0 ? 0 : Math.max(1, percentage));
            appliedGain = requested;
        }

        private void startPlayer() throws MediaException {
            synchronized (Sound.this) {
                if (clip != this || command == STOP) {
                    return;
                }
            }
            player.start();
            playing();
        }

        private void playing() {
            synchronized (Sound.this) {
                if (clip == this) {
                    started = true;
                    if (command != STOP) {
                        state = SOUND_PLAYING;
                    }
                }
            }
        }

        private void closePlayer() {
            Player old;
            synchronized (Sound.this) {
                old = player;
                player = null;
                started = false;
                state = SOUND_STOPPED;
                appliedGain = 255;
            }
            if (old != null) {
                old.close();
            }
        }

        public void playerUpdate(Player source, String event, Object value) {
            SoundListener callback;
            int next;
            synchronized (Sound.this) {
                if (clip != this || source != player) {
                    return;
                }
                if (PlayerListener.STARTED.equals(event)) {
                    next = SOUND_PLAYING;
                } else if (PlayerListener.STOPPED.equals(event) ||
                           PlayerListener.END_OF_MEDIA.equals(event) ||
                           PlayerListener.ERROR.equals(event)) {
                    next = SOUND_STOPPED;
                    if (PlayerListener.ERROR.equals(event)) {
                        started = false;
                    }
                } else {
                    return;
                }
                state = command == STOP ? SOUND_STOPPED : next;
                callback = listener;
            }
            if (callback != null) {
                callback.soundStateChanged(Sound.this, next);
            }
        }
    }
}
