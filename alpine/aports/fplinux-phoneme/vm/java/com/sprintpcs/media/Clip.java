// SPDX-License-Identifier: GPL-2.0-only
package com.sprintpcs.media;

import java.io.ByteArrayInputStream;
import java.io.IOException;
import java.io.InputStream;
import javax.microedition.media.Manager;
import javax.microedition.media.MediaException;

public class Clip {
    private static final String RESOURCE_SCHEME = "resource:";

    private final String resource;
    private final byte[] data;
    private final String contentType;
    private final int priority;
    private final int vibration;

    public Clip(String location, String contentType, int priority, int vibration)
            throws IOException {
        if (location == null || contentType == null) {
            throw new NullPointerException("clip location or type");
        }
        if (vibration < 0) {
            throw new IllegalArgumentException("vibration duration");
        }
        if (location.startsWith(RESOURCE_SCHEME)) {
            location = location.substring(RESOURCE_SCHEME.length());
        } else if (location.indexOf(':') >= 0) {
            throw new IOException("Unsupported clip location: " + location);
        }
        resource = location.startsWith("/") ? location : "/" + location;
        data = null;
        this.contentType = contentType;
        this.priority = priority;
        this.vibration = vibration;
    }

    public Clip(byte[] source, String contentType, int priority, int vibration)
            throws IOException {
        if (source == null || contentType == null) {
            throw new NullPointerException("clip data or type");
        }
        if (vibration < 0) {
            throw new IllegalArgumentException("vibration duration");
        }
        data = new byte[source.length];
        System.arraycopy(source, 0, data, 0, source.length);
        resource = null;
        this.contentType = contentType;
        this.priority = priority;
        this.vibration = vibration;
    }

    public String getContentType() {
        return contentType;
    }

    protected int getPriority() {
        return priority;
    }

    protected int getVibration() {
        return vibration;
    }

    javax.microedition.media.Player createPlayer() throws IOException, MediaException {
        InputStream input = data == null ? Clip.class.getResourceAsStream(resource) :
            new ByteArrayInputStream(data);
        if (input == null) {
            throw new IOException("Clip resource not found: " + resource);
        }
        boolean created = false;
        try {
            javax.microedition.media.Player player = Manager.createPlayer(input, contentType);
            created = true;
            return player;
        } finally {
            if (!created) {
                input.close();
            }
        }
    }
}
