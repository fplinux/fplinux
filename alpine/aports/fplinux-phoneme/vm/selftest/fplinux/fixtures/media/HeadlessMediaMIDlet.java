/* SPDX-License-Identifier: GPL-2.0-only */
package fplinux.fixtures.media;

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

    protected void pauseApp() {
    }

    protected void destroyApp(boolean unconditional) {
    }
}
