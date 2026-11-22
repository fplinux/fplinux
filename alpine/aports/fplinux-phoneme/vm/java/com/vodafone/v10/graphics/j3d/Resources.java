// SPDX-License-Identifier: GPL-2.0-only
package com.vodafone.v10.graphics.j3d;

/**
 * j3d resource names are relative to the root of the application JAR with or
 * without a leading slash, while Class.getResourceAsStream() resolves a name
 * without one against the package of the loading class.
 */
final class Resources {
    private Resources() {
    }

    static String absolute(String name) {
        return name.startsWith("/") ? name : "/" + name;
    }
}
