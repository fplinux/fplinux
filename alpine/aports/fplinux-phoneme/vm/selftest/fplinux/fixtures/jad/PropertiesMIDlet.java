/* SPDX-License-Identifier: GPL-2.0-only */
package fplinux.fixtures.jad;

import javax.microedition.midlet.MIDlet;

public final class PropertiesMIDlet extends MIDlet {
    protected void startApp() {
        System.out.println("PM-JAD custom=" + getAppProperty("Fixture-Custom"));
        System.out.println("PM-JAD override=" + getAppProperty("Fixture-Override"));
        System.out.println("PM-JAD manifest=" + getAppProperty("Fixture-Manifest-Only"));
        System.out.println("PM-JAD system=" + getAppProperty("microedition.profiles"));
        System.out.println("PM-JAD utf8=" + "\u0416".equals(getAppProperty("Fixture-UTF8")));
        notifyDestroyed();
    }

    protected void pauseApp() {}

    protected void destroyApp(boolean unconditional) {}
}
