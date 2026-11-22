// SPDX-License-Identifier: GPL-2.0-only
package com.samsung.util;

import com.sun.midp.lcdui.DisplayDeviceAccess;

public final class LCDLight {
    private static final DisplayDeviceAccess DISPLAY = new DisplayDeviceAccess();

    private LCDLight() {
    }

    public static boolean isSupported() {
        return DisplayDeviceAccess.isBacklightSupported();
    }

    public static void on(int duration) {
        if (duration <= 0) {
            throw new IllegalArgumentException("backlight duration");
        }
        flash((int)Math.min(duration * 1000L, Integer.MAX_VALUE));
    }

    public static void off() {
        flash(0);
    }

    private static void flash(int millis) {
        if (!DISPLAY.flashBacklight(0, millis)) {
            throw new IllegalStateException("Backlight is unavailable");
        }
    }
}
