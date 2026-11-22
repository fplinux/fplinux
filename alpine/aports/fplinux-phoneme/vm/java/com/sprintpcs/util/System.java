// SPDX-License-Identifier: GPL-2.0-only
package com.sprintpcs.util;

public class System {
    public System() {
    }

    public static void appendReferringURI(String suffix) {
        requireDisabled(suffix);
    }

    public static void setExitURI(String uri) {
        requireDisabled(uri);
    }

    private static void requireDisabled(String value) {
        if (value != null && value.length() != 0) {
            throw new IllegalStateException("Sprint URI dispatch is unavailable");
        }
    }
}
