/* SPDX-License-Identifier: GPL-2.0-only */
package fplinux.fixtures.media;

import com.nokia.mid.sound.Sound;
import java.io.ByteArrayInputStream;
import javax.microedition.media.Manager;
import javax.microedition.media.MediaException;
import javax.microedition.media.Player;
import javax.microedition.midlet.MIDlet;

/** The actual native VM must reject playback without a UI audio provider. */
public final class HeadlessMediaMIDlet extends MIDlet {
    protected void startApp() {
        Player player = null;
        try {
            player = Manager.createPlayer(new ByteArrayInputStream(AudioMIDlet.wave()),
                                          "audio/x-wav");
            try {
                player.prefetch();
                throw new RuntimeException("Headless audio accepted");
            } catch (MediaException expected) {
                AudioMIDlet.require(player.getState() == Player.UNREALIZED,
                                    "False headless PREFETCHED");
            }
            try {
                Manager.playTone(69, 100, 50);
                throw new RuntimeException("Headless tone accepted");
            } catch (MediaException expected) {
            }
            nokiaSound();
            System.out.println("PM-MEDIA headless failure honest");
        } catch (Throwable failure) {
            System.out.println("PM-MEDIA FAIL " + failure);
            failure.printStackTrace();
        } finally {
            if (player != null) {
                player.close();
            }
            notifyDestroyed();
        }
    }

    private void nokiaSound() {
        // Untitled program: scale 1, tempo 160, one quarter-note A.
        byte[] ota = { 2, 74, 58, 64, 4, 0, 6, (byte) 137, 3, 72, 0 };
        Sound sound = new Sound(ota, Sound.FORMAT_TONE);
        AudioMIDlet.require(sound.getState() == Sound.SOUND_STOPPED,
                            "OTA sound was not initialized");
        sound.play(1);
        AudioMIDlet.require(sound.getState() == Sound.SOUND_STOPPED,
                            "Headless Nokia sound reports playback");
        sound.init(1000, 500L);
        AudioMIDlet.require(sound.getState() == Sound.SOUND_STOPPED,
                            "Simple sound was not initialized");
        sound.setGain(-1);
        AudioMIDlet.require(sound.getGain() == 0, "Nokia gain floor");
        sound.setGain(256);
        AudioMIDlet.require(sound.getGain() == 255, "Nokia gain ceiling");
        try {
            sound.init(new byte[] { 2, 74, 58 }, Sound.FORMAT_TONE);
            throw new RuntimeException("Incomplete OTA tone accepted");
        } catch (IllegalArgumentException expected) {
            AudioMIDlet.require(sound.getState() == Sound.SOUND_UNINITIALIZED,
                                "Failed Nokia sound init kept old sound");
        }
    }

    protected void pauseApp() {
    }

    protected void destroyApp(boolean unconditional) {
    }
}
