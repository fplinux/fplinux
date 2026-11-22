// SPDX-License-Identifier: GPL-2.0-only
package com.samsung.util;

import com.sun.midp.lcdui.DisplayDeviceAccess;

public final class Vibration {
    private static final DisplayDeviceAccess DISPLAY = new DisplayDeviceAccess();

    private Vibration() {
    }

    public static boolean isSupported() {
        return DisplayDeviceAccess.isVibrationSupported();
    }

    public static void start(int duration, int strength) {
        if (duration <= 0 || strength < 1 || strength > 5) {
            throw new IllegalArgumentException("vibration duration or strength");
        }
        vibrate((int)Math.min(duration * 1000L, Integer.MAX_VALUE));
    }

    public static void stop() {
        vibrate(0);
    }

    private static void vibrate(int millis) {
        if (!DISPLAY.vibrate(0, millis)) {
            throw new IllegalStateException("Vibration is unavailable");
        }
    }
}
