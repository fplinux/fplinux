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

    private static byte[] waveMp3() {
        // A 440 Hz, 8 kHz mono MP3 tone in a RIFF WAVE data chunk.
        String mp3 =
            "ffe318c4000d0806e259410002923425e07fc0183e0f83e1f0401004010c" +
            "1fe7c1f3e81c080632efd4081c9cffff13821a8130fe27041d500c3f9304" +
            "380cff0c74aa818186180001ffe318c4060e80f6acf194280091789a31ff" +
            "2e932644d7f8cb132014d03134d000ad0d094c0d21d1de1102874c637ff8" +
            "88743a60f078a77f951105415111effc15ffffd7f8180180ffe318c4070e" +
            "80ce340197100079018e425c101821404a40c010047c0c37034340c08702" +
            "5606022824c061561a5806017810515f1be3fffffff6ffffffffffe8ffff" +
            "df79a8ccd4aeffe318c4080ea12aa00199380035ffffccebe5118843ae97" +
            "c44d7fe2b61f43a287cd91a1920b33ffd18f3dd80702d0a111b0fff99fce" +
            "1d28c152bffb02a860fac427cfa3782ad56cffe318c4080e48ca5001cf18" +
            "016cbd7b168f93c873dc312795d3a94828b8a89b50d50c5f053a86c74e9d" +
            "2a18b09f3e8de13e8db8a0b882b828e847745745554c414d45332e313030";
        byte[] header = {
            82, 73, 70, 70, (byte)140, 1, 0, 0, 87, 65, 86, 69,
            102, 109, 116, 32, 16, 0, 0, 0, 85, 0, 1, 0,
            64, 31, 0, 0, (byte)232, 3, 0, 0, 1, 0, 0, 0,
            100, 97, 116, 97, 104, 1, 0, 0
        };
        byte[] bytes = new byte[header.length + mp3.length() / 2];
        System.arraycopy(header, 0, bytes, 0, header.length);
        for (int i = 0; i < mp3.length(); i += 2) {
            bytes[header.length + i / 2] =
                (byte)Integer.parseInt(mp3.substring(i, i + 2), 16);
        }
        return bytes;
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

    private static void playWave(byte[] data, String type)
            throws Exception {
        Player player = Manager.createPlayer(new ByteArrayInputStream(data), type);
        Events events = new Events();
        try {
            player.realize();
            require("audio/x-wav".equals(player.getContentType()), "WAV type " + type);
            require(player.setMediaTime(50000) == 50000, "WAV seek " + type);
            player.addPlayerListener(events);
            player.start();
            events.await(2);
            require(PlayerListener.STARTED.equals(events.names.elementAt(0)),
                    "WAV start " + type);
            require(PlayerListener.END_OF_MEDIA.equals(events.names.elementAt(1)),
                    "WAV EOF " + type);
            require(player.getState() == Player.PREFETCHED, "WAV end state " + type);
        } finally {
            player.close();
        }
    }

    private static void contentTypes() throws Exception {
        byte[] pcm = wave();
        playWave(pcm, null);
        playWave(pcm, "");
        playWave(pcm, " \t ");
        playWave(waveMp3(), "audio/x-wav");
        String[] unsupported = { "audio/unknown", " ; charset=x" };
        for (int i = 0; i < unsupported.length; i++) {
            try {
                Player unexpected = Manager.createPlayer(new ByteArrayInputStream(pcm),
                                                         unsupported[i]);
                unexpected.close();
                throw new RuntimeException("Unsupported type accepted " + unsupported[i]);
            } catch (MediaException expected) {
            }
        }
        System.out.println("PM-MEDIA default MIME and MP3 in WAV");
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
            contentTypes();
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
