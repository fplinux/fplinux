// SPDX-License-Identifier: GPL-2.0-only
package com.siemens.mp.game;

import com.sun.midp.lcdui.DisplayDeviceAccess;

public class Vibrator {
    private static final DisplayDeviceAccess DISPLAY = new DisplayDeviceAccess();

    public Vibrator() {
    }

    public static void startVibrator() {
        DISPLAY.vibrate(0, Integer.MAX_VALUE);
    }

    public static void stopVibrator() {
        DISPLAY.vibrate(0, 0);
    }

    public static void triggerVibrator(int duration) {
        DISPLAY.vibrate(0, Math.max(duration, 0));
    }
}
