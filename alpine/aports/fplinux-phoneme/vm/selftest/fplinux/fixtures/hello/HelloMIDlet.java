// SPDX-License-Identifier: GPL-2.0-only
package fplinux.fixtures.hello;

import javax.microedition.midlet.MIDlet;

/** Headless hello MIDlet: prints marker lines to System.out, no LCDUI. */
public class HelloMIDlet extends MIDlet {
    protected void startApp() {
        System.out.println("PM-HELLO start");
        System.out.println(getAppProperty("MIDlet-Name"));
        System.out.println(System.getProperty("microedition.configuration"));
        System.out.println(System.getProperty("microedition.profiles"));
        System.out.println("PM-HELLO done");
        notifyDestroyed();
    }

    protected void pauseApp() {
    }

    protected void destroyApp(boolean unconditional) {
        System.out.println("PM-HELLO destroy " + unconditional);
    }
}
