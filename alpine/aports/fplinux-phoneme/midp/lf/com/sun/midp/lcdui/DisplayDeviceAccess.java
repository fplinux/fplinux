/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * fplinux-phoneme: Display.vibrate() and Display.flashBacklight(), replacing
 * upstream com.sun.midp.lcdui.DisplayDeviceAccess (build-midp-java.py lets
 * the --lf directory replace it). Upstream switches the vibrator on and
 * off and blinks the backlight from Java timers; FPLinux gets the duration
 * instead (port/canvas/fplinux-pm-device.c): the vibrator plays pulses of
 * at most 3000 ms through the session, and as the session owns the lights,
 * flashBacklight keeps the display on for the duration.
 * Both return false headless, or without a vibrator for vibrate().
 */
package com.sun.midp.lcdui;

public class DisplayDeviceAccess {

    public DisplayDeviceAccess() {
    }

    /**
     * Display.flashBacklight(): keeps the display on for duration ms; 0
     * ends an earlier request. Display has checked that the duration is
     * not negative and that it has the foreground.
     */
    public synchronized boolean flashBacklight(int displayId, int duration) {
        return flashBacklight1(duration);
    }

    /**
     * Display.vibrate(): vibrates for duration ms, replacing an earlier
     * request; 0 stops the vibrator.
     */
    public synchronized boolean vibrate(int displayId, int duration) {
        return vibrate1(duration);
    }

    private static native boolean flashBacklight1(int duration);

    private static native boolean vibrate1(int duration);
}
