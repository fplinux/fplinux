// SPDX-License-Identifier: GPL-2.0-only
package fplinux.fixtures.bluetooth;

import javax.bluetooth.BluetoothStateException;
import javax.bluetooth.LocalDevice;
import javax.microedition.midlet.MIDlet;

public class BluetoothAvailabilityMIDlet extends MIDlet {
    protected void startApp() {
        try {
            LocalDevice.getLocalDevice();
            throw new IllegalStateException("Unexpected local Bluetooth device");
        } catch (BluetoothStateException expected) {
            System.out.println("PM-BLUETOOTH local device unavailable");
        }
        notifyDestroyed();
    }

    protected void pauseApp() { }

    protected void destroyApp(boolean unconditional) { }
}
