// SPDX-License-Identifier: GPL-2.0-only
package fplinux.fixtures.vendor;

import com.vodafone.v10.sound.Sound;
import com.vodafone.v10.sound.SoundPlayer;
import com.vodafone.v10.sound.SoundTrack;
import com.vodafone.v10.system.device.DeviceControl;
import java.io.IOException;

final class VodafoneMediaProbe {
    static void run() throws IOException {
        DeviceControl devices = DeviceControl.getDefaultDeviceControl();
        require(devices == DeviceControl.getDefaultDeviceControl(), "shared device control");
        require(devices.getDeviceState(3) == 0, "idle key polling");
        require(!devices.setDeviceActive(3, true), "unsupported key polling activation");
        require(!devices.setDeviceActive(4, true), "headless vibration start");
        require(!devices.setDeviceActive(4, false), "headless vibration stop");

        SoundPlayer player = SoundPlayer.getPlayer();
        require(player == SoundPlayer.getPlayer(), "shared player");
        SoundTrack midi = player.getTrack();
        SoundTrack unsupported = player.getTrack();
        require(midi != unsupported && midi.getState() == 0, "independent empty tracks");
        Sound sound = new Sound("tone.mid");
        midi.setSound(sound);
        require(midi.getSound() == sound && midi.getState() == 1, "set resource sound");
        midi.setSound(new Sound("/tone.mid"));
        try {
            new Sound("/absent.mid");
            throw new RuntimeException("Missing Vodafone sound accepted");
        } catch (IOException expected) {
        }
        try {
            midi.play(-1);
            throw new RuntimeException("Negative Vodafone repeat count accepted");
        } catch (IllegalArgumentException expected) {
        }

        unsupported.setSound(new Sound(new byte[] { 'M', 'M', 'M', 'D', 0, 0, 0, 0 }));
        unsupported.play(1);
        midi.play(2);
        long until = System.currentTimeMillis() + 300;
        while (System.currentTimeMillis() < until) {
            require(midi.getState() == 1, "headless MIDI reports playback");
            require(unsupported.getState() == 1, "unsupported sound reports playback");
            try {
                Thread.sleep(5);
            } catch (InterruptedException failure) {
                throw new RuntimeException(failure.toString());
            }
        }
        midi.stop();
        require(midi.getState() == 1, "stop keeps sound");
        midi.removeSound();
        midi.play(1);
        require(midi.getSound() == null && midi.getState() == 0, "remove clears sound");
    }

    private static void require(boolean condition, String property) {
        if (!condition) {
            throw new RuntimeException("Vodafone " + property);
        }
    }
}
