/* SPDX-License-Identifier: GPL-2.0-only */
package javax.microedition.media;

import com.sun.cldchi.jvm.JVM;
import java.io.IOException;
import java.io.InputStream;
import org.fplinux.phoneme.media.PlatformPlayer;

/** Entry points for the MIDP sound subset. */
public final class Manager {
    public static final String TONE_DEVICE_LOCATOR = "device://tone";

    private Manager() {
    }

    /** Returns the shared time base used by players. */
    public static TimeBase getSystemTimeBase() {
        return SystemTimeBase.INSTANCE;
    }

    public static String[] getSupportedContentTypes(String protocol) {
        if (protocol == null) {
            return new String[] { "audio/x-wav", "audio/wav", "audio/midi",
                "audio/x-midi", "audio/mpeg", "audio/amr", "audio/x-tone-seq" };
        }
        if ("device".equals(protocol)) {
            return new String[] { "audio/x-tone-seq" };
        }
        return new String[0];
    }

    public static String[] getSupportedProtocols(String type) {
        if (type == null || "audio/x-tone-seq".equals(type.toLowerCase())) {
            return new String[] { "device" };
        }
        return new String[0];
    }

    public static Player createPlayer(String locator)
            throws IOException, MediaException {
        if (locator == null) {
            throw new IllegalArgumentException("null locator");
        }
        if (TONE_DEVICE_LOCATOR.equals(locator)) {
            return new PlatformPlayer(null, "audio/x-tone-seq");
        }
        throw new MediaException("Unsupported media locator");
    }

    public static Player createPlayer(InputStream stream, String type)
            throws IOException, MediaException {
        if (stream == null) {
            throw new IllegalArgumentException("null stream");
        }
        return new PlatformPlayer(stream, type);
    }

    public static void playTone(int note, int duration, int volume)
            throws MediaException {
        if (note < 0 || note > 127 || duration <= 0) {
            throw new IllegalArgumentException("Invalid tone");
        }
        PlatformPlayer.playTone(note, duration, volume);
    }

    private static final class SystemTimeBase implements TimeBase {
        private static final TimeBase INSTANCE = new SystemTimeBase();
        private final long originMillis = JVM.monotonicTimeMillis();

        public long getTime() {
            return (JVM.monotonicTimeMillis() - originMillis) * 1000L;
        }
    }
}
