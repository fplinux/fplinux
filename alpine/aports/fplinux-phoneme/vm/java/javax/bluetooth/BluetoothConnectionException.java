// SPDX-License-Identifier: GPL-2.0-only
package javax.bluetooth;

import java.io.IOException;

/** A Bluetooth connection could not be established; getStatus() tells why. */
public class BluetoothConnectionException extends IOException {
    public static final int UNKNOWN_PSM = 0x0001;
    public static final int SECURITY_BLOCK = 0x0002;
    public static final int NO_RESOURCES = 0x0003;
    public static final int FAILED_NOINFO = 0x0004;
    public static final int TIMEOUT = 0x0005;
    public static final int UNACCEPTABLE_PARAMS = 0x0006;

    private final int status;

    /** The status must be one of the constants. */
    public BluetoothConnectionException(int error) {
        this(error, null);
    }

    public BluetoothConnectionException(int error, String msg) {
        super(msg);
        if (error < UNKNOWN_PSM || error > UNACCEPTABLE_PARAMS) {
            throw new IllegalArgumentException("connection error status");
        }
        status = error;
    }

    public int getStatus() {
        return status;
    }
}
