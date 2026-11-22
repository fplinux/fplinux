// SPDX-License-Identifier: GPL-2.0-only
package javax.bluetooth;

/**
 * Immutable 128-bit Bluetooth UUID. A 16-bit or 32-bit value stands for the
 * 128-bit UUID that the Bluetooth Base UUID
 * 00000000-0000-1000-8000-00805F9B34FB gives with that value in its first
 * 32 bits.
 */
public class UUID {
    private static final long BASE_HIGH = 0x1000L;
    private static final long BASE_LOW = 0x800000805F9B34FBL;
    private static final String DIGITS = "0123456789ABCDEF";

    private final long high;
    private final long low;

    /** A 16-bit or 32-bit UUID; the value must be from 0 to 0xFFFFFFFF. */
    public UUID(long uuidValue) {
        if (uuidValue < 0 || uuidValue > 0xFFFFFFFFL) {
            throw new IllegalArgumentException("UUID value out of range");
        }
        high = (uuidValue << 32) | BASE_HIGH;
        low = BASE_LOW;
    }

    /**
     * Parses hexadecimal digits without a prefix or separators. A short UUID
     * has 1 to 8 digits, a 128-bit UUID 1 to 32; fewer digits than that fill
     * the low end of the value.
     */
    public UUID(String uuidValue, boolean shortUUID) {
        if (uuidValue == null) {
            throw new NullPointerException("UUID value");
        }
        int length = uuidValue.length();
        if (length == 0 || length > (shortUUID ? 8 : 32)) {
            throw new IllegalArgumentException("UUID length");
        }
        long upper = 0;
        long lower = 0;
        for (int i = 0; i < length; i++) {
            int digit = hexDigit(uuidValue.charAt(i));
            if (digit < 0) {
                throw new NumberFormatException("UUID digit");
            }
            upper = (upper << 4) | (lower >>> 60);
            lower = (lower << 4) | digit;
        }
        if (shortUUID) {
            high = (lower << 32) | BASE_HIGH;
            low = BASE_LOW;
        } else {
            high = upper;
            low = lower;
        }
    }

    private static int hexDigit(char c) {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'A' && c <= 'F') {
            return c - 'A' + 10;
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        return -1;
    }

    /** Uppercase hexadecimal representation of the full UUID without leading zeros. */
    public String toString() {
        StringBuffer text = new StringBuffer(32);
        for (int i = 31; i >= 0; i--) {
            long word = i >= 16 ? high : low;
            int digit = (int) (word >>> ((i & 15) * 4)) & 15;
            if (digit != 0 || text.length() != 0 || i == 0) {
                text.append(DIGITS.charAt(digit));
            }
        }
        return text.toString();
    }

    /** True for a UUID whose 128 bits are the same. */
    public boolean equals(Object value) {
        if (!(value instanceof UUID)) {
            return false;
        }
        UUID other = (UUID) value;
        return high == other.high && low == other.low;
    }

    public int hashCode() {
        long mixed = high ^ low;
        return (int) (mixed ^ (mixed >>> 32));
    }
}
