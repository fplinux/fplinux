// SPDX-License-Identifier: GPL-2.0-only
package javax.microedition.pim;

public class PIMException extends Exception {
    public static final int FEATURE_NOT_SUPPORTED = 0;
    public static final int GENERAL_ERROR = 1;
    public static final int LIST_CLOSED = 2;
    public static final int LIST_NOT_ACCESSIBLE = 3;
    public static final int MAX_CATEGORIES_EXCEEDED = 4;
    public static final int UNSUPPORTED_VERSION = 5;
    public static final int UPDATE_ERROR = 6;

    private final int reason;

    public PIMException() {
        reason = GENERAL_ERROR;
    }

    public PIMException(String detailMessage) {
        super(detailMessage);
        reason = GENERAL_ERROR;
    }

    public PIMException(String detailMessage, int reason) {
        super(detailMessage);
        this.reason = reason;
    }

    public int getReason() {
        return reason;
    }
}
