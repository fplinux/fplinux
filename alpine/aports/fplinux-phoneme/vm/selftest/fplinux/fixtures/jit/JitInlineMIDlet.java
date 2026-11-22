// SPDX-License-Identifier: GPL-2.0-only
package fplinux.fixtures.jit;

import javax.microedition.midlet.MIDlet;

public final class JitInlineMIDlet extends MIDlet {
    private static final int ROUNDS = 20000;

    static int at(int[] array, int index) {
        return array[index];
    }

    // The caller must catch exceptions thrown by the inlined array access.
    private static void handler(int[] array) {
        int caught = 0, npe = 0, sum = 0;
        for (int i = 0; i < ROUNDS; i++) {
            try {
                sum += at((i % 7) == 0 ? null : array, i & 31);
            } catch (ArrayIndexOutOfBoundsException e) {
                caught++;
            } catch (NullPointerException e) {
                npe++;
            }
        }
        System.out.println("PM-JIT-INLINE handler caught=" + caught + " null=" + npe + " sum=" + sum);
    }

    protected void startApp() {
        int[] array = new int[16];
        for (int i = 0; i < array.length; i++) {
            array[i] = i * 3 - 7;
        }
        for (int pass = 0; pass < 3; pass++) {
            handler(array);
        }
        notifyDestroyed();
    }

    protected void pauseApp() {
    }

    protected void destroyApp(boolean unconditional) {
    }
}
