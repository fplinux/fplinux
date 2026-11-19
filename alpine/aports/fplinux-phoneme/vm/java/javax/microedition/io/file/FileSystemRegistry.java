// SPDX-License-Identifier: GPL-2.0-only
package javax.microedition.io.file;

import com.sun.midp.io.j2me.file.Protocol;
import java.util.Enumeration;
import java.util.Vector;

public final class FileSystemRegistry {
    private FileSystemRegistry() {
    }

    public static Enumeration listRoots() {
        Vector roots = new Vector(2);
        if (Protocol.rootAvailable(Protocol.MY_FILES)) {
            roots.addElement("c:/");
        }
        if (Protocol.rootAvailable(Protocol.MEMORY_CARD)) {
            roots.addElement("e:/");
        }
        return roots.elements();
    }
}
