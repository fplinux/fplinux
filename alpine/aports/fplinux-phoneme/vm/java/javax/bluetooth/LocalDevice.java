// SPDX-License-Identifier: GPL-2.0-only
package javax.bluetooth;

public class LocalDevice {
    private LocalDevice() {
    }

    public static LocalDevice getLocalDevice() throws BluetoothStateException {
        throw new BluetoothStateException("Bluetooth is unavailable in this runtime");
    }
}
