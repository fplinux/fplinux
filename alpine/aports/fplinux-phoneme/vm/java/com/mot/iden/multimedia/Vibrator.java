// SPDX-License-Identifier: GPL-2.0-only
package com.mot.iden.multimedia;

import com.sun.midp.lcdui.DisplayDeviceAccess;

public class Vibrator extends Thread {
    public static final int MAX_VIBRATE_TIME = 500;
    public static final int MIN_PAUSE_TIME = 50;

    private static final DisplayDeviceAccess DISPLAY = new DisplayDeviceAccess();

    private Vibrator() {
    }

    public static void vibrateFor(int millis) {
        if (millis <= 0 || millis > MAX_VIBRATE_TIME) {
            throw new IllegalArgumentException("vibration duration");
        }
        DISPLAY.vibrate(0, millis);
    }

    public static void vibratorOn() {
        vibrateFor(MAX_VIBRATE_TIME);
    }

    public static void vibratorOff() {
        DISPLAY.vibrate(0, 0);
    }
}
