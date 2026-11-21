// SPDX-License-Identifier: GPL-2.0-only
package fplinux.fixtures.jit;

import javax.microedition.midlet.MIDlet;

public final class JitMIDlet extends MIDlet {
    private static int hotLoop(int value, int offset) {
        for (int i = 0; i < 2048; i++) {
            value = (value ^ (offset + i)) * 1664525 + 1013904223;
            value ^= value >>> 16;
        }
        return value;
    }

    private static int countWrongRemainders(int first) {
        int errors = 0;
        for (int i = first; i < first + 64; i++) {
            float single = (float)i * 1.25f;
            double wide = (double)i * 0.125 + 0.1;
            if (single % single != 0.0f) {
                errors++;
            }
            if (wide % wide != 0.0) {
                errors++;
            }
            float singleLeft = (float)(i * 4 + 3);
            float singleRight = (float)(i * 4 + 1);
            double wideLeft = (double)(i * 4 + 3);
            double wideRight = (double)(i * 4 + 1);
            if (singleLeft % singleRight != 2.0f ||
                    singleRight % singleLeft != singleRight) {
                errors++;
            }
            if (wideLeft % wideRight != 2.0 ||
                    wideRight % wideLeft != wideRight) {
                errors++;
            }
        }
        return errors;
    }

    // The return expression repeats the multiply-add stored in last. The
    // method has no branch, so the compiler always enters it at bci 0, and it
    // is too large to be inlined into the caller.
    private static int regionWidth(int offset, int rows, int scan, int width) {
        int last = offset + (rows - 1) * scan + width;
        return last - (offset + (rows - 1) * scan);
    }

    protected void startApp() {
        int value = 0x13579bdf;
        for (int i = 0; i < 1024; i++) {
            value = hotLoop(value, i);
        }

        long started = System.currentTimeMillis();
        for (int i = 1024; i < 5120; i++) {
            value = hotLoop(value, i);
        }
        long elapsed = System.currentTimeMillis() - started;
        System.out.println("PM-JIT result=" + value);
        System.out.println("PM-JIT elapsed-ms=" + elapsed);

        int wrongRemainders = 0;
        for (int i = 1; i <= 1024; i++) {
            wrongRemainders += countWrongRemainders(i);
        }
        System.out.println("PM-JIT wrong-remainders=" + wrongRemainders);

        int regionWidths = 0;
        for (int round = 0; round < 1024; round++) {
            for (int offset = -8; offset <= 8; offset++) {
                for (int rows = 1; rows <= 3; rows++) {
                    regionWidths += regionWidth(offset, rows, 5, 3);
                    regionWidths += regionWidth(offset, rows, -5, 3);
                }
            }
        }
        System.out.println("PM-JIT region-widths=" + regionWidths);
        notifyDestroyed();
    }

    protected void pauseApp() {
    }

    protected void destroyApp(boolean unconditional) {
    }
}
