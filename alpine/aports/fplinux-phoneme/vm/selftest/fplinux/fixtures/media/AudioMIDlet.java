/* SPDX-License-Identifier: GPL-2.0-only */
package fplinux.fixtures.media;

import java.io.ByteArrayInputStream;
import java.util.Vector;
import javax.microedition.lcdui.Display;
import javax.microedition.lcdui.Form;
import javax.microedition.media.Manager;
import javax.microedition.media.MediaException;
import javax.microedition.media.Player;
import javax.microedition.media.PlayerListener;
import javax.microedition.media.control.ToneControl;
import javax.microedition.media.control.VolumeControl;
import javax.microedition.midlet.MIDlet;

/** Runs on a UI connection with audio, separately from headless tests. */
public final class AudioMIDlet extends MIDlet implements Runnable {
    private boolean started;
    private final Player[] midi = new Player[12];

    static final byte[] MIDI = {
        77, 84, 104, 100, 0, 0, 0, 6, 0, 0, 0, 1, 0, 96,
        77, 84, 114, 107, 0, 0, 0, 12,
        0, (byte)144, 69, 100, 96, (byte)128, 69, 0, 0, (byte)255, 47, 0
    };

    static byte[] wave() {
        byte[] bytes = new byte[1644];
        byte[] header = {
            82, 73, 70, 70, 100, 6, 0, 0, 87, 65, 86, 69,
            102, 109, 116, 32, 16, 0, 0, 0, 1, 0, 1, 0,
            64, 31, 0, 0, (byte)128, 62, 0, 0, 2, 0, 16, 0,
            100, 97, 116, 97, 64, 6, 0, 0
        };
        System.arraycopy(header, 0, bytes, 0, header.length);
        for (int i = 0; i < 800; i++) {
            short sample = (short)((i / 10 % 2 == 0) ? 6000 : -6000);
            bytes[44 + i * 2] = (byte)sample;
            bytes[45 + i * 2] = (byte)(sample >> 8);
        }
        return bytes;
    }

    static void require(boolean condition, String message) {
        if (!condition) {
            throw new RuntimeException(message);
        }
    }

    protected void startApp() {
        if (!started) {
            started = true;
            Display.getDisplay(this).setCurrent(new Form("Media test"));
            new Thread(this).start();
        }
    }

    protected void pauseApp() {
    }

    protected void destroyApp(boolean unconditional) {
        for (int i = 0; i < midi.length; i++) {
            if (midi[i] != null) {
                midi[i].close();
            }
        }
    }

    private static final class Events implements PlayerListener {
        final Vector names = new Vector();

        public synchronized void playerUpdate(Player player, String name, Object value) {
            if (PlayerListener.STARTED.equals(name) ||
                PlayerListener.STOPPED.equals(name) ||
                PlayerListener.END_OF_MEDIA.equals(name) ||
                PlayerListener.ERROR.equals(name)) {
                names.addElement(name);
                notifyAll();
            }
        }

        synchronized void await(int count) throws InterruptedException {
            long deadline = System.currentTimeMillis() + 10000;
            while (names.size() < count && System.currentTimeMillis() < deadline) {
                wait(100);
            }
            require(names.size() >= count, "Media event deadline");
        }
    }

    public void run() {
        Player pcm = null;
        Player tone = null;
        try {
            require("false".equals(System.getProperty("supports.audio.capture")),
                    "Capture capability");
            require(Manager.getSupportedContentTypes("http").length == 0,
                    "Unsupported protocol");
            for (int i = 0; i < midi.length; i++) {
                midi[i] = Manager.createPlayer(new ByteArrayInputStream(MIDI), "audio/midi");
                midi[i].prefetch();
                require(midi[i].getState() == Player.PREFETCHED, "MIDI prefetch");
                require(midi[i].getDuration() == 500000, "MIDI duration");
            }
            System.out.println("PM-MEDIA prefetch 12");
            midi[0].start();
            Thread.sleep(80);
            midi[0].stop();
            long stopped = midi[0].getMediaTime();
            Thread.sleep(80);
            require(midi[0].getMediaTime() == stopped, "Stopped time advances");
            midi[1].start();
            Thread.sleep(80);
            midi[1].stop();
            require(midi[0].getState() == Player.PREFETCHED, "Stopped MIDI state");
            long actual = midi[0].setMediaTime(250000);
            require(actual >= 249000 && actual <= 250000, "Actual MIDI seek");
            midi[0].start();
            System.out.println("PM-MEDIA MIDI stop seek switch");

            pcm = Manager.createPlayer(new ByteArrayInputStream(wave()), "audio/x-wav");
            pcm.realize();
            require(pcm.getDuration() == 100000, "WAV duration");
            require(pcm.setMediaTime(50000) == 50000, "Actual WAV seek");
            require(pcm.setMediaTime(-1) == 0, "Negative WAV seek");
            VolumeControl volume = (VolumeControl)pcm.getControl("VolumeControl");
            require(volume.setLevel(25) == 25, "Local player gain");
            volume.setMute(true);
            require(volume.isMuted() && volume.getLevel() == 25, "Mute preserves level");
            volume.setMute(false);
            final Events events = new Events();
            pcm.addPlayerListener(new PlayerListener() {
                public void playerUpdate(Player player, String event, Object data) {
                    throw new RuntimeException("Listener isolation");
                }
            });
            pcm.addPlayerListener(events);
            pcm.setLoopCount(2);
            pcm.start();
            events.await(4);
            require(PlayerListener.STARTED.equals(events.names.elementAt(0)), "First STARTED");
            require(PlayerListener.END_OF_MEDIA.equals(events.names.elementAt(1)), "First EOF");
            require(PlayerListener.STARTED.equals(events.names.elementAt(2)), "Loop STARTED");
            require(PlayerListener.END_OF_MEDIA.equals(events.names.elementAt(3)), "Final EOF");
            require(pcm.getState() == Player.PREFETCHED, "Final WAV state");
            System.out.println("PM-MEDIA WAV mixing gain loop listener");
            midi[0].stop();
            for (int i = 0; i < midi.length; i++) {
                midi[i].deallocate();
                midi[i].close();
            }

            tone = Manager.createPlayer(Manager.TONE_DEVICE_LOCATOR);
            tone.realize();
            ToneControl control = (ToneControl)tone.getControl("ToneControl");
            try {
                control.setSequence(new byte[] { -2, 1, -9, 2, 69 });
                throw new RuntimeException("Truncated tone accepted");
            } catch (IllegalArgumentException expected) {
            }
            control.setSequence(new byte[] { -2, 1, 69, 16 });
            Events tones = new Events();
            tone.addPlayerListener(tones);
            tone.start();
            tones.await(2);
            Manager.playTone(69, 100, 25);
            Thread.sleep(500);
            System.out.println("PM-MEDIA tones");
            System.out.println("PM-MEDIA ok");
        } catch (Throwable failure) {
            System.out.println("PM-MEDIA FAIL " + failure);
            failure.printStackTrace();
        } finally {
            if (pcm != null) {
                pcm.close();
            }
            if (tone != null) {
                tone.close();
            }
            destroyApp(true);
            notifyDestroyed();
        }
    }
}
