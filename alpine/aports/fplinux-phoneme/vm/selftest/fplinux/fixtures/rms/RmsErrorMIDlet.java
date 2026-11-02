// SPDX-License-Identifier: GPL-2.0-only
package fplinux.fixtures.rms;

import javax.microedition.midlet.MIDlet;
import javax.microedition.rms.RecordStore;
import javax.microedition.rms.RecordStoreException;

/** An unusable storage path must report a RecordStoreException to the MIDlet. */
public class RmsErrorMIDlet extends MIDlet {
    protected void startApp() {
        try {
            RecordStore store = RecordStore.openRecordStore("probe", true);
            store.closeRecordStore();
            System.out.println("PM-RMS unexpectedly opened storage");
        } catch (RecordStoreException expected) {
            System.out.println("PM-RMS storage-error");
        }
        notifyDestroyed();
    }

    protected void pauseApp() {}

    protected void destroyApp(boolean unconditional) {}
}
