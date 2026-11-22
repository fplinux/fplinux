// SPDX-License-Identifier: GPL-2.0-only
package javax.microedition.io;

import java.io.IOException;

import com.sun.midp.midlet.MIDletStateHandler;
import com.sun.midp.midlet.MIDletSuite;

/** The foreground runtime has no service for push or alarm launches. */
public class PushRegistry {
    private PushRegistry() {
    }

    public static void registerConnection(String connection, String midlet,
                                          String filter)
            throws ClassNotFoundException, IOException {
        if (connection == null || connection.indexOf(':') <= 0 ||
            filter == null) {
            throw new IllegalArgumentException("Invalid push connection or filter");
        }
        requireRegisteredMidlet(midlet);
        throw new ConnectionNotFoundException(
            "Push delivery is unavailable for " + connection);
    }

    public static boolean unregisterConnection(String connection) {
        return false;
    }

    public static String[] listConnections(boolean available) {
        return new String[0];
    }

    public static String getMIDlet(String connection) {
        return null;
    }

    public static String getFilter(String connection) {
        return null;
    }

    public static long registerAlarm(String midlet, long time)
            throws ClassNotFoundException, ConnectionNotFoundException {
        requireRegisteredMidlet(midlet);
        throw new ConnectionNotFoundException("MIDlet alarm launch is unavailable");
    }

    private static void requireRegisteredMidlet(String name)
            throws ClassNotFoundException {
        MIDletSuite suite = MIDletStateHandler.getMidletStateHandler()
                                              .getMIDletSuite();
        if (name == null || suite == null || !suite.isRegistered(name)) {
            throw new ClassNotFoundException(name);
        }
    }
}
