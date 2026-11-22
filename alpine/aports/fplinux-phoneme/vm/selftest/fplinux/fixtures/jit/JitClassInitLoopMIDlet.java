// SPDX-License-Identifier: GPL-2.0-only
package fplinux.fixtures.jit;

import javax.microedition.midlet.MIDlet;

public final class JitClassInitLoopMIDlet extends MIDlet {
    protected void startApp() {
        System.out.println("PM-JIT-CLINIT-LOOP total=" + ClassInitLoop.run());
        notifyDestroyed();
    }

    protected void pauseApp() {
    }

    protected void destroyApp(boolean unconditional) {
    }
}
