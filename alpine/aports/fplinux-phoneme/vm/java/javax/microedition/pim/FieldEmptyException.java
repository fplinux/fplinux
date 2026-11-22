// SPDX-License-Identifier: GPL-2.0-only
package javax.microedition.pim;

public class FieldEmptyException extends RuntimeException {
    private final int field;

    public FieldEmptyException() {
        field = -1;
    }

    public FieldEmptyException(String detailMessage) {
        super(detailMessage);
        field = -1;
    }

    public FieldEmptyException(String detailMessage, int field) {
        super(detailMessage);
        this.field = field;
    }

    public int getField() {
        return field;
    }
}
