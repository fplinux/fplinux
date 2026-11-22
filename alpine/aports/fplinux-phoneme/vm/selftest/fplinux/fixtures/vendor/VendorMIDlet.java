// SPDX-License-Identifier: GPL-2.0-only
package fplinux.fixtures.vendor;

import com.samsung.util.AudioClip;
import com.samsung.util.LCDLight;
import com.samsung.util.SM;
import com.samsung.util.SMS;
import com.samsung.util.Vibration;
import com.siemens.mp.game.Light;
import com.siemens.mp.game.Vibrator;
import com.sprintpcs.media.Clip;
import com.sprintpcs.media.Player;
import com.sprintpcs.util.Muglet;
import java.io.IOException;
import javax.microedition.lcdui.Image;
import javax.microedition.midlet.MIDlet;

public final class VendorMIDlet extends MIDlet {
    protected void startApp() {
        try {
            checkSamsungAudio();
            checkSamsungDevices();
            checkSiemens();
            checkSprint();
            checkMotorola();
            VodafoneMediaProbe.run();
        } catch (IOException failure) {
            throw new RuntimeException(failure.toString());
        }
        System.out.println("PM-VENDOR checks=ok");
        notifyDestroyed();
    }

    private static void checkSamsungAudio() throws IOException {
        require(AudioClip.TYPE_MMF == 1 && AudioClip.TYPE_MP3 == 2 &&
            AudioClip.TYPE_MIDI == 3, "Samsung audio format IDs");
        require(!AudioClip.isSupported(), "headless Samsung audio capability");
        AudioClip clip = new AudioClip(AudioClip.TYPE_MIDI, "tone.mid");
        try {
            new AudioClip(AudioClip.TYPE_MIDI, "/absent.mid");
            throw new RuntimeException("missing Samsung audio resource accepted");
        } catch (IOException expected) {
        }
        try {
            new AudioClip(AudioClip.TYPE_MP3, new byte[4], 2, 3);
            throw new RuntimeException("invalid Samsung clip range accepted");
        } catch (ArrayIndexOutOfBoundsException expected) {
        }
        try {
            clip.play(-1, 5);
            throw new RuntimeException("invalid Samsung loop count accepted");
        } catch (IllegalArgumentException expected) {
        }
        try {
            clip.play(1, 5);
            throw new RuntimeException("headless Samsung clip played");
        } catch (IllegalStateException expected) {
        }
        try {
            clip.pause();
            throw new RuntimeException("headless Samsung clip paused");
        } catch (IllegalStateException expected) {
        }
        try {
            clip.resume();
            throw new RuntimeException("headless Samsung clip resumed");
        } catch (IllegalStateException expected) {
        }
        try {
            clip.stop();
            throw new RuntimeException("headless Samsung clip stopped");
        } catch (IllegalStateException expected) {
        }
    }

    private static void checkSamsungDevices() {
        SM message = new SM("+77001234567", "76543210", "score 42");
        require("+77001234567".equals(message.getDestAddress()) &&
            "76543210".equals(message.getCallbackAddress()) &&
            "score 42".equals(message.getData()), "Samsung message fields");
        require(!SMS.isSupported(), "Samsung SMS capability");
        try {
            SMS.send(message);
            throw new RuntimeException("Samsung SMS unexpectedly sent");
        } catch (IllegalStateException expected) {
        }
        require(!Vibration.isSupported(), "headless Samsung vibration capability");
        try {
            Vibration.start(1, 0);
            throw new RuntimeException("invalid Samsung vibration strength accepted");
        } catch (IllegalArgumentException expected) {
        }
        try {
            Vibration.start(1, 5);
            throw new RuntimeException("headless Samsung vibrator started");
        } catch (IllegalStateException expected) {
        }
        try {
            Vibration.stop();
            throw new RuntimeException("headless Samsung vibrator stopped");
        } catch (IllegalStateException expected) {
        }
        require(!LCDLight.isSupported(), "headless Samsung backlight capability");
        try {
            LCDLight.on(0);
            throw new RuntimeException("invalid Samsung backlight duration accepted");
        } catch (IllegalArgumentException expected) {
        }
        try {
            LCDLight.on(1);
            throw new RuntimeException("headless Samsung backlight enabled");
        } catch (IllegalStateException expected) {
        }
        try {
            LCDLight.off();
            throw new RuntimeException("headless Samsung backlight disabled");
        } catch (IllegalStateException expected) {
        }
    }

    private static void checkSiemens() throws IOException {
        new Light();
        Light.setLightOn();
        Light.setLightOff();
        new Vibrator();
        Vibrator.triggerVibrator(100);
        Vibrator.startVibrator();
        Vibrator.stopVibrator();

        Image image = com.siemens.mp.ui.Image.createImageWithoutScaling("/pixel.png");
        require(image.getWidth() == 3 && image.getHeight() == 2, "Siemens image dimensions");
        int[] rgb = new int[6];
        image.getRGB(rgb, 0, 3, 0, 0, 3, 2);
        require(rgb[0] == 0xffff0000 && rgb[1] == 0xff00ff00 && rgb[2] == 0xff0000ff &&
            rgb[3] == 0xffffffff && rgb[4] == 0xff000000 && rgb[5] == 0xffffff00,
            "Siemens image pixels");
        try {
            com.siemens.mp.ui.Image.createImageWithoutScaling("/absent.png");
            throw new RuntimeException("missing Siemens image accepted");
        } catch (IOException expected) {
        }
    }

    private static void checkSprint() throws IOException {
        require(Muglet.getMuglet() == null, "no Sprint URI handler invocation");
        com.sprintpcs.util.System.setExitURI(null);
        try {
            com.sprintpcs.util.System.setExitURI("https://example.invalid/");
            throw new RuntimeException("Sprint exit URI accepted");
        } catch (IllegalStateException expected) {
        }
        try {
            new Clip("file:///tone.mid", "audio/midi", 1, 0);
            throw new RuntimeException("Sprint native file locator accepted");
        } catch (IOException expected) {
        }
        Clip clip = new Clip("resource:/tone.mid", "audio/midi", 1, 0);
        require("audio/midi".equals(clip.getContentType()), "Sprint clip content type");
        try {
            Player.play(clip, -2);
            throw new RuntimeException("invalid Sprint repeat count accepted");
        } catch (IllegalArgumentException expected) {
        }
        Player.play(clip, 0);
        Player.playBackground(clip, -1);
        Player.stop();
        try {
            com.sprintpcs.media.Vibrator.vibrate(-1);
            throw new RuntimeException("invalid Sprint vibration duration accepted");
        } catch (IllegalArgumentException expected) {
        }
    }

    private static void checkMotorola() {
        try {
            com.mot.iden.multimedia.Vibrator.vibrateFor(0);
            throw new RuntimeException("zero Motorola vibration duration accepted");
        } catch (IllegalArgumentException expected) {
        }
        try {
            com.mot.iden.multimedia.Vibrator.vibrateFor(501);
            throw new RuntimeException("overlong Motorola vibration accepted");
        } catch (IllegalArgumentException expected) {
        }
        com.mot.iden.multimedia.Vibrator.vibrateFor(500);
        com.mot.iden.multimedia.Vibrator.vibratorOn();
        com.mot.iden.multimedia.Vibrator.vibratorOff();
    }

    private static void require(boolean condition, String name) {
        if (!condition) {
            throw new RuntimeException(name);
        }
    }

    protected void pauseApp() {
    }

    protected void destroyApp(boolean unconditional) {
    }
}
