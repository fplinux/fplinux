// SPDX-License-Identifier: GPL-2.0-only
package fplinux.fixtures.clock;

import javax.microedition.midlet.MIDlet;

// Deadlines kept by the loops that games wait in, with and without another
// Java thread that has to make progress meanwhile.
public final class ClockWaitMIDlet extends MIDlet {
    private static final int WAITS = 5;
    private static final int WAIT_MS = 50;

    private static volatile boolean stop;
    private static volatile long progress;
    private static volatile long sleeperLate;
    private static long last;
    private static int backwards;

    private static long now() {
        long now = System.currentTimeMillis();
        if (now < last) {
            backwards++;
        }
        last = now;
        return now;
    }

    // Returns the largest lateness in ms over WAITS waits of WAIT_MS each.
    private static long wait(boolean yielding) {
        long latest = 0;
        for (int wait = 0; wait < WAITS; wait++) {
            long deadline = now() + WAIT_MS;
            long now;
            if (yielding) {
                do {
                    Thread.yield();
                } while ((now = now()) < deadline);
            } else {
                while ((now = now()) < deadline) {
                }
            }
            if (now - deadline > latest) {
                latest = now - deadline;
            }
        }
        return latest;
    }

    private static final class Counter extends Thread {
        public void run() {
            while (!stop) {
                progress++;
            }
        }
    }

    private static final class Sleeper extends Thread {
        public void run() {
            while (!stop) {
                long start = System.currentTimeMillis();
                try {
                    Thread.sleep(5);
                } catch (InterruptedException interrupted) {
                    return;
                }
                long late = System.currentTimeMillis() - start - 5;
                if (late > sleeperLate) {
                    sleeperLate = late;
                }
            }
        }
    }

    protected void startApp() {
        long busy = wait(false);
        long yielding = wait(true);

        Counter counter = new Counter();
        counter.start();
        long withCounter = wait(false);
        long counted = progress;
        stop = true;
        try {
            counter.join();
        } catch (InterruptedException interrupted) {
        }
        stop = false;

        Sleeper sleeper = new Sleeper();
        sleeper.start();
        long withSleeper = wait(false);
        stop = true;
        try {
            sleeper.join();
        } catch (InterruptedException interrupted) {
        }

        System.out.println("PM-CLOCK late busy=" + busy + " yield=" + yielding
                           + " with-counter=" + withCounter + " with-sleeper=" + withSleeper);
        System.out.println("PM-CLOCK counter-progress=" + (counted > 0));
        System.out.println("PM-CLOCK sleeper-late=" + sleeperLate);
        System.out.println("PM-CLOCK backwards=" + backwards);
        notifyDestroyed();
    }

    protected void pauseApp() {
    }

    protected void destroyApp(boolean unconditional) {
    }
}
