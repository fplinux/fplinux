// SPDX-License-Identifier: GPL-2.0-only
package fplinux.fixtures.clock;

import javax.microedition.midlet.MIDlet;

// Waits for deadlines in a loop that only reads the clock, as games do while
// they wait for the next frame. Run under strace, the pauses show up as
// sleeps.
public final class ClockSpinMIDlet extends MIDlet {
    protected void startApp() {
        long last = 0;
        long latest = 0;
        int backwards = 0;
        for (int wait = 0; wait < 5; wait++) {
            long deadline = System.currentTimeMillis() + 20;
            long now;
            while ((now = System.currentTimeMillis()) < deadline) {
                if (now < last) {
                    backwards++;
                }
                last = now;
            }
            if (now - deadline > latest) {
                latest = now - deadline;
            }
        }
        System.out.println("PM-CLOCK spin late-max=" + latest + " backwards=" + backwards);
        notifyDestroyed();
    }

    protected void pauseApp() {
    }

    protected void destroyApp(boolean unconditional) {
    }
}
