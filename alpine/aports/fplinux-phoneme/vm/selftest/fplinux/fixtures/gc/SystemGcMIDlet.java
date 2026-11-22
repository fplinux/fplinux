// SPDX-License-Identifier: GPL-2.0-only
package fplinux.fixtures.gc;

import java.lang.ref.WeakReference;
import javax.microedition.midlet.MIDlet;

// What a program can observe of System.gc(): a weak reference to an
// unreachable object is cleared whether the object is young or old, and
// freeMemory() reflects the memory a collection freed once less than a
// quarter of the heap is free. A call that cannot free anything a program can
// observe may be skipped.
public final class SystemGcMIDlet extends MIDlet {
    private static final int ROUNDS = 8;
    private static final int CHUNK = 4096;
    private static Object sink;

    // Allocates chunks that stay reachable until the caller drops them, so
    // that collections during the allocation promote them and everything else
    // alive at that time to the old generation.
    private static Object[] promote(long bytes) {
        Object[] ballast = new Object[(int) (bytes / CHUNK)];
        for (int i = 0; i < ballast.length; i++) {
            ballast[i] = new byte[CHUNK];
        }
        return ballast;
    }

    private static int clearedYoung() {
        int cleared = 0;
        for (int round = 0; round < ROUNDS; round++) {
            Object referent = new byte[64];
            WeakReference reference = new WeakReference(referent);
            referent = null;
            System.gc();
            if (reference.get() == null) {
                cleared++;
            }
        }
        return cleared;
    }

    private static int clearedOld() {
        long total = Runtime.getRuntime().totalMemory();
        int cleared = 0;
        for (int round = 0; round < ROUNDS; round++) {
            Object referent = new byte[1024];
            WeakReference reference = new WeakReference(referent);
            Object[] ballast = promote(total / 3);
            // Reachable only through the weak reference from here on.
            referent = null;
            ballast = null;
            System.gc();
            if (reference.get() == null) {
                cleared++;
            }
        }
        return cleared;
    }

    // Fills the heap, drops most of it and asks how much is free.
    private static boolean freeMemoryReflectsCollection() {
        Runtime runtime = Runtime.getRuntime();
        Object[] fill = new Object[(int) (runtime.totalMemory() / CHUNK)];
        int held = 0;
        try {
            while (held < fill.length) {
                fill[held] = new byte[CHUNK];
                held++;
            }
        } catch (OutOfMemoryError full) {
        }
        int dropped = held / 4 * 3;
        for (int i = 0; i < dropped; i++) {
            fill[i] = null;
        }
        long before = runtime.freeMemory();
        System.gc();
        long after = runtime.freeMemory();
        return after - before >= (long) dropped * CHUNK / 2;
    }

    // Garbage that no weak reference mentions, with plenty of memory free.
    private static boolean idleCallSkipped() {
        Runtime runtime = Runtime.getRuntime();
        for (int i = 0; i < 25; i++) {
            sink = new byte[CHUNK];
        }
        sink = null;
        long before = runtime.freeMemory();
        System.gc();
        return runtime.freeMemory() <= before;
    }

    protected void startApp() {
        int young = clearedYoung();
        int old = clearedOld();
        boolean reflected = freeMemoryReflectsCollection();
        // Collect what the previous checks left behind first.
        System.gc();
        boolean skipped = idleCallSkipped();
        System.out.println("PM-GC weak-young=" + young + "/" + ROUNDS);
        System.out.println("PM-GC weak-old=" + old + "/" + ROUNDS);
        System.out.println("PM-GC free-after-low-memory=" + reflected);
        System.out.println("PM-GC idle-call-skipped=" + skipped);
        notifyDestroyed();
    }

    protected void pauseApp() {
    }

    protected void destroyApp(boolean unconditional) {
    }
}
