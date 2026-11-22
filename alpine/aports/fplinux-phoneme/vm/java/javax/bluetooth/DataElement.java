// SPDX-License-Identifier: GPL-2.0-only
package javax.bluetooth;

import java.util.Vector;

/**
 * Typed value of a service record attribute: a number, a boolean, a string,
 * a UUID, a URL or a sequence or alternative of other elements.
 */
public class DataElement {
    public static final int NULL = 0x00;
    public static final int U_INT_1 = 0x08;
    public static final int U_INT_2 = 0x09;
    public static final int U_INT_4 = 0x0A;
    public static final int U_INT_8 = 0x0B;
    public static final int U_INT_16 = 0x0C;
    public static final int INT_1 = 0x10;
    public static final int INT_2 = 0x11;
    public static final int INT_4 = 0x12;
    public static final int INT_8 = 0x13;
    public static final int INT_16 = 0x14;
    public static final int UUID = 0x18;
    public static final int STRING = 0x20;
    public static final int BOOL = 0x28;
    public static final int DATSEQ = 0x30;
    public static final int DATALT = 0x38;
    public static final int URL = 0x40;

    private final int type;
    private long number;
    private boolean flag;
    // A String, a javax.bluetooth.UUID, a byte array or, for DATSEQ and DATALT,
    // a Vector of DataElement.
    private Object value;

    /** NULL, or an empty DATSEQ or DATALT. */
    public DataElement(int valueType) {
        if (valueType != NULL && valueType != DATSEQ && valueType != DATALT) {
            throw new IllegalArgumentException("data element type");
        }
        type = valueType;
        if (valueType != NULL) {
            value = new Vector();
        }
    }

    public DataElement(boolean bool) {
        type = BOOL;
        flag = bool;
    }

    /** An integer of at most 64 bits: U_INT_1, 2 or 4 or INT_1, 2, 4 or 8. */
    public DataElement(int valueType, long valueNumber) {
        long minimum;
        long maximum;
        switch (valueType) {
        case U_INT_1:
            minimum = 0;
            maximum = 0xFFL;
            break;
        case U_INT_2:
            minimum = 0;
            maximum = 0xFFFFL;
            break;
        case U_INT_4:
            minimum = 0;
            maximum = 0xFFFFFFFFL;
            break;
        case INT_1:
            minimum = -0x80L;
            maximum = 0x7FL;
            break;
        case INT_2:
            minimum = -0x8000L;
            maximum = 0x7FFFL;
            break;
        case INT_4:
            minimum = -0x80000000L;
            maximum = 0x7FFFFFFFL;
            break;
        case INT_8:
            minimum = Long.MIN_VALUE;
            maximum = Long.MAX_VALUE;
            break;
        default:
            throw new IllegalArgumentException("data element type");
        }
        if (valueNumber < minimum || valueNumber > maximum) {
            throw new IllegalArgumentException("data element value");
        }
        type = valueType;
        number = valueNumber;
    }

    /**
     * A UUID (a javax.bluetooth.UUID), a STRING or URL (a String), or a wide
     * integer: U_INT_8 (a byte array of 8 bytes), U_INT_16 or INT_16 (16 bytes).
     */
    public DataElement(int valueType, Object valueObject) {
        switch (valueType) {
        case UUID:
            if (!(valueObject instanceof javax.bluetooth.UUID)) {
                throw new IllegalArgumentException("data element value");
            }
            break;
        case STRING:
        case URL:
            if (!(valueObject instanceof String)) {
                throw new IllegalArgumentException("data element value");
            }
            break;
        case U_INT_8:
            valueObject = copyOfBytes(valueObject, 8);
            break;
        case U_INT_16:
        case INT_16:
            valueObject = copyOfBytes(valueObject, 16);
            break;
        default:
            throw new IllegalArgumentException("data element type");
        }
        type = valueType;
        value = valueObject;
    }

    private static byte[] copyOfBytes(Object bytes, int length) {
        if (!(bytes instanceof byte[]) || ((byte[]) bytes).length != length) {
            throw new IllegalArgumentException("data element value");
        }
        byte[] copy = new byte[length];
        System.arraycopy(bytes, 0, copy, 0, length);
        return copy;
    }

    private Vector elements() {
        if (type != DATSEQ && type != DATALT) {
            throw new ClassCastException("not a sequence or alternative");
        }
        return (Vector) value;
    }

    /** Appends an element to a DATSEQ or DATALT. */
    public synchronized void addElement(DataElement elem) {
        Vector list = elements();
        if (elem == null) {
            throw new NullPointerException("data element");
        }
        list.addElement(elem);
    }

    /** Inserts an element at an index from 0 to getSize() of a DATSEQ or DATALT. */
    public synchronized void insertElementAt(DataElement elem, int index) {
        Vector list = elements();
        if (elem == null) {
            throw new NullPointerException("data element");
        }
        if (index < 0 || index > list.size()) {
            throw new IndexOutOfBoundsException("data element index");
        }
        list.insertElementAt(elem, index);
    }

    /** Number of elements of a DATSEQ or DATALT. */
    public int getSize() {
        return elements().size();
    }

    /** Removes the first element with the same reference as elem. */
    public synchronized boolean removeElement(DataElement elem) {
        Vector list = elements();
        if (elem == null) {
            throw new NullPointerException("data element");
        }
        for (int i = 0; i < list.size(); i++) {
            if (list.elementAt(i) == elem) {
                list.removeElementAt(i);
                return true;
            }
        }
        return false;
    }

    public int getDataType() {
        return type;
    }

    /** The value of an integer of up to 64 bits. */
    public long getLong() {
        switch (type) {
        case U_INT_1:
        case U_INT_2:
        case U_INT_4:
        case INT_1:
        case INT_2:
        case INT_4:
        case INT_8:
            return number;
        default:
            throw new ClassCastException("not an integer");
        }
    }

    public boolean getBoolean() {
        if (type != BOOL) {
            throw new ClassCastException("not a boolean");
        }
        return flag;
    }

    /**
     * Object values include strings, UUIDs, byte array copies for wide
     * integers, and enumerations for sequences and alternatives.
     */
    public Object getValue() {
        switch (type) {
        case UUID:
        case STRING:
        case URL:
            return value;
        case U_INT_8:
        case U_INT_16:
        case INT_16:
            return copyOfBytes(value, ((byte[]) value).length);
        case DATSEQ:
        case DATALT:
            return ((Vector) value).elements();
        default:
            throw new ClassCastException("no object value");
        }
    }
}
