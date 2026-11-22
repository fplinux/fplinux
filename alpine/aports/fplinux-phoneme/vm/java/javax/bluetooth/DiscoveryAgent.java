// SPDX-License-Identifier: GPL-2.0-only
package javax.bluetooth;

/**
 * Device inquiry and service search. LocalDevice cannot create a discovery
 * agent while the MIDlet runtime has no Bluetooth service.
 */
public class DiscoveryAgent {
    public static final int NOT_DISCOVERABLE = 0;
    public static final int GIAC = 0x9E8B33;
    public static final int LIAC = 0x9E8B00;
    public static final int CACHED = 0x00;
    public static final int PREKNOWN = 0x01;

    static final int LAST_ACCESS_CODE = 0x9E8B3F;
    private static final int LAST_ATTRIBUTE_ID = 0xFFFF;

    DiscoveryAgent() {
    }

    /** The cached or pre-known devices: none, as null. */
    public RemoteDevice[] retrieveDevices(int option) {
        if (option != CACHED && option != PREKNOWN) {
            throw new IllegalArgumentException("device list option");
        }
        return null;
    }

    public boolean startInquiry(int accessCode, DiscoveryListener listener)
            throws BluetoothStateException {
        if (listener == null) {
            throw new NullPointerException("listener");
        }
        if (accessCode < LIAC || accessCode > LAST_ACCESS_CODE) {
            throw new IllegalArgumentException("inquiry access code");
        }
        throw new BluetoothStateException(LocalDevice.UNAVAILABLE);
    }

    /** No inquiry runs, so there is nothing to cancel. */
    public boolean cancelInquiry(DiscoveryListener listener) {
        if (listener == null) {
            throw new NullPointerException("listener");
        }
        return false;
    }

    public int searchServices(int[] attrSet, UUID[] uuidSet, RemoteDevice btDev,
                              DiscoveryListener discListener)
            throws BluetoothStateException {
        if (uuidSet == null || btDev == null || discListener == null) {
            throw new NullPointerException("search argument");
        }
        if (uuidSet.length == 0) {
            throw new IllegalArgumentException("no service UUID");
        }
        for (int i = 0; i < uuidSet.length; i++) {
            if (uuidSet[i] == null) {
                throw new NullPointerException("service UUID");
            }
        }
        if (attrSet != null) {
            for (int i = 0; i < attrSet.length; i++) {
                if (attrSet[i] < 0 || attrSet[i] > LAST_ATTRIBUTE_ID) {
                    throw new IllegalArgumentException("attribute ID");
                }
            }
        }
        throw new BluetoothStateException(LocalDevice.UNAVAILABLE);
    }

    /** No search runs, so there is nothing to cancel. */
    public boolean cancelServiceSearch(int transID) {
        return false;
    }

    public String selectService(UUID uuid, int security, boolean master)
            throws BluetoothStateException {
        if (uuid == null) {
            throw new NullPointerException("service UUID");
        }
        if (security < ServiceRecord.NOAUTHENTICATE_NOENCRYPT ||
                security > ServiceRecord.AUTHENTICATE_ENCRYPT) {
            throw new IllegalArgumentException("security level");
        }
        throw new BluetoothStateException(LocalDevice.UNAVAILABLE);
    }
}
