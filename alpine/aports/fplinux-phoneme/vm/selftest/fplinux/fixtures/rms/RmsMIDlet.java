// SPDX-License-Identifier: GPL-2.0-only
package fplinux.fixtures.rms;

import javax.microedition.midlet.MIDlet;
import javax.microedition.rms.RecordStore;

/** Writes records on the first run and reads them in the next VM process. */
public class RmsMIDlet extends MIDlet {
    protected void startApp() {
        try {
            RecordStore store = RecordStore.openRecordStore("probe", true);
            String result;
            if (store.getNumRecords() == 0) {
                byte[] first = { 0, 1, 127, -128, -1 };
                byte[] second = { 42, 0, 17 };
                int firstId = store.addRecord(first, 0, first.length);
                int secondId = store.addRecord(second, 0, second.length);
                result = "PM-RMS write " + firstId + " " + secondId;
            } else {
                byte[] first = store.getRecord(1);
                byte[] second = store.getRecord(2);
                if (store.getNumRecords() != 2 || first.length != 5
                    || first[0] != 0 || first[1] != 1 || first[2] != 127
                    || first[3] != -128 || first[4] != -1
                    || second.length != 3 || second[0] != 42
                    || second[1] != 0 || second[2] != 17) {
                    throw new Exception("record contents differ");
                }
                result = "PM-RMS read 1 2 next " + store.getNextRecordID();
            }
            store.closeRecordStore();
            System.out.println(result);
        } catch (Exception e) {
            System.out.println("PM-RMS error " + e);
        }
        notifyDestroyed();
    }

    protected void pauseApp() {}

    protected void destroyApp(boolean unconditional) {}
}
