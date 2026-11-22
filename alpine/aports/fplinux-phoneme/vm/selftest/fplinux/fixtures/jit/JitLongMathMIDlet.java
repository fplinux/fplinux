// SPDX-License-Identifier: GPL-2.0-only
package fplinux.fixtures.jit;

import javax.microedition.midlet.MIDlet;

// Long arithmetic with constant operands in compiled code: multiplication by
// constants whose high word is 0 or -1, by powers of two, 1 and -1; division
// and remainder by 1, -1, powers of two, 16 bit, 32 bit and wider divisors,
// including MIN_VALUE / -1 and MIN_VALUE % -1; masks that change no word.
// HotSpot prints the same line.
public final class JitLongMathMIDlet extends MIDlet {
    private static final long[] VALUES = {
        0L, 1L, -1L, 2L, -3L, 10000L, -10000L, 65537L, 0x7fffffffL, 0x80000000L,
        0xffffffffL, 0x100000000L, -0x80000001L, 0x123456789L, -0x123456789L,
        0x7fffffff80000000L, Long.MAX_VALUE, Long.MAX_VALUE - 1, Long.MIN_VALUE,
        Long.MIN_VALUE + 1, 0x0123456789abcdefL, 0xfedcba9876543210L, -123456789012345L,
        Integer.MAX_VALUE, Integer.MIN_VALUE,
    };

    private static long multiply(long[] values) {
        long hash = 17;
        for (int i = 0; i < values.length; i++) {
            long a = values[i];
            hash = hash * 31 + a * 10000L;
            hash = hash * 31 + a * -10000L;
            hash = hash * 31 + a * 0xffffffffL;
            hash = hash * 31 + a * -4294967295L;
            hash = hash * 31 + a * -2147483648L;
            hash = hash * 31 + 3L * a;
            hash = hash * 31 + a * 65536L;
            hash = hash * 31 + a * 0x80000000L;
            hash = hash * 31 + a * 0x100000000L;
            hash = hash * 31 + a * 0x4000000000000000L;
            hash = hash * 31 + a * 1L;
            hash = hash * 31 + a * -1L;
            hash = hash * 31 + ((a * 10000L) & 0xffffffffffL);
            hash = hash * 31 + (a & -1L) + (a | 0L) + (a ^ 0L);
        }
        return hash;
    }

    private static long divideByPowersOfTwo(long[] values) {
        long hash = 17;
        for (int i = 0; i < values.length; i++) {
            long a = values[i];
            hash = hash * 31 + a / 1L + a % 1L + a / -1L + a % -1L;
            hash = hash * 31 + a / 2L + a % 2L;
            hash = hash * 31 + a / 32L + a % 32L;
            hash = hash * 31 + a / 0x80000000L + a % 0x80000000L;
            hash = hash * 31 + a / 0x100000000L + a % 0x100000000L;
            hash = hash * 31 + a / 0x200000000L + a % 0x200000000L;
            hash = hash * 31 + a / 0x4000000000000000L + a % 0x4000000000000000L;
        }
        return hash;
    }

    private static long divideByOthers(long[] values) {
        long hash = 17;
        for (int i = 0; i < values.length; i++) {
            long a = values[i];
            hash = hash * 31 + a / 10000L + a % 10000L;
            hash = hash * 31 + a / -10000L + a % -10000L;
            hash = hash * 31 + a / 1000000007L + a % 1000000007L;
            hash = hash * 31 + a / 4294967295L + a % 4294967295L;
            hash = hash * 31 + a / -2147483648L + a % -2147483648L;
            hash = hash * 31 + a / 0x123456789L + a % 0x123456789L;
            hash = hash * 31 + a / Long.MIN_VALUE + a % Long.MIN_VALUE;
            long b = values[(i * 7 + 3) % values.length];
            if (b == 0) {
                b = -1;
            }
            hash = hash * 31 + a / b + a % b;
        }
        return hash;
    }

    protected void startApp() {
        long hash = 0;
        for (int call = 0; call < 20000; call++) {
            hash = hash * 31 + multiply(VALUES);
            hash = hash * 31 + divideByPowersOfTwo(VALUES);
            hash = hash * 31 + divideByOthers(VALUES);
        }
        System.out.println("PM-JIT-LONG hash=" + hash);
        notifyDestroyed();
    }

    protected void pauseApp() {
    }

    protected void destroyApp(boolean unconditional) {
    }
}
