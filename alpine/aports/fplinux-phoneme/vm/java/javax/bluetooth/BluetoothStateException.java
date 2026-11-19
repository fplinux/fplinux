// SPDX-License-Identifier: GPL-2.0-only
package javax.bluetooth;

import java.io.IOException;

public class BluetoothStateException extends IOException {
    public BluetoothStateException() {
    }

    public BluetoothStateException(String message) {
        super(message);
    }
}
