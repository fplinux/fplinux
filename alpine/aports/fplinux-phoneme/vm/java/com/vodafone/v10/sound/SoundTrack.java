// SPDX-License-Identifier: GPL-2.0-only
package com.vodafone.v10.sound;

import java.io.IOException;
import java.util.Vector;
import javax.microedition.media.MediaException;
import javax.microedition.media.Player;
import javax.microedition.media.PlayerListener;
import javax.microedition.media.control.VolumeControl;

public class SoundTrack {
    private static final int NO_SOUND = 0;
    private static final int READY = 1;
    private static final int PLAYING = 2;

    private static final Vector tasks = new Vector();
    private static Thread worker;

    private Sound sound;
    private int state = NO_SOUND;
    private int volume = 100;
    private int request;
    private Player player;
    private int remaining;

    private final PlayerListener listener = new PlayerListener() {
        public void playerUpdate(Player source, String event, Object data) {
            synchronized (SoundTrack.this) {
                if (source != player) {
                    return;
                }
                if (PlayerListener.STARTED.equals(event)) {
                    state = PLAYING;
                } else if (PlayerListener.END_OF_MEDIA.equals(event)) {
                    // MMAPI reports completion before each automatic repeat.
                    if (remaining > 0 && --remaining == 0) {
                        finishPlayback();
                    }
                } else if (PlayerListener.ERROR.equals(event)) {
                    finishPlayback();
                }
            }
        }
    };

    SoundTrack() {
    }

    public synchronized Sound getSound() {
        return sound;
    }

    /** Returns 0 without a sound, 1 when ready and 2 during playback. */
    public synchronized int getState() {
        return state;
    }

    public synchronized void setSound(Sound next) {
        request++;
        sound = next;
        finishPlayback();
    }

    public void removeSound() {
        setSound(null);
    }

    /** Sets the volume as a percentage, limited to 0..100. */
    public synchronized void setVolume(int level) {
        volume = Math.max(0, Math.min(100, level));
        final Player current = player;
        if (current != null) {
            submit(new Runnable() {
                public void run() {
                    applyVolume(current);
                }
            });
        }
    }

    /** Plays the sound loop times, or until stopped when loop is 0. */
    public synchronized void play(final int loop) {
        if (loop < 0) {
            throw new IllegalArgumentException("loop count");
        }
        if (sound == null) {
            return;
        }
        final int id = ++request;
        final Sound target = sound;
        finishPlayback();
        submit(new Runnable() {
            public void run() {
                start(target, id, loop == 0 ? -1 : loop);
            }
        });
    }

    public synchronized void stop() {
        request++;
        finishPlayback();
    }

    // This also invalidates late events from players being closed.
    private void finishPlayback() {
        final Player old = player;
        player = null;
        state = sound == null ? NO_SOUND : READY;
        if (old != null) {
            submit(new Runnable() {
                public void run() {
                    close(old);
                }
            });
        }
    }

    // MMAPI may wait for the audio service, so game threads only queue work.
    private static void submit(Runnable task) {
        synchronized (tasks) {
            tasks.addElement(task);
            if (worker == null) {
                worker = new Thread(new Runnable() {
                    public void run() {
                        runTasks();
                    }
                });
                worker.start();
            }
            tasks.notify();
        }
    }

    private static void runTasks() {
        for (;;) {
            Runnable next;
            synchronized (tasks) {
                while (tasks.isEmpty()) {
                    try {
                        tasks.wait();
                    } catch (InterruptedException ignored) {
                    }
                }
                next = (Runnable)tasks.elementAt(0);
                tasks.removeElementAt(0);
            }
            next.run();
        }
    }

    private void start(Sound target, int id, int loops) {
        synchronized (this) {
            if (request != id) {
                return;
            }
        }
        Player prepared = null;
        boolean started = false;
        try {
            prepared = target.createPlayer();
            prepared.realize();
            prepared.addPlayerListener(listener);
            prepared.setLoopCount(loops);
            synchronized (this) {
                if (request != id) {
                    return;
                }
                player = prepared;
                remaining = loops;
            }
            applyVolume(prepared);
            synchronized (this) {
                if (player != prepared) {
                    return;
                }
            }
            prepared.start();
            started = true;
        } catch (IOException unavailable) {
        } catch (MediaException unavailable) {
        } catch (IllegalStateException unavailable) {
        } finally {
            if (!started && prepared != null) {
                synchronized (this) {
                    if (player == prepared) {
                        player = null;
                        state = READY;
                    }
                }
                close(prepared);
            }
        }
    }

    private static void close(Player old) {
        try {
            old.close();
        } catch (IllegalStateException closed) {
        }
    }

    private void applyVolume(Player current) {
        int level;
        synchronized (this) {
            level = volume;
        }
        try {
            VolumeControl control = (VolumeControl)current.getControl("VolumeControl");
            if (control != null) {
                control.setLevel(level);
            }
        } catch (IllegalStateException closed) {
        }
    }
}
