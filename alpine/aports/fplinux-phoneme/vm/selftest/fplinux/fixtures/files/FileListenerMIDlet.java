// SPDX-License-Identifier: GPL-2.0-only
package fplinux.fixtures.files;

import java.util.Enumeration;
import javax.microedition.io.file.FileSystemListener;
import javax.microedition.io.file.FileSystemRegistry;
import javax.microedition.midlet.MIDlet;

public final class FileListenerMIDlet extends MIDlet implements FileSystemListener {
    private int events;

    protected void startApp() {
        if (hasCard() && FileSystemRegistry.addFileSystemListener(this)
                && FileSystemRegistry.addFileSystemListener(this)) {
            System.out.println("PM-FILE-LISTENER ready");
        } else {
            notifyDestroyed();
        }
    }

    public void rootChanged(int state, String rootName) {
        events++;
        if (events == 1 && "e:/".equals(rootName)
                && state == ROOT_REMOVED && !hasCard()) {
            System.out.println("PM-FILE-LISTENER removed");
            return;
        }
        if (events == 2 && "e:/".equals(rootName) && state == ROOT_ADDED && hasCard()
                && FileSystemRegistry.removeFileSystemListener(this)
                && !FileSystemRegistry.removeFileSystemListener(this)) {
            System.out.println("PM-FILE-LISTENER pass");
        } else {
            System.out.println("PM-FILE-LISTENER unexpected root change");
        }
        notifyDestroyed();
    }

    private static boolean hasCard() {
        for (Enumeration roots = FileSystemRegistry.listRoots(); roots.hasMoreElements();) {
            if ("e:/".equals(roots.nextElement())) {
                return true;
            }
        }
        return false;
    }

    protected void pauseApp() {}

    protected void destroyApp(boolean unconditional) {}
}
