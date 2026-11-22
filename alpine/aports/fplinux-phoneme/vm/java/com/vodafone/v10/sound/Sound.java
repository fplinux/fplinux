// SPDX-License-Identifier: GPL-2.0-only
package com.vodafone.v10.sound;

import java.io.ByteArrayInputStream;
import java.io.IOException;
import java.io.InputStream;
import javax.microedition.media.Manager;
import javax.microedition.media.MediaException;
import javax.microedition.media.Player;

/** Sound data to be decoded by MMAPI when played. */
public class Sound {
    private final byte[] data;
    private final String resource;

    public Sound(byte[] data) {
        this.data = new byte[data.length];
        System.arraycopy(data, 0, this.data, 0, data.length);
        resource = null;
    }

    /** Selects a resource from the application's JAR root. */
    public Sound(String name) throws IOException {
        data = null;
        resource = name.startsWith("/") ? name : "/" + name;
        open().close();
    }

    Player createPlayer() throws IOException, MediaException {
        InputStream input = open();
        boolean created = false;
        try {
            Player player = Manager.createPlayer(input, null);
            created = true;
            return player;
        } finally {
            if (!created) {
                input.close();
            }
        }
    }

    private InputStream open() throws IOException {
        if (data != null) {
            return new ByteArrayInputStream(data);
        }
        InputStream input = Sound.class.getResourceAsStream(resource);
        if (input == null) {
            throw new IOException("Sound resource not found: " + resource);
        }
        return input;
    }
}
