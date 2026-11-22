// SPDX-License-Identifier: GPL-2.0-only
package com.siemens.mp.game;

import com.nokia.mid.ui.DeviceControl;

public class Light {
    public Light() {
    }

    public static void setLightOn() {
        DeviceControl.flashLights(Integer.MAX_VALUE);
    }

    public static void setLightOff() {
        DeviceControl.flashLights(0);
    }
}
