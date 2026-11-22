// SPDX-License-Identifier: GPL-2.0-only
package com.sprintpcs.media;

import java.io.IOException;
import javax.microedition.media.MediaException;
import javax.microedition.media.PlayerListener;

public class Player {
    private static final Object LOCK = new Object();
    private static final Channel FOREGROUND = new Channel();
    private static final Channel BACKGROUND = new Channel();
    private static Thread worker;

    public Player() {
    }

    public static void play(Clip clip, int repeat) {
        post(FOREGROUND, clip, repeat);
    }

    public static void playBackground(Clip clip, int repeat) {
        post(BACKGROUND, clip, repeat);
    }

    public static void stop() {
        synchronized (LOCK) {
            FOREGROUND.setRequest(null);
            BACKGROUND.setRequest(null);
        }
    }

    private static void post(Channel channel, Clip clip, int repeat) {
        if (clip == null) {
            throw new NullPointerException("clip");
        }
        if (repeat < -1) {
            throw new IllegalArgumentException("repeat count");
        }
        int count = repeat == -1 ? -1 : (int)Math.min(repeat + 1L, Integer.MAX_VALUE);
        Playback next = new Playback(channel, clip, count);
        synchronized (LOCK) {
            if (channel.requested != null &&
                    next.priority < channel.requested.priority) {
                return;
            }
            channel.setRequest(next);
            if (worker == null) {
                worker = new Thread(new Runnable() {
                    public void run() {
                        serveRequests();
                    }
                });
                worker.start();
            }
        }
    }

    // MMAPI operations can wait; the caller can still queue replacement or stop.
    private static void serveRequests() {
        for (;;) {
            synchronized (LOCK) {
                while (!FOREGROUND.changed && !BACKGROUND.changed) {
                    try {
                        LOCK.wait();
                    } catch (InterruptedException ignored) {
                    }
                }
            }
            FOREGROUND.update();
            BACKGROUND.update();
        }
    }

    private static final class Channel {
        private boolean changed;
        private Playback requested;
        private Playback active;

        void setRequest(Playback next) {
            requested = next;
            changed = true;
            LOCK.notify();
        }

        void update() {
            Playback previous;
            Playback next;
            synchronized (LOCK) {
                if (!changed) {
                    return;
                }
                changed = false;
                previous = active;
                active = null;
                next = requested;
            }
            if (previous != null) {
                previous.player.close();
            }
            if (next != null) {
                next.start();
            }
        }
    }

    private static final class Playback implements PlayerListener {
        private final Channel channel;
        private final Clip clip;
        private final int count;
        private final int priority;
        private final int vibration;
        private int remaining;
        private javax.microedition.media.Player player;

        Playback(Channel channel, Clip clip, int count) {
            this.channel = channel;
            this.clip = clip;
            this.count = count;
            priority = clip.getPriority();
            vibration = clip.getVibration();
            remaining = count;
        }

        void start() {
            try {
                player = clip.createPlayer();
                player.realize();
                player.setLoopCount(count);
                player.addPlayerListener(this);
                boolean cancelled;
                synchronized (LOCK) {
                    cancelled = channel.requested != this;
                    if (!cancelled) {
                        channel.active = this;
                    }
                }
                if (cancelled) {
                    player.close();
                    return;
                }
                player.start();
                synchronized (LOCK) {
                    if (vibration > 0 && channel.requested == this) {
                        Vibrator.vibrate(vibration);
                    }
                }
            } catch (IOException failure) {
                finish();
            } catch (MediaException failure) {
                finish();
            } catch (IllegalStateException failure) {
                finish();
            }
        }

        private void finish() {
            synchronized (LOCK) {
                if (channel.active == this) {
                    channel.active = null;
                }
                if (channel.requested == this) {
                    channel.requested = null;
                }
            }
            if (player != null) {
                player.close();
            }
        }

        public void playerUpdate(javax.microedition.media.Player source, String event, Object value) {
            if (END_OF_MEDIA.equals(event)) {
                if (count == -1 || --remaining != 0) {
                    return;
                }
            } else if (!ERROR.equals(event)) {
                return;
            }
            finish();
        }
    }
}
