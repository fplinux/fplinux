// SPDX-License-Identifier: GPL-2.0-only
package javax.microedition.pim;

public class FieldFullException extends RuntimeException {
    private final int field;

    public FieldFullException() {
        field = -1;
    }

    public FieldFullException(String detailMessage) {
        super(detailMessage);
        field = -1;
    }

    public FieldFullException(String detailMessage, int field) {
        super(detailMessage);
        this.field = field;
    }

    public int getField() {
        return field;
    }
}
