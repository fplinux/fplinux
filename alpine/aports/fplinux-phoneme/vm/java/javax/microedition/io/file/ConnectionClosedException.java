// SPDX-License-Identifier: GPL-2.0-only
package javax.microedition.io.file;

public class ConnectionClosedException extends RuntimeException {
    public ConnectionClosedException() {
    }

    public ConnectionClosedException(String message) {
        super(message);
    }
}
