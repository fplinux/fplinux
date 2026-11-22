// SPDX-License-Identifier: GPL-2.0-only
package javax.bluetooth;

import java.io.IOException;
import javax.microedition.io.Connection;

/** Server end of L2CAP connections; see L2CAPConnection. */
public interface L2CAPConnectionNotifier extends Connection {
    L2CAPConnection acceptAndOpen() throws IOException;
}
