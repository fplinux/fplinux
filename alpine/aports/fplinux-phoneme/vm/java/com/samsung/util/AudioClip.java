// SPDX-License-Identifier: GPL-2.0-only
package com.samsung.util;

import com.sun.midp.lcdui.DisplayDeviceAccess;
import java.io.ByteArrayInputStream;
import java.io.IOException;
import java.io.InputStream;
import javax.microedition.media.Manager;
import javax.microedition.media.MediaException;
import javax.microedition.media.Player;
import javax.microedition.media.PlayerListener;
import javax.microedition.media.control.VolumeControl;

public class AudioClip {
    public static final int TYPE_MMF = 1;
    public static final int TYPE_MP3 = 2;
    public static final int TYPE_MIDI = 3;

    private static final Object LOCK = new Object();
    private static Thread worker;
    private static boolean changed;
    private static Playback requested;
    private static Playback active;

    private final String contentType;
    private final byte[] data;
    private final String resource;

    public AudioClip(int type, byte[] audioData, int audioOffset, int audioLength) {
        contentType = contentType(type);
        if (audioData == null) {
            throw new NullPointerException();
        }
        if (audioOffset < 0 || audioLength < 0 ||
                audioLength > audioData.length - audioOffset) {
            throw new ArrayIndexOutOfBoundsException("audio range");
        }
        data = new byte[audioLength];
        System.arraycopy(audioData, audioOffset, data, 0, audioLength);
        resource = null;
    }

    public AudioClip(int type, String filename) throws IOException {
        contentType = contentType(type);
        if (filename == null) {
            throw new NullPointerException();
        }
        resource = filename.startsWith("/") ? filename : "/" + filename;
        data = null;
        openResource().close();
    }

    private static String contentType(int type) {
        switch (type) {
        case TYPE_MMF:
            return null;
        case TYPE_MP3:
            return "audio/mpeg";
        case TYPE_MIDI:
            return "audio/midi";
        default:
            throw new IllegalArgumentException("audio type");
        }
    }

    private InputStream openResource() throws IOException {
        InputStream input = AudioClip.class.getResourceAsStream(resource);
        if (input == null) {
            throw new IOException("Audio resource not found: " + resource);
        }
        return input;
    }

    public static boolean isSupported() {
        return DisplayDeviceAccess.isAudioSupported();
    }

    public void play(int loop, int volume) {
        if (loop < 0 || loop > 255 || volume < 0 || volume > 5) {
            throw new IllegalArgumentException("loop count or volume");
        }
        requireAudio();
        if (contentType == null) {
            throw new IllegalStateException("MMF audio is unavailable");
        }
        synchronized (LOCK) {
            requested = new Playback(this, loop == 0 ? -1 : loop, volume * 20);
            if (worker == null) {
                worker = new Thread(new Runnable() {
                    public void run() {
                        serveRequests();
                    }
                });
                worker.start();
            }
            signalChange();
        }
    }

    public void pause() {
        setPaused(true);
    }

    public void resume() {
        setPaused(false);
    }

    private void setPaused(boolean paused) {
        requireAudio();
        synchronized (LOCK) {
            if (requested != null && requested.clip == this && requested.paused != paused) {
                requested.paused = paused;
                signalChange();
            }
        }
    }

    public void stop() {
        requireAudio();
        synchronized (LOCK) {
            if (requested != null && requested.clip == this) {
                requested = null;
                signalChange();
            }
        }
    }

    private static void requireAudio() {
        if (!isSupported()) {
            throw new IllegalStateException("Audio clips are unavailable");
        }
    }

    private static void signalChange() {
        changed = true;
        LOCK.notify();
    }

    // One clip owns playback. MMAPI waits never hold the request lock.
    private static void serveRequests() {
        for (;;) {
            Playback previous = null;
            Playback next;
            Player current;
            boolean paused;
            synchronized (LOCK) {
                while (!changed) {
                    try {
                        LOCK.wait();
                    } catch (InterruptedException ignored) {
                    }
                }
                changed = false;
                next = requested;
                if (active != next) {
                    previous = active;
                    active = null;
                }
                current = active == null ? null : active.player;
                paused = next != null && next.paused;
            }
            if (previous != null) {
                previous.player.close();
            }
            if (next != null) {
                next.update(current, paused);
            }
        }
    }

    private static final class Playback implements PlayerListener {
        private final AudioClip clip;
        private final int count;
        private final int volume;
        private int remaining;
        private boolean paused;
        private Player player;

        Playback(AudioClip clip, int count, int volume) {
            this.clip = clip;
            this.count = count;
            this.volume = volume;
            remaining = count;
        }

        private Player createPlayer() throws IOException, MediaException {
            InputStream input = clip.data == null ? clip.openResource() :
                new ByteArrayInputStream(clip.data);
            boolean created = false;
            try {
                Player result = Manager.createPlayer(input, clip.contentType);
                created = true;
                return result;
            } finally {
                if (!created) {
                    input.close();
                }
            }
        }

        void update(Player current, boolean shouldPause) {
            try {
                if (current == null) {
                    current = createPlayer();
                    current.realize();
                    current.setLoopCount(count);
                    VolumeControl gain = (VolumeControl)current.getControl("VolumeControl");
                    gain.setLevel(volume);
                    current.addPlayerListener(this);
                    synchronized (LOCK) {
                        if (requested == this) {
                            player = current;
                            active = this;
                            shouldPause = paused;
                        } else {
                            shouldPause = true;
                        }
                    }
                    if (player == null) {
                        current.close();
                        return;
                    }
                }
                if (shouldPause) {
                    current.stop();
                } else {
                    current.start();
                }
            } catch (IOException failure) {
                finish(current);
            } catch (MediaException failure) {
                finish(current);
            } catch (IllegalStateException failure) {
                finish(current);
            }
        }

        private void finish(Player finished) {
            synchronized (LOCK) {
                if (active == this) {
                    active = null;
                }
                if (requested == this) {
                    requested = null;
                }
            }
            if (finished != null) {
                finished.close();
            }
        }

        public void playerUpdate(Player source, String event, Object value) {
            if (PlayerListener.END_OF_MEDIA.equals(event)) {
                if (count == -1 || --remaining != 0) {
                    return;
                }
            } else if (!PlayerListener.ERROR.equals(event)) {
                return;
            }
            finish(source);
        }
    }
}
