// SPDX-License-Identifier: GPL-2.0-only
package com.sprintpcs.media;

import com.sun.midp.lcdui.DisplayDeviceAccess;

public final class Vibrator {
    private static final DisplayDeviceAccess DISPLAY = new DisplayDeviceAccess();

    private Vibrator() {
    }

    public static void vibrate(int duration) {
        if (duration < 0) {
            throw new IllegalArgumentException("vibration duration");
        }
        DISPLAY.vibrate(0, duration);
    }
}
