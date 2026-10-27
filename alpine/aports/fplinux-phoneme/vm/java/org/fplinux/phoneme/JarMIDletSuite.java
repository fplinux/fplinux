/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The one MIDlet suite of this VM: attributes come from the JAR manifest,
 * then from system properties. There is no installer, so there is no JAD,
 * no signature and no permission prompt; every permission is granted.
 */
package org.fplinux.phoneme;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.Hashtable;

import com.sun.midp.io.j2me.storage.File;
import com.sun.midp.midlet.MIDletSuite;
import com.sun.midp.security.SecurityToken;

final class JarMIDletSuite implements MIDletSuite {
    static final int SUITE_ID = 1;

    /* A larger manifest is refused, not truncated. */
    private static final int MANIFEST_MAX_BYTES = 64 * 1024;

    /* Main-section attributes keyed by the ASCII lower-case name. */
    private final Hashtable attributes = new Hashtable();

    /**
     * Reads /META-INF/MANIFEST.MF from the classpath: the MIDlet JAR is the
     * only entry with a manifest, since classes.zip is built without one.
     */
    void loadManifest() throws IOException {
        InputStream in = getClass().getResourceAsStream("/META-INF/MANIFEST.MF");
        if (in == null) {
            return;
        }
        try {
            ByteArrayOutputStream content = new ByteArrayOutputStream();
            byte[] buffer = new byte[512];
            for (int n = in.read(buffer); n > 0; n = in.read(buffer)) {
                if (content.size() + n > MANIFEST_MAX_BYTES) {
                    throw new IOException("manifest larger than "
                                          + MANIFEST_MAX_BYTES + " bytes");
                }
                content.write(buffer, 0, n);
            }
            parse(content.toByteArray());
        } finally {
            in.close();
        }
    }

    /* JAR manifest lines: "Name: value", continued by a leading space. */
    private void parse(byte[] bytes) throws IOException {
        String name = null;
        ByteArrayOutputStream value = new ByteArrayOutputStream();
        int start = 0;
        while (start < bytes.length) {
            int end = start;
            while (end < bytes.length && bytes[end] != '\r'
                   && bytes[end] != '\n') {
                end++;
            }
            if (end == start) {
                break; /* only the main section describes the suite */
            }
            if (bytes[start] == ' ') {
                if (name != null) {
                    value.write(bytes, start + 1, end - start - 1);
                }
            } else {
                store(name, value);
                name = null;
                int colon = start;
                while (colon < end && bytes[colon] != ':') {
                    colon++;
                }
                if (colon > start && colon < end) {
                    name = lowerCase(new String(bytes, start, colon - start,
                                                "UTF-8"));
                    int valueStart = colon + 1;
                    while (valueStart < end && bytes[valueStart] == ' ') {
                        valueStart++;
                    }
                    value.reset();
                    value.write(bytes, valueStart, end - valueStart);
                }
            }
            start = end;
            if (start < bytes.length) {
                start++;
                if (bytes[end] == '\r' && start < bytes.length
                    && bytes[start] == '\n') {
                    start++;
                }
            }
        }
        store(name, value);
    }

    private void store(String name, ByteArrayOutputStream value)
        throws IOException {
        if (name != null) {
            attributes.put(name,
                           new String(value.toByteArray(), "UTF-8").trim());
        }
    }

    private static String lowerCase(String name) {
        char[] chars = name.toCharArray();
        for (int i = 0; i < chars.length; i++) {
            if (chars[i] >= 'A' && chars[i] <= 'Z') {
                chars[i] = (char)(chars[i] + ('a' - 'A'));
            }
        }
        return new String(chars);
    }

    public String getProperty(String key) {
        String value = (String)attributes.get(lowerCase(key));
        return value != null ? value : System.getProperty(key);
    }

    public byte getPushInterruptSetting() {
        return 0;
    }

    public int getPushOptions() {
        return 0;
    }

    public byte[] getPermissions() {
        return null;
    }

    public void setTempProperty(SecurityToken token, String key,
                                String value) {}

    public String getMIDletName(String className) {
        String name = getProperty("MIDlet-Name");
        return name != null ? name : className;
    }

    public void checkIfPermissionAllowed(String permission) {}

    public void checkForPermission(String permission, String resource) {}

    public void checkForPermission(String permission, String resource,
                                   String extraValue) {}

    public int checkPermission(String permission) {
        return 1; /* allowed */
    }

    public int getID() {
        return SUITE_ID;
    }

    public int getMIDletNumber(String className) {
        return 1;
    }

    public boolean permissionToInterrupt(String connection) {
        return true;
    }

    public boolean isRegistered(String midletClassName) {
        return false;
    }

    public boolean isTrusted() {
        return false;
    }

    public boolean isVerified() {
        return false;
    }

    public boolean isEnabled() {
        return true;
    }

    public String getSecureFilenameBase() {
        return filenameBase();
    }

    /*
     * RMS uses this prefix as the path of its files, so it names a file in
     * the per-run storage directory (fplinux_pm_vm_config.storage_dir).
     */
    static String filenameBase() {
        return File.getStorageRoot(0) + "suite";
    }

    public void close() {}
}
