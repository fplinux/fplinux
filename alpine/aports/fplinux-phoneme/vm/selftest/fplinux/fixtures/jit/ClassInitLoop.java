// SPDX-License-Identifier: GPL-2.0-only
package fplinux.fixtures.jit;

// The static initializer of Spin calls its own static methods thousands of
// times, so they become hot while the class is still being initialized. The
// compiled code reaches a call, a static field access or an allocation that
// needs the initialization of the class to finish, and every such method used
// to be compiled again after each call.
public final class ClassInitLoop {
    static final class Spin {
        static int total;
        static int made;
        static int shift = 3;
        int id;

        static {
            for (int i = 0; i < ClassInitLoop.ROUNDS; i++) {
                total += call(i) + field(i) + create(i);
            }
        }

        private static int mix(int value) {
            return (value * 31 + 7) ^ (value >>> shift);
        }

        // Calls a static method of the class that is being initialized.
        static int call(int i) {
            int result = mix(i);
            for (int k = 0; k < 4; k++) {
                result += mix(result) & 15;
            }
            return result & 0xff;
        }

        // Reads and writes a static field of it.
        static int field(int i) {
            made += i & 1;
            return made & 0xf;
        }

        // Allocates an instance of it.
        static int create(int i) {
            Spin spin = new Spin();
            spin.id = i;
            return spin.id & 3;
        }
    }

    static final int ROUNDS = 30000;

    // Returns the checksum the initializer computed; HotSpot gives the same.
    public static int run() {
        return Spin.total * 31 + Spin.made;
    }

    public static void main(String[] args) {
        System.out.println("PM-JIT-CLINIT-LOOP total=" + run());
    }
}
