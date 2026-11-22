// SPDX-License-Identifier: GPL-2.0-only
package fplinux.fixtures.jardir;

import java.io.IOException;
import java.io.InputStream;
import javax.microedition.midlet.MIDlet;

/**
 * Opens resources of the application JAR by name and prints for each whether
 * the stream is null and, if not, the number and a hash of the bytes read.
 * The JAR may carry explicit directory entries (names ending in a slash),
 * which open as empty streams; names must match exactly.
 */
public final class JarDirMIDlet extends MIDlet {
    private static final String[] NAMES = {
        "/dir/", "/dir/sub/", "/dir", "/dir/sub", "/dir/sub/a.txt", "/dir/b.txt",
        "/dir/missing.txt", "/dir/sub/missing/", "/DIR/", "/DIR/B.TXT", "/dir/B.txt",
        "/empty/", "/empty", "/",
        // A directory entry is found again after the files behind it were opened.
        "/dir/",
    };

    protected void startApp() {
        for (int i = 0; i < NAMES.length; i++) {
            System.out.println("PM-JARDIR " + NAMES[i] + " " + describe(NAMES[i]));
        }
        notifyDestroyed();
    }

    protected void pauseApp() {
    }

    protected void destroyApp(boolean unconditional) {
    }

    private String describe(String name) {
        try {
            InputStream stream = getClass().getResourceAsStream(name);
            if (stream == null) {
                return "null";
            }
            byte[] buffer = new byte[256];
            int length = 0;
            int hash = 0;
            int count;
            while ((count = stream.read(buffer, 0, buffer.length)) > 0) {
                for (int i = 0; i < count; i++) {
                    hash = hash * 31 + (buffer[i] & 0xff);
                }
                length += count;
            }
            stream.close();
            return "length=" + length + " hash=" + hash;
        } catch (IOException failure) {
            return "IOException";
        } catch (RuntimeException failure) {
            return failure.getClass().getName();
        }
    }
}
