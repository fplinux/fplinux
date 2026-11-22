// SPDX-License-Identifier: GPL-2.0-only
package fplinux.fixtures.jit;

import javax.microedition.midlet.MIDlet;

// Float.floatToIntBits, Float.intBitsToFloat, Double.doubleToLongBits and
// Double.longBitsToDouble in compiled code, on values from arrays, locals,
// parameters, folded constants, round trips and arithmetic results: signed
// zeros, subnormals, extremes, infinities, NaNs with several payloads (the
// conversions to bits return the canonical NaN) and random patterns.
// HotSpot prints the same line.
public final class JitFloatBitsMIDlet extends MIDlet {
    private static final int[] INTS = {
        0, 0x80000000, 1, 0x80000001, 0x007fffff, 0x807fffff, 0x00800000, 0x3f800000,
        0xbf800000, 0x7f7fffff, 0xff7fffff, 0x7f800000, 0xff800000, 0x7f800001,
        0x7fc00000, 0x7fc00001, 0x7fffffff, 0x7fa00000, 0xff800001, 0xffc00000,
        0xffffffff, 0xffa00000, 0x7f80ffff, 0xff80ffff, 0x40490fdb, 0xc0490fdb,
    };
    private static final long[] LONGS = {
        0L, 0x8000000000000000L, 1L, 0x8000000000000001L, 0x000fffffffffffffL,
        0x800fffffffffffffL, 0x0010000000000000L, 0x3ff0000000000000L, 0xbff0000000000000L,
        0x7fefffffffffffffL, 0xffefffffffffffffL, 0x7ff0000000000000L, 0xfff0000000000000L,
        0x7ff0000000000001L, 0x7ff8000000000000L, 0x7ff8000000000001L, 0x7fffffffffffffffL,
        0x7ff4000000000000L, 0xfff0000000000001L, 0xfff8000000000000L, 0xffffffffffffffffL,
        0x7ff00000ffffffffL, 0x7ff0000080000000L, 0x7ff0000100000000L, 0xfff00000ffffffffL,
        0x400921fb54442d18L, 0xc00921fb54442d18L,
    };
    private static int[] ints;
    private static long[] longs;
    private static float[] floats;
    private static double[] doubles;

    private static int floatBits(float f) {
        return Float.floatToIntBits(f);
    }

    private static long doubleBits(double d) {
        return Double.doubleToLongBits(d);
    }

    private static long floatFlows() {
        long hash = 17;
        for (int i = 0; i < ints.length; i++) {
            float a = floats[i];
            float b = floats[(i * 7 + 3) % floats.length];
            hash = hash * 31 + Float.floatToIntBits(floats[i]);
            hash = hash * 31 + floatBits(a);
            hash = hash * 31 + Float.floatToIntBits(-a) + Float.floatToIntBits(a);
            hash = hash * 31 + Float.floatToIntBits(Float.intBitsToFloat(ints[i]));
            hash = hash * 31 + Float.floatToIntBits(a + b) + Float.floatToIntBits(a * b);
            float c = Float.intBitsToFloat(ints[i]) * 2.0f + 1.0f;
            hash = hash * 31 + Float.floatToIntBits(c) + (c < 0.0f ? 1 : 0) + (c == c ? 2 : 0);
        }
        hash = hash * 31 + Float.floatToIntBits(Float.NaN) + Float.floatToIntBits(-0.0f);
        hash = hash * 31 + Float.floatToIntBits(Float.intBitsToFloat(0xffc00001));
        hash = hash * 31 + Float.floatToIntBits(Float.intBitsToFloat(0x7f800001));
        hash = hash * 31 + Float.floatToIntBits(Float.intBitsToFloat(0x00000001));
        return hash;
    }

    private static long doubleFlows() {
        long hash = 17;
        for (int i = 0; i < longs.length; i++) {
            double a = doubles[i];
            double b = doubles[(i * 7 + 3) % doubles.length];
            hash = hash * 31 + Double.doubleToLongBits(doubles[i]);
            hash = hash * 31 + doubleBits(a);
            hash = hash * 31 + Double.doubleToLongBits(-a) + Double.doubleToLongBits(a);
            hash = hash * 31 + Double.doubleToLongBits(Double.longBitsToDouble(longs[i]));
            hash = hash * 31 + Double.doubleToLongBits(a + b) + Double.doubleToLongBits(a * b);
            double c = Double.longBitsToDouble(longs[i]) * 2.0 + 1.0;
            hash = hash * 31 + Double.doubleToLongBits(c) + (c < 0.0 ? 1 : 0) + (c == c ? 2 : 0);
        }
        hash = hash * 31 + Double.doubleToLongBits(Double.NaN) + Double.doubleToLongBits(-0.0);
        hash = hash * 31 + Double.doubleToLongBits(Double.longBitsToDouble(0xfff8000000000001L));
        hash = hash * 31 + Double.doubleToLongBits(Double.longBitsToDouble(0x7ff0000000000001L));
        hash = hash * 31 + Double.doubleToLongBits(Double.longBitsToDouble(0x0000000000000001L));
        return hash;
    }

    protected void startApp() {
        long seed = 12345;
        ints = new int[INTS.length + 6];
        longs = new long[LONGS.length + 5];
        for (int i = 0; i < ints.length; i++) {
            seed = seed * 6364136223846793005L + 1442695040888963407L;
            ints[i] = i < INTS.length ? INTS[i] : (int) (seed >>> 32);
            longs[i] = i < LONGS.length ? LONGS[i] : seed;
        }
        floats = new float[ints.length];
        doubles = new double[longs.length];
        for (int i = 0; i < ints.length; i++) {
            floats[i] = Float.intBitsToFloat(ints[i]);
            doubles[i] = Double.longBitsToDouble(longs[i]);
        }
        long hash = 0;
        for (int call = 0; call < 3000; call++) {
            hash = hash * 31 + floatFlows();
            hash = hash * 31 + doubleFlows();
        }
        System.out.println("PM-JIT-FLOATBITS hash=" + hash);
        notifyDestroyed();
    }

    protected void pauseApp() {
    }

    protected void destroyApp(boolean unconditional) {
    }
}
