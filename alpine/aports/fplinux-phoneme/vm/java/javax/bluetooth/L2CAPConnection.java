// SPDX-License-Identifier: GPL-2.0-only
package javax.bluetooth;

import java.io.IOException;
import javax.microedition.io.Connection;

/**
 * Packet connection over L2CAP. Opening a btl2cap URL in this runtime
 * throws ConnectionNotFoundException.
 */
public interface L2CAPConnection extends Connection {
    int DEFAULT_MTU = 672;
    int MINIMUM_MTU = 48;

    int getTransmitMTU() throws IOException;

    int getReceiveMTU() throws IOException;

    void send(byte[] data) throws IOException;

    int receive(byte[] inBuf) throws IOException;

    boolean ready() throws IOException;
}
