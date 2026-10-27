// SPDX-License-Identifier: GPL-2.0-only
package fplinux.fixtures.vectors;

import java.io.PrintStream;

/**
 * Deterministic CLDC 1.1 numeric vectors. Only CLDC 1.1 API is used so the
 * same class runs on HotSpot (oracle) and on CLDC-HI.
 * Output: one line per vector, "name bits human".
 */
public class Vectors {
    private static PrintStream out;

    public static void main(String[] args) {
        run(System.out);
    }

    private static void d(String n, double x) {
        out.println(n + " " + Long.toString(Double.doubleToLongBits(x), 16) + " " + Double.toString(x));
    }

    private static void f(String n, float x) {
        out.println(n + " " + Integer.toHexString(Float.floatToIntBits(x)) + " " + Float.toString(x));
    }

    private static void l(String n, long x) {
        out.println(n + " " + Long.toString(x, 16) + " " + x);
    }

    private static void i(String n, int x) {
        out.println(n + " " + Integer.toHexString(x) + " " + x);
    }

    private static void b(String n, boolean x) {
        out.println(n + " " + (x ? "true" : "false"));
    }

    private static void s(String n, String x) {
        out.println(n + " [" + x + "]");
    }

    private static double[] dv() {
        return new double[] {
            0.0, -0.0, 1.0, -1.0, 0.5, 2.5, -2.5, 3.0, 7.0, -7.0, 1.0 / 3.0, 1e10, 1e-10, 123456.789,
            Double.MAX_VALUE, Double.MIN_VALUE, 1.0 / 0.0, -1.0 / 0.0, 0.0 / 0.0
        };
    }

    private static float[] fv() {
        return new float[] {
            0.0f, -0.0f, 1.0f, -1.0f, 0.5f, 2.5f, -2.5f, 3.0f, 7.0f, -7.0f, 1.0f / 3.0f, 1e10f, 1e-10f,
            Float.MAX_VALUE, Float.MIN_VALUE, 1.0f / 0.0f, -1.0f / 0.0f, 0.0f / 0.0f
        };
    }

    public static void run(PrintStream o) {
        out = o;
        o.println("VECTORS begin");
        arith();
        rem();
        cmp();
        conv();
        math();
        text();
        ints();
        strings();
        stress();
        o.println("VECTORS end");
    }

    static void arith() {
        double[] a = { 1.0, 0.1, -3.5, 1e308, 1e-300, 3.0, 0.0, 1.0 / 0.0 };
        double[] c = { 3.0, 0.2, 2.0, 10.0, 1e10, 0.0, 0.0, -1.0 / 0.0 };
        for (int k = 0; k < a.length; k++) {
            d("dadd" + k, a[k] + c[k]);
            d("dsub" + k, a[k] - c[k]);
            d("dmul" + k, a[k] * c[k]);
            d("ddiv" + k, a[k] / c[k]);
            d("dneg" + k, -a[k]);
        }
        float[] fa = { 1.0f, 0.1f, -3.5f, 3e38f, 1e-30f, 3.0f, 0.0f, 1.0f / 0.0f };
        float[] fc = { 3.0f, 0.2f, 2.0f, 10.0f, 1e10f, 0.0f, 0.0f, -1.0f / 0.0f };
        for (int k = 0; k < fa.length; k++) {
            f("fadd" + k, fa[k] + fc[k]);
            f("fsub" + k, fa[k] - fc[k]);
            f("fmul" + k, fa[k] * fc[k]);
            f("fdiv" + k, fa[k] / fc[k]);
            f("fneg" + k, -fa[k]);
        }
        d("dsum", 0.1 + 0.2);
        d("dsum10", sum10());
        f("fsum10", fsum10());
    }

    static double sum10() {
        double t = 0.0;
        for (int k = 0; k < 10; k++) t += 0.1;
        return t;
    }

    static float fsum10() {
        float t = 0.0f;
        for (int k = 0; k < 10; k++) t += 0.1f;
        return t;
    }

    static void rem() {
        double[] a = { 5.5, -5.5, 5.5, -5.5, 1e300, 0.0, -0.0, 7.0, 7.0, 1.0 / 0.0, -1.0 / 0.0, 0.0 / 0.0, 3.0, 3.0, 0.3 };
        double[] c = { 2.0, 2.0, -2.0, -2.0, 7.0, 3.0, 3.0, 0.0, 1.0 / 0.0, 3.0, 3.0, 3.0, 0.0 / 0.0, -1.0 / 0.0, 0.1 };
        for (int k = 0; k < a.length; k++) d("drem" + k, a[k] % c[k]);
        float[] fa = { 5.5f, -5.5f, 5.5f, -5.5f, 1e30f, 0.0f, -0.0f, 7.0f, 7.0f, 1.0f / 0.0f, 0.0f / 0.0f, 3.0f, 0.3f };
        float[] fc = { 2.0f, 2.0f, -2.0f, -2.0f, 7.0f, 3.0f, 3.0f, 0.0f, 1.0f / 0.0f, 3.0f, 3.0f, 0.0f / 0.0f, 0.1f };
        for (int k = 0; k < fa.length; k++) f("frem" + k, fa[k] % fc[k]);
    }

    static void cmp() {
        double[] v = dv();
        int[] idx = { 0, 1, 2, 3, 14, 16, 17, 18 };
        for (int p = 0; p < idx.length; p++) {
            for (int q = 0; q < idx.length; q++) {
                double x = v[idx[p]], y = v[idx[q]];
                out.println("dcmp" + p + "_" + q + " " + (x < y ? 1 : 0) + (x <= y ? 1 : 0) + (x > y ? 1 : 0)
                    + (x >= y ? 1 : 0) + (x == y ? 1 : 0) + (x != y ? 1 : 0));
            }
        }
        float[] w = fv();
        int[] fi = { 0, 1, 2, 3, 14, 15, 16, 17 };
        for (int p = 0; p < fi.length; p++) {
            for (int q = 0; q < fi.length; q++) {
                float x = w[fi[p]], y = w[fi[q]];
                out.println("fcmp" + p + "_" + q + " " + (x < y ? 1 : 0) + (x <= y ? 1 : 0) + (x > y ? 1 : 0)
                    + (x >= y ? 1 : 0) + (x == y ? 1 : 0) + (x != y ? 1 : 0));
            }
        }
        double nan = 0.0 / 0.0;
        b("nan_self_eq", nan == nan);
        b("nan_self_ne", nan != nan);
        b("zero_eq_negzero", 0.0 == -0.0);
        b("zero_bits_eq", Double.doubleToLongBits(0.0) == Double.doubleToLongBits(-0.0));
        b("isNaN", Double.isNaN(nan));
        b("isInfinite", Double.isInfinite(1.0 / 0.0));
        b("fisNaN", Float.isNaN(0.0f / 0.0f));
        l("nanbits", Double.doubleToLongBits(nan));
        i("fnanbits", Float.floatToIntBits(0.0f / 0.0f));
        b("dequals_nan", new Double(nan).equals(new Double(nan)));
        b("dequals_zero", new Double(0.0).equals(new Double(-0.0)));
    }

    static void conv() {
        double[] dd = { 0.0, -0.0, 0.9, -0.9, 1.5, -1.5, 2147483647.0, 2147483648.0, -2147483648.0, -2147483649.0,
            1e10, -1e10, 9.223372036854775807E18, -9.223372036854775808E18, 1e19, -1e19, 1.0 / 0.0, -1.0 / 0.0,
            0.0 / 0.0, Double.MAX_VALUE };
        for (int k = 0; k < dd.length; k++) {
            i("d2i" + k, (int) dd[k]);
            l("d2l" + k, (long) dd[k]);
            f("d2f" + k, (float) dd[k]);
        }
        float[] ff = { 0.0f, 0.9f, -0.9f, 1.5f, -1.5f, 2147483648.0f, -2147483648.0f, 3e9f, -3e9f, 9.223372E18f,
            -9.223372E18f, 1e19f, -1e19f, 1.0f / 0.0f, -1.0f / 0.0f, 0.0f / 0.0f, Float.MAX_VALUE };
        for (int k = 0; k < ff.length; k++) {
            i("f2i" + k, (int) ff[k]);
            l("f2l" + k, (long) ff[k]);
            d("f2d" + k, (double) ff[k]);
        }
        int[] iv = { 0, 1, -1, 16777216, 16777217, 16777219, -16777217, 2147483647, -2147483648, 1234567891 };
        for (int k = 0; k < iv.length; k++) {
            f("i2f" + k, (float) iv[k]);
            d("i2d" + k, (double) iv[k]);
        }
        long[] lv = { 0L, 1L, -1L, 9007199254740992L, 9007199254740993L, 9007199254740995L, -9007199254740993L,
            9223372036854775807L, -9223372036854775808L, 16777217L, 0x7fffff8000000001L, 0x7fffffbfffffffffL,
            123456789012345678L };
        for (int k = 0; k < lv.length; k++) {
            d("l2d" + k, (double) lv[k]);
            f("l2f" + k, (float) lv[k]);
            i("l2i" + k, (int) lv[k]);
        }
        i("i2b", (byte) 200);
        i("i2s", (short) 70000);
        i("i2c", (char) -1);
    }

    static void math() {
        double[] x = { 0.0, -0.0, 0.5, 1.0, -1.0, 2.0, 3.0, 10.0, 100.0, 1e-5, 0.7853981633974483, 1.5707963267948966,
            3.141592653589793, -3.141592653589793, 6.283185307179586, 1e6, 123456.789, 1e22, 1.0 / 0.0, -1.0 / 0.0,
            0.0 / 0.0 };
        for (int k = 0; k < x.length; k++) {
            d("sqrt" + k, Math.sqrt(x[k]));
            d("sin" + k, Math.sin(x[k]));
            d("cos" + k, Math.cos(x[k]));
            d("tan" + k, Math.tan(x[k]));
            d("abs" + k, Math.abs(x[k]));
        }
        double[] r = { 0.0, -0.0, 0.4, 0.5, 0.6, 1.0, -0.4, -0.5, -0.6, -1.0, 2.5, -2.5, 4503599627370496.0, 1e300,
            1.0 / 0.0, -1.0 / 0.0, 0.0 / 0.0 };
        for (int k = 0; k < r.length; k++) {
            d("floor" + k, Math.floor(r[k]));
            d("ceil" + k, Math.ceil(r[k]));
        }
        double[] m1 = { 0.0, -0.0, 1.0, -1.0, 0.0 / 0.0, 1.0 };
        double[] m2 = { -0.0, 0.0, 2.0, -2.0, 1.0, 0.0 / 0.0 };
        for (int k = 0; k < m1.length; k++) {
            d("min" + k, Math.min(m1[k], m2[k]));
            d("max" + k, Math.max(m1[k], m2[k]));
        }
        f("fabs", Math.abs(-2.5f));
        f("fabs_negzero", Math.abs(-0.0f));
        f("fmin0", Math.min(0.0f, -0.0f));
        f("fmax0", Math.max(-0.0f, 0.0f));
        f("fmin_nan", Math.min(0.0f / 0.0f, 1.0f));
        f("fmax_nan", Math.max(1.0f, 0.0f / 0.0f));
        i("iabs_min", Math.abs(Integer.MIN_VALUE));
        l("labs_min", Math.abs(Long.MIN_VALUE));
        i("imin", Math.min(-3, 2));
        i("imax", Math.max(-3, 2));
        l("lmin", Math.min(-3L, 2L));
        l("lmax", Math.max(-3L, 2L));
        d("toRadians180", Math.toRadians(180.0));
        d("toDegrees_pi", Math.toDegrees(Math.PI));
        d("pi", Math.PI);
        d("e", Math.E);
    }

    static void text() {
        double[] dd = { 0.0, -0.0, 1.0, -1.0, 0.1, 0.5, 100.0, 1234567.0, 1e7, 1e-3, 1e-4, 9999999.0, 10000000.0,
            123456789.0, 1.0 / 3.0, 2.0 / 3.0, 1e21, 1e-5, 4.9e-324, Double.MAX_VALUE, 1.0 / 0.0, -1.0 / 0.0, 0.0 / 0.0,
            0.1 + 0.2, 100.0 / 3.0, 1e100 };
        for (int k = 0; k < dd.length; k++) {
            String t = Double.toString(dd[k]);
            out.println("dstr" + k + " " + t);
            try {
                double back = Double.parseDouble(t);
                out.println("dtrip" + k + " " + Long.toString(Double.doubleToLongBits(back), 16));
            } catch (NumberFormatException e) {
                out.println("dtrip" + k + " NFE");
            }
        }
        float[] ff = { 0.0f, -0.0f, 1.0f, -1.0f, 0.1f, 0.5f, 100.0f, 1234567.0f, 1e7f, 1e-3f, 1e-4f, 1.0f / 3.0f,
            2.0f / 3.0f, 1e10f, 1e-5f, 1.4e-45f, Float.MAX_VALUE, 1.0f / 0.0f, 0.0f / 0.0f, 16777216.0f, 0.1f + 0.2f };
        for (int k = 0; k < ff.length; k++) {
            String t = Float.toString(ff[k]);
            out.println("fstr" + k + " " + t);
            try {
                float back = Float.parseFloat(t);
                out.println("ftrip" + k + " " + Integer.toHexString(Float.floatToIntBits(back)));
            } catch (NumberFormatException e) {
                out.println("ftrip" + k + " NFE");
            }
        }
        String[] ps = { "0", "-0", "1", "1.5", "-2.25e3", "1e308", "1e309", "1e-323", "1e-400", "4.9e-324",
            "2.2250738585072014E-308", "2.2250738585072011E-308", "0.1", "123456789012345678901234567890",
            "9007199254740993", "NaN", "Infinity", "-Infinity", "  12.5  ", "1.7976931348623157e308",
            "1.7976931348623159e308", ".5", "5.", "1d", "1f", "0x10", "", "abc" };
        for (int k = 0; k < ps.length; k++) {
            try {
                d("pd" + k, Double.parseDouble(ps[k]));
            } catch (NumberFormatException e) {
                out.println("pd" + k + " NFE");
            }
            try {
                f("pf" + k, Float.parseFloat(ps[k]));
            } catch (NumberFormatException e) {
                out.println("pf" + k + " NFE");
            }
        }
        l("bits1", Double.doubleToLongBits(1.0));
        d("frombits", Double.longBitsToDouble(0x400921fb54442d18L));
        f("ffrombits", Float.intBitsToFloat(0x40490fdb));
        d("fromnanbits", Double.longBitsToDouble(0x7ff8000000000001L));
        l("nanbits2", Double.doubleToLongBits(Double.longBitsToDouble(0x7ff8000000000001L)));
        i("fnanbits2", Float.floatToIntBits(Float.intBitsToFloat(0x7fc00001)));
        d("valueOf", Double.valueOf("2.5").doubleValue());
        f("fvalueOf", Float.valueOf("2.5").floatValue());
        d("dMax", Double.MAX_VALUE);
        d("dMin", Double.MIN_VALUE);
        f("fMax", Float.MAX_VALUE);
        f("fMin", Float.MIN_VALUE);
    }

    static void ints() {
        int mn = Integer.MIN_VALUE;
        int m1 = -1;
        i("imin_div", mn / m1);
        i("imin_rem", mn % m1);
        i("idiv1", -7 / 2);
        i("irem1", -7 % 2);
        i("idiv2", 7 / -2);
        i("irem2", 7 % -2);
        long lmn = Long.MIN_VALUE;
        long lm1 = -1L;
        l("lmin_div", lmn / lm1);
        l("lmin_rem", lmn % lm1);
        l("ldiv1", -7L / 2L);
        l("lrem1", -7L % 2L);
        l("ldiv_big", 123456789012345678L / 1000003L);
        l("lrem_big", 123456789012345678L % 1000003L);
        try {
            int z = 0;
            i("idiv0", 1 / z);
        } catch (ArithmeticException e) {
            out.println("idiv0 ArithmeticException");
        }
        try {
            long z = 0L;
            l("lrem0", 1L % z);
        } catch (ArithmeticException e) {
            out.println("lrem0 ArithmeticException");
        }
        long v = 0x123456789abcdef0L;
        int[] sh = { 0, 1, 4, 31, 32, 33, 63, 64, 65, -1 };
        for (int k = 0; k < sh.length; k++) {
            l("lshl" + k, v << sh[k]);
            l("lshr" + k, v >> sh[k]);
            l("lushr" + k, v >>> sh[k]);
            l("nlshr" + k, -v >> sh[k]);
            l("nlushr" + k, -v >>> sh[k]);
            i("ishl" + k, 0x89abcdef << sh[k]);
            i("ishr" + k, 0x89abcdef >> sh[k]);
            i("iushr" + k, 0x89abcdef >>> sh[k]);
        }
        l("lmul", 0x7fffffffffffffffL * 3L);
        l("lmul2", 123456789L * 987654321L);
        i("imul", 0x7fffffff * 3);
        i("iadd_ovf", Integer.MAX_VALUE + 1);
        l("ladd_ovf", Long.MAX_VALUE + 1L);
        l("lneg_min", -Long.MIN_VALUE);
        l("land", 0xff00ff00ff00ff00L & 0x0ff00ff00ff00ff0L);
        l("lor", 0xff00ff00ff00ff00L | 0x0ff00ff00ff00ff0L);
        l("lxor", 0xff00ff00ff00ff00L ^ 0x0ff00ff00ff00ff0L);
        l("lnot", ~0x0123456789abcdefL);
        b("lcmp1", 5L < 7L);
        b("lcmp2", -5L < 7L);
        b("lcmp3", Long.MIN_VALUE < Long.MAX_VALUE);
        b("lcmp4", 0x8000000000000000L > 1L);
        i("parseInt", Integer.parseInt("-2147483648"));
        l("parseLong", Long.parseLong("-9223372036854775808"));
        s("itoa", Integer.toString(Integer.MIN_VALUE));
        s("ltoa", Long.toString(Long.MIN_VALUE));
        s("ltoa36", Long.toString(Long.MAX_VALUE, 36));
        s("ihex", Integer.toHexString(-1));
    }

    static void strings() {
        String a = "abc";
        s("cat1", a + 1 + 2L + 'x' + true + 1.5f + 2.5 + null);
        s("cat2", 1 + 2 + a + 1 + 2);
        s("cat3", "" + (char) 65 + (int) 'A' + (byte) -1);
        s("cat4", a + 0.0 + -0.0 + 1.0f / 0.0f);
        StringBuffer sb = new StringBuffer();
        for (int k = 0; k < 20; k++) sb.append(k).append(',');
        sb.append(1.25).append(' ').append(2.5f).append(' ').append(true).append(' ').append('c').append(' ').append(99L);
        s("sb1", sb.toString());
        i("sb1len", sb.length());
        sb.insert(0, "<<").append(">>");
        s("sb2", sb.toString());
        sb.setLength(5);
        s("sb3", sb.toString());
        sb.setCharAt(0, 'Z');
        sb.deleteCharAt(1);
        s("sb4", sb.toString());
        sb.delete(0, 2);
        s("sb5", sb.toString());
        String u = "Hello, World";
        s("upper", u.toUpperCase());
        s("lower", u.toLowerCase());
        i("indexOf", u.indexOf("World"));
        i("lastIndexOf", u.lastIndexOf('o'));
        s("substr", u.substring(3, 8));
        s("replace", u.replace('l', 'L'));
        s("trim", "  x y  ".trim());
        i("cmp", "apple".compareTo("apricot"));
        b("eq", u.equals("Hello, World"));
        i("hash", u.hashCode());
        i("hash_empty", "".hashCode());
        b("startsWith", u.startsWith("Hell"));
        b("endsWith", u.endsWith("ld"));
        s("valueOf", String.valueOf(3.25));
        String uni = new String(new char[] { 'a', 'é', '中', 'z' });
        i("uni_len", uni.length());
        i("uni_c1", uni.charAt(1));
        i("uni_c2", uni.charAt(2));
        i("uni_hash", uni.hashCode());
    }

    static class Node {
        int v;
        Node next;
        double[] data;

        Node(int v, Node n) {
            this.v = v;
            next = n;
            data = new double[8];
            data[v & 7] = v * 0.5;
        }
    }

    static void stress() {
        Node head = null;
        long checksum = 0L;
        for (int round = 0; round < 200; round++) {
            Node h = null;
            for (int k = 0; k < 200; k++) h = new Node(round * 1000 + k, h);
            long sum = 0L;
            for (Node n = h; n != null; n = n.next) sum += n.v + (long) (n.data[n.v & 7] * 2.0);
            checksum = checksum * 31L + sum;
            if (round % 10 == 0) head = new Node(round, head);
            int[] big = new int[1000 + round];
            for (int k = 0; k < big.length; k++) big[k] = k * round;
            checksum ^= big[big.length - 1];
            String t = "r" + round;
            checksum += t.length();
        }
        l("gc_checksum", checksum);
        int cnt = 0;
        for (Node n = head; n != null; n = n.next) cnt++;
        i("gc_live", cnt);
        Object[][] grid = new Object[50][];
        for (int k = 0; k < 50; k++) {
            grid[k] = new Object[k + 1];
            for (int j = 0; j <= k; j++) grid[k][j] = new Integer(k * j);
        }
        long gs = 0L;
        for (int k = 0; k < 50; k++)
            for (int j = 0; j <= k; j++) gs += ((Integer) grid[k][j]).intValue();
        l("grid_sum", gs);
        try {
            Object[] oa = new String[1];
            oa[0] = new Integer(1);
            out.println("ase none");
        } catch (ArrayStoreException e) {
            out.println("ase caught");
        }
        try {
            int[] ia = new int[2];
            ia[2] = 1;
        } catch (ArrayIndexOutOfBoundsException e) {
            out.println("aioobe caught");
        }
        try {
            int[] ia = new int[-1];
            out.println("nase none " + ia.length);
        } catch (NegativeArraySizeException e) {
            out.println("nase caught");
        }
        try {
            Object o = "x";
            Integer bad = (Integer) o;
            out.println("cce none " + bad);
        } catch (ClassCastException e) {
            out.println("cce caught");
        }
    }
}
