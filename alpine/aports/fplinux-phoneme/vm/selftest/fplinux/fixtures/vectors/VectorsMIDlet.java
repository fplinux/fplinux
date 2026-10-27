// SPDX-License-Identifier: GPL-2.0-only
package fplinux.fixtures.vectors;

import javax.microedition.midlet.MIDlet;

/** MIDlet entry point: prints the same vector lines as Vectors.main. */
public class VectorsMIDlet extends MIDlet {
    protected void startApp() {
        Vectors.run(System.out);
        notifyDestroyed();
    }

    protected void pauseApp() {
    }

    protected void destroyApp(boolean unconditional) {
    }
}
