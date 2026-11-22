// SPDX-License-Identifier: GPL-2.0-only
package com.vodafone.v10.system.device;

import com.sun.midp.lcdui.DisplayDeviceAccess;

public final class DeviceControl {
    private static final int VIBRATION = 4;
    private static final int VIBRATION_MILLIS = 3000;

    private static final DeviceControl DEFAULT = new DeviceControl();
    private static final DisplayDeviceAccess DISPLAY = new DisplayDeviceAccess();

    private DeviceControl() {
    }

    public static DeviceControl getDefaultDeviceControl() {
        return DEFAULT;
    }

    /** Returns false when the device or display service is unavailable. */
    public boolean setDeviceActive(int device, boolean active) {
        return device == VIBRATION && DISPLAY.vibrate(0, active ? VIBRATION_MILLIS : 0);
    }

    /** Returns the idle state for device polling. */
    public int getDeviceState(int device) {
        return 0;
    }
}
