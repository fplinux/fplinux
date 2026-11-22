// SPDX-License-Identifier: GPL-2.0-only
package javax.microedition.io;

/** A serial port connection; the runtime offers no comm: protocol. */
public interface CommConnection extends StreamConnection {
    int getBaudRate();

    int setBaudRate(int baudrate);
}
