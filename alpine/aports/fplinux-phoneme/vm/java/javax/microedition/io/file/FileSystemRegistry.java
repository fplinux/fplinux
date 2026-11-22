// SPDX-License-Identifier: GPL-2.0-only
package javax.microedition.io.file;

import com.sun.midp.io.j2me.file.Protocol;
import java.util.Enumeration;
import java.util.Timer;
import java.util.TimerTask;
import java.util.Vector;

public final class FileSystemRegistry {
    private static final long REFRESH_MS = 1000;
    private static final Vector listeners = new Vector();
    private static CardMonitor monitor;

    private FileSystemRegistry() {
    }

    public static boolean addFileSystemListener(FileSystemListener listener) {
        if (listener == null) {
            throw new NullPointerException();
        }
        synchronized (listeners) {
            if (!listeners.contains(listener)) {
                if (monitor == null) {
                    CardMonitor started = new CardMonitor();
                    started.timer.schedule(started, 0, REFRESH_MS);
                    monitor = started;
                }
                listeners.addElement(listener);
            }
        }
        return true;
    }

    public static boolean removeFileSystemListener(FileSystemListener listener) {
        if (listener == null) {
            throw new NullPointerException();
        }
        synchronized (listeners) {
            boolean removed = listeners.removeElement(listener);
            if (removed && listeners.isEmpty()) {
                monitor.timer.cancel();
                monitor = null;
            }
            return removed;
        }
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

    private static native void requestRootRefresh0();

    private static final class CardMonitor extends TimerTask {
        final Timer timer = new Timer();
        private boolean mounted = Protocol.rootAvailable(Protocol.MEMORY_CARD);

        public void run() {
            FileSystemListener[] subscribed;
            int event;
            synchronized (listeners) {
                if (monitor != this) {
                    return;
                }
                requestRootRefresh0();
                boolean available = Protocol.rootAvailable(Protocol.MEMORY_CARD);
                if (available == mounted) {
                    return;
                }
                mounted = available;
                event = mounted ? FileSystemListener.ROOT_ADDED : FileSystemListener.ROOT_REMOVED;
                subscribed = new FileSystemListener[listeners.size()];
                listeners.copyInto(subscribed);
            }
            for (int i = 0; i < subscribed.length; i++) {
                try {
                    subscribed[i].rootChanged(event, "e:/");
                } catch (Throwable error) {
                    error.printStackTrace();
                }
            }
        }
    }
}
