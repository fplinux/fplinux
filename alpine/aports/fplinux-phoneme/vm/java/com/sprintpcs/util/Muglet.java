// SPDX-License-Identifier: GPL-2.0-only
package com.sprintpcs.util;

/** Launch information for a Sprint URI or media handler. */
public class Muglet {
    public Muglet() {
    }

    public static Muglet getMuglet() {
        // Normal MIDlet launches have no Sprint handler context.
        return null;
    }

    public String getMediaType() {
        return null;
    }

    public String getReferringURI() {
        return null;
    }

    public String getURI() {
        return null;
    }
}
