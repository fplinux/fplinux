// SPDX-License-Identifier: GPL-2.0-only
package javax.microedition.pim;

public class UnsupportedFieldException extends RuntimeException {
    private final int field;

    public UnsupportedFieldException() {
        field = -1;
    }

    public UnsupportedFieldException(String detailMessage) {
        super(detailMessage);
        field = -1;
    }

    public UnsupportedFieldException(String detailMessage, int field) {
        super(detailMessage);
        this.field = field;
    }

    public int getField() {
        return field;
    }
}
