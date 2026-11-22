// SPDX-License-Identifier: GPL-2.0-only
package com.samsung.util;

public class SMS {
    private SMS() {
    }

    public static boolean isSupported() {
        return false;
    }

    public static void send(SM message) {
        throw new IllegalStateException("SMS is unavailable");
    }
}
