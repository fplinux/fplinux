// SPDX-License-Identifier: GPL-2.0-only
package fplinux.fixtures.jit;

import javax.microedition.midlet.MIDlet;

// Loads the classes that run.sh writes with make-verify-classes.py and prints,
// for each, the result of f(0), f(1) and f(7), or the error the verifier threw
// when it rejected the class.
public final class VerifyMapsMIDlet extends MIDlet {
    private static final String[] NAMES = {
        "VerifyLong", "VerifyNoMap", "VerifyUnordered", "VerifyDuplicate"
    };

    protected void startApp() {
        for (int i = 0; i < NAMES.length; i++) {
            String line;
            try {
                VerifyBody body = (VerifyBody) Class.forName(
                    "fplinux.fixtures.jit." + NAMES[i]).newInstance();
                line = "accepted " + body.f(0) + " " + body.f(1) + " " + body.f(7);
            } catch (Throwable t) {
                line = "rejected " + t;
            }
            System.out.println("PM-VERIFY " + NAMES[i] + " " + line);
        }
        notifyDestroyed();
    }

    protected void pauseApp() {
    }

    protected void destroyApp(boolean unconditional) {
    }
}
