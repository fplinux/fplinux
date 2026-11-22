// SPDX-License-Identifier: GPL-2.0-only
package javax.bluetooth;

import javax.microedition.io.Connection;

/** Bluetooth discovery and connections are unavailable to MIDlets. */
public class LocalDevice {
    static final String UNAVAILABLE = "Bluetooth is unavailable in this runtime";

    private LocalDevice() {
    }

    public static LocalDevice getLocalDevice() throws BluetoothStateException {
        throw new BluetoothStateException(UNAVAILABLE);
    }

    public static String getProperty(String property) {
        if (property == null) {
            throw new NullPointerException("property");
        }
        return null;
    }

    public static boolean isPowerOn() {
        return false;
    }

    public DiscoveryAgent getDiscoveryAgent() {
        return new DiscoveryAgent();
    }

    public String getFriendlyName() {
        return null;
    }

    public DeviceClass getDeviceClass() {
        return null;
    }

    public String getBluetoothAddress() {
        return null;
    }

    public boolean setDiscoverable(int mode) throws BluetoothStateException {
        if (mode != DiscoveryAgent.NOT_DISCOVERABLE
                && (mode < DiscoveryAgent.LIAC || mode > DiscoveryAgent.LAST_ACCESS_CODE)) {
            throw new IllegalArgumentException("discoverable mode");
        }
        throw new BluetoothStateException(UNAVAILABLE);
    }

    public int getDiscoverable() {
        return DiscoveryAgent.NOT_DISCOVERABLE;
    }

    public ServiceRecord getRecord(Connection notifier) {
        if (notifier == null) {
            throw new NullPointerException("notifier");
        }
        throw new IllegalArgumentException("not a Bluetooth service notifier");
    }

    public void updateRecord(ServiceRecord srvRecord) throws ServiceRegistrationException {
        if (srvRecord == null) {
            throw new NullPointerException("service record");
        }
        throw new IllegalArgumentException("not a record of a local service");
    }
}
