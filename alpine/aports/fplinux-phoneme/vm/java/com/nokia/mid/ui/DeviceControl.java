// SPDX-License-Identifier: GPL-2.0-only
package com.nokia.mid.ui;

import com.sun.midp.lcdui.DisplayDeviceAccess;

public final class DeviceControl {
    private static final DisplayDeviceAccess DISPLAY = new DisplayDeviceAccess();

    private DeviceControl() {
    }

    public static void setLights(int device, int level) {
        if (device != 0 || level < 0 || level > 100) {
            throw new IllegalArgumentException("light or level");
        }
        setBacklight0(level);
    }

    public static void flashLights(long duration) {
        DISPLAY.flashBacklight(0, durationMillis(duration));
    }

    public static void startVibra(int frequency, long duration) {
        if (frequency < 0 || frequency > 100) {
            throw new IllegalArgumentException("vibration frequency");
        }
        int millis = durationMillis(duration);
        if (!DISPLAY.vibrate(0, frequency == 0 ? 0 : millis)) {
            throw new IllegalStateException("vibration unavailable");
        }
    }

    public static void stopVibra() {
        DISPLAY.vibrate(0, 0);
    }

    private static int durationMillis(long duration) {
        if (duration < 0) {
            throw new IllegalArgumentException("duration");
        }
        return (int) Math.min(duration, Integer.MAX_VALUE);
    }

    private static native void setBacklight0(int level);
}
