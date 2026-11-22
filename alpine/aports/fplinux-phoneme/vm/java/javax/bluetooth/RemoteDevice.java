// SPDX-License-Identifier: GPL-2.0-only
package javax.bluetooth;

import java.io.IOException;
import javax.microedition.io.Connection;

/**
 * A remote Bluetooth device, identified by its address. MIDlets have no
 * access to Bluetooth discovery or connections.
 */
public class RemoteDevice {
    private final String address;

    /** The address is 12 hexadecimal digits without a prefix. */
    protected RemoteDevice(String address) {
        if (address == null) {
            throw new NullPointerException("Bluetooth address");
        }
        if (address.length() != 12) {
            throw new IllegalArgumentException("Bluetooth address length");
        }
        for (int i = 0; i < 12; i++) {
            char c = address.charAt(i);
            if ((c < '0' || c > '9') && (c < 'A' || c > 'F') && (c < 'a' || c > 'f')) {
                throw new IllegalArgumentException("Bluetooth address digit");
            }
        }
        this.address = address.toUpperCase();
    }

    /** The device of a Bluetooth connection; the runtime has no such connection. */
    public static RemoteDevice getRemoteDevice(Connection conn) throws IOException {
        throw rejectConnection(conn);
    }

    private static RuntimeException rejectConnection(Connection conn) {
        if (conn == null) {
            return new NullPointerException("connection");
        }
        return new IllegalArgumentException("not a Bluetooth connection");
    }

    public final String getBluetoothAddress() {
        return address;
    }

    public String getFriendlyName(boolean alwaysAsk) throws IOException {
        throw new IOException(LocalDevice.UNAVAILABLE);
    }

    public boolean isTrustedDevice() {
        return false;
    }

    public boolean isAuthenticated() {
        return false;
    }

    public boolean isEncrypted() {
        return false;
    }

    public boolean isAuthorized(Connection conn) throws IOException {
        throw rejectConnection(conn);
    }

    /** Authentication needs an open connection to the device, and there is none. */
    public boolean authenticate() throws IOException {
        throw new IOException("no connection to " + address);
    }

    public boolean authorize(Connection conn) throws IOException {
        throw rejectConnection(conn);
    }

    public boolean encrypt(Connection conn, boolean on) throws IOException {
        throw rejectConnection(conn);
    }

    /** True for a RemoteDevice with the same Bluetooth address. */
    public boolean equals(Object obj) {
        return obj instanceof RemoteDevice && address.equals(((RemoteDevice) obj).address);
    }

    public int hashCode() {
        return address.hashCode();
    }
}
