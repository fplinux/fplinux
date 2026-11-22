/* SPDX-License-Identifier: GPL-2.0-only */
package org.fplinux.phoneme.media;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.Vector;
import javax.microedition.media.Control;
import javax.microedition.media.Manager;
import javax.microedition.media.MediaException;
import javax.microedition.media.Player;
import javax.microedition.media.PlayerListener;
import javax.microedition.media.TimeBase;
import javax.microedition.media.control.ToneControl;
import javax.microedition.media.control.VolumeControl;

/** MIDP Player state and listeners stay on Java threads. */
public final class PlatformPlayer implements Player {
    private static final int SOURCE_LIMIT = 2 * 1024 * 1024;
    // Format and event IDs exchanged with the native media port.
    private static final int AUTO = 0;
    private static final int WAV = 1;
    private static final int MP3 = 2;
    private static final int MIDI = 3;
    private static final int SEQUENCE = 4;
    private static final int TONE = 5;
    private static final int AMR_NB = 6;
    private static final int END_EVENT = 1;
    private static final int ERROR_EVENT = 2;
    private static final int START_EVENT = 3;
    private static final int STOP_EVENT = 4;
    private static final int CLOSE_EVENT = 5;
    private static final int VOLUME_EVENT = 6;
    private static final int DURATION_EVENT = 7;
    private static final Vector players = new Vector();
    private static Thread dispatcher;
    private static int nextClientId = -1;

    private InputStream input;
    private boolean realizing;
    private int preparationEpoch;
    private int pendingHandle;
    private String type;
    private int format;
    private int state = UNREALIZED;
    private int handle;
    private long playback;
    private final int clientId;
    private int loopCount = 1;
    private int loopsLeft = 1;
    private boolean loopActive;
    private boolean restartAllowed;
    private int level = 100;
    private boolean muted;
    private boolean automaticClose;
    private final Vector listeners = new Vector();
    private final PlayerVolume volume = new PlayerVolume();
    private final PlayerTone tone = new PlayerTone();

    public PlatformPlayer(InputStream stream, String contentType)
            throws MediaException {
        synchronized (players) {
            clientId = nextClientId--;
        }
        input = stream;
        type = normalize(contentType);
        format = format(type);
        if (stream == null && format != SEQUENCE) {
            throw new MediaException("Missing media stream");
        }
        ensureDispatcher();
    }

    private static String normalize(String contentType) {
        if (contentType == null) {
            return null;
        }
        int parameters = contentType.indexOf(';');
        String normalized = (parameters < 0 ? contentType :
            contentType.substring(0, parameters)).trim().toLowerCase();
        return parameters < 0 && normalized.length() == 0 ? null : normalized;
    }

    private static int format(String type) throws MediaException {
        if (type == null) {
            return AUTO;
        }
        if ("audio/wav".equals(type) || "audio/x-wav".equals(type)) {
            return WAV;
        }
        if ("audio/midi".equals(type) || "audio/x-midi".equals(type)) {
            return MIDI;
        }
        if ("audio/mpeg".equals(type)) {
            return MP3;
        }
        if ("audio/amr".equals(type)) {
            return AMR_NB;
        }
        if ("audio/x-tone-seq".equals(type)) {
            return SEQUENCE;
        }
        throw new MediaException("Unsupported audio content type");
    }

    private static synchronized void ensureDispatcher() {
        if (dispatcher == null) {
            dispatcher = new Thread(new Runnable() {
                public void run() {
                    dispatchEvents();
                }
            });
            dispatcher.start();
        }
    }

    private static final class MediaEvent {
        int handle;
        int type;
        long timeUs;
        int error;
        long playbackId;
    }

    private static void dispatchEvents() {
        MediaEvent event = new MediaEvent();
        initEvent0(event);
        while (nextEvent0(event)) {
            PlatformPlayer player = null;
            synchronized (players) {
                for (int i = 0; i < players.size(); i++) {
                    PlatformPlayer candidate =
                        (PlatformPlayer)players.elementAt(i);
                    if (candidate.handle == event.handle ||
                        candidate.clientId == event.handle) {
                        player = candidate;
                        break;
                    }
                }
            }
            if (player != null) {
                player.event(event);
            }
        }
    }

    private void event(MediaEvent event) {
        int eventType = event.type;
        String name;
        Object data = new Long(event.timeUs);
        boolean repeat = false;
        boolean closeTone = false;
        synchronized (this) {
            if (event.handle != handle && event.handle != clientId) {
                return;
            }
            if (state == CLOSED && eventType != CLOSE_EVENT) {
                return;
            }
            // The broker may have queued completion before a later START.
            if ((eventType == END_EVENT || eventType == ERROR_EVENT) &&
                event.playbackId != playback) {
                return;
            }
            if (eventType == END_EVENT) {
                if (state != STARTED) {
                    return;
                }
                state = PREFETCHED;
                name = PlayerListener.END_OF_MEDIA;
                if (loopsLeft == -1 || loopsLeft > 1) {
                    if (loopsLeft > 1) {
                        loopsLeft--;
                    }
                    repeat = true;
                    restartAllowed = true;
                } else {
                    loopActive = false;
                }
                closeTone = automaticClose && !repeat;
            } else if (eventType == ERROR_EVENT) {
                state = CLOSED;
                loopActive = false;
                destroy0(handle);
                postClosed0(handle);
                name = PlayerListener.ERROR;
                data = "Audio error " + event.error;
            } else if (eventType == START_EVENT) {
                name = PlayerListener.STARTED;
            } else if (eventType == STOP_EVENT) {
                name = PlayerListener.STOPPED;
            } else if (eventType == CLOSE_EVENT) {
                name = PlayerListener.CLOSED;
                data = null;
            } else if (eventType == VOLUME_EVENT) {
                name = PlayerListener.VOLUME_CHANGED;
                data = volume;
            } else if (eventType == DURATION_EVENT) {
                name = PlayerListener.DURATION_UPDATED;
            } else {
                return;
            }
        }
        PlayerListener[] current;
        synchronized (listeners) {
            current = new PlayerListener[listeners.size()];
            listeners.copyInto(current);
        }
        for (int i = 0; i < current.length; i++) {
            try {
                current[i].playerUpdate(this, name, data);
            } catch (Throwable ignored) {
                // One listener cannot prevent delivery to the others.
            }
        }
        if (repeat) {
            synchronized (this) {
                // A listener may stop, close or restart the player itself.
                if (state == PREFETCHED && restartAllowed &&
                    playback == event.playbackId) {
                    try {
                        playback = start0(handle);
                        state = STARTED;
                    } catch (MediaException failure) {
                        postError0(handle);
                    }
                }
            }
        } else if (closeTone) {
            close();
        }
        if (eventType == CLOSE_EVENT) {
            synchronized (players) {
                players.removeElement(this);
            }
        }
    }

    private void checkOpen() {
        if (state == CLOSED) {
            throw new IllegalStateException("Player is closed");
        }
    }

    private void install(byte[] bytes, int selectedFormat, int note,
                         int duration) throws MediaException {
        install(bytes, selectedFormat, note, duration, preparationEpoch);
    }

    private void install(byte[] bytes, int selectedFormat, int note,
                         int duration, int epoch) throws MediaException {
        int created = create0(selectedFormat, bytes, note, duration);
        synchronized (this) {
            if (state == CLOSED || epoch != preparationEpoch) {
                destroy0(created);
                throw new MediaException("Preparation was cancelled");
            }
            pendingHandle = created;
        }
        try {
            await0(created);
        } catch (MediaException failure) {
            destroy0(created);
            throw failure;
        } finally {
            synchronized (this) {
                if (pendingHandle == created) {
                    pendingHandle = 0;
                }
            }
        }
        synchronized (this) {
            if (state == CLOSED || epoch != preparationEpoch) {
                destroy0(created);
                throw new MediaException("Preparation was cancelled");
            }
            handle = created;
            synchronized (players) {
                players.addElement(this);
            }
            int actual = format0(handle);
            if (actual == WAV) {
                type = "audio/x-wav";
            } else if (actual == MIDI) {
                type = "audio/midi";
            } else if (actual == MP3) {
                type = "audio/mpeg";
            } else if (actual == AMR_NB) {
                type = "audio/amr";
            }
        }
    }

    public void realize() throws MediaException {
        InputStream sourceInput;
        int epoch;
        synchronized (this) {
            checkOpen();
            while (realizing) {
                try {
                    wait();
                } catch (InterruptedException failure) {
                    throw new MediaException("Preparation was interrupted");
                }
                checkOpen();
            }
            if (state >= REALIZED) {
                return;
            }
            if (input == null) {
                if (format == SEQUENCE) {
                    state = REALIZED;
                    return;
                }
                throw new MediaException("Audio source is closed");
            }
            sourceInput = input;
            realizing = true;
            epoch = preparationEpoch;
        }
        ByteArrayOutputStream source = new ByteArrayOutputStream();
        byte[] block = new byte[4096];
        try {
            int count;
            while ((count = sourceInput.read(block)) != -1) {
                if (count == 0) {
                    int one = sourceInput.read();
                    if (one < 0) {
                        break;
                    }
                    if (source.size() == SOURCE_LIMIT) {
                        throw new MediaException("Audio resource is too large");
                    }
                    source.write(one);
                } else {
                    if (count > SOURCE_LIMIT - source.size()) {
                        throw new MediaException("Audio resource is too large");
                    }
                    source.write(block, 0, count);
                }
            }
            install(source.toByteArray(), format, 0, 0, epoch);
            try {
                sourceInput.close();
            } catch (IOException ignored) {
            }
            synchronized (this) {
                if (state == CLOSED || epoch != preparationEpoch) {
                    throw new MediaException("Preparation was cancelled");
                }
                input = null;
                state = REALIZED;
                long duration = duration0(handle);
                if (duration != TIME_UNKNOWN) {
                    postDuration0(handle, duration);
                }
            }
        } catch (IOException failure) {
            throw new MediaException("Cannot read audio resource: " + failure);
        } finally {
            synchronized (this) {
                if (epoch == preparationEpoch) {
                    realizing = false;
                }
                notifyAll();
            }
        }
    }

    public void prefetch() throws MediaException {
        realize();
        synchronized (this) {
            checkOpen();
            if (state >= PREFETCHED) {
                return;
            }
            if (handle != 0) {
                prefetch0(handle);
                setGain0(handle, muted ? 0 : level);
            }
            state = PREFETCHED;
        }
    }

    public void start() throws MediaException {
        prefetch();
        synchronized (this) {
            checkOpen();
            if (state == STARTED) {
                return;
            }
            if (handle == 0) {
                throw new MediaException("Tone sequence is not set");
            }
            playback = start0(handle);
            if (!loopActive) {
                loopsLeft = loopCount;
            }
            loopActive = true;
            state = STARTED;
        }
    }

    public synchronized void stop() throws MediaException {
        checkOpen();
        restartAllowed = false;
        if (state != STARTED) {
            return;
        }
        pause0(handle);
        state = PREFETCHED;
    }

    public synchronized void deallocate() {
        checkOpen();
        if (realizing) {
            cancelPreparation();
            return;
        }
        if (state < PREFETCHED) {
            return;
        }
        try {
            stop();
            if (handle != 0) {
                release0(handle);
            }
        } catch (MediaException failure) {
            postError0(handle);
        }
        state = REALIZED;
    }

    public synchronized void close() {
        if (state == CLOSED) {
            return;
        }
        state = CLOSED;
        cancelPreparation();
        loopsLeft = 0;
        loopActive = false;
        if (handle != 0) {
            destroy0(handle);
            postClosed0(handle);
        } else {
            synchronized (players) {
                players.addElement(this);
            }
            postClosed0(clientId);
        }
    }

    private void cancelPreparation() {
        preparationEpoch++;
        realizing = false;
        if (pendingHandle != 0) {
            destroy0(pendingHandle);
            pendingHandle = 0;
        }
        if (state == UNREALIZED && handle != 0) {
            destroy0(handle);
            handle = 0;
            synchronized (players) {
                players.removeElement(this);
            }
        }
        if (input != null) {
            try {
                input.close();
            } catch (IOException ignored) {
            }
            input = null;
        }
        notifyAll();
    }

    public synchronized long setMediaTime(long time) throws MediaException {
        checkOpen();
        if (state < REALIZED) {
            throw new IllegalStateException("Player is not realized");
        }
        if (handle == 0) {
            return 0;
        }
        return seek0(handle, time < 0 ? 0 : time);
    }

    public synchronized long getMediaTime() {
        checkOpen();
        return handle == 0 ? 0 : mediaTime0(handle);
    }

    public synchronized long getDuration() {
        checkOpen();
        return handle == 0 ? (format == SEQUENCE ? 0 : TIME_UNKNOWN) :
            duration0(handle);
    }

    public synchronized int getState() {
        return state;
    }

    public synchronized String getContentType() {
        checkOpen();
        if (state < REALIZED) {
            throw new IllegalStateException("Player is not realized");
        }
        return type;
    }

    public synchronized void setLoopCount(int count) {
        checkOpen();
        if (state == STARTED) {
            throw new IllegalStateException("Player is started");
        }
        if (count == 0 || count < -1) {
            throw new IllegalArgumentException("Invalid loop count");
        }
        loopCount = count;
        loopsLeft = count;
        loopActive = false;
    }

    public synchronized TimeBase getTimeBase() {
        checkOpen();
        if (state < REALIZED) {
            throw new IllegalStateException("Player is not realized");
        }
        return Manager.getSystemTimeBase();
    }

    public synchronized void setTimeBase(TimeBase master) throws MediaException {
        checkOpen();
        if (state < REALIZED || state == STARTED) {
            throw new IllegalStateException("Player is not realized or is started");
        }
        if (master != null && master != Manager.getSystemTimeBase()) {
            throw new MediaException("Unsupported time base");
        }
    }

    public void addPlayerListener(PlayerListener listener) {
        checkOpen();
        if (listener != null) {
            synchronized (listeners) {
                if (!listeners.contains(listener)) {
                    listeners.addElement(listener);
                }
            }
        }
    }

    public void removePlayerListener(PlayerListener listener) {
        checkOpen();
        if (listener != null) {
            synchronized (listeners) {
                listeners.removeElement(listener);
            }
        }
    }

    public synchronized Control getControl(String name) {
        checkOpen();
        if (state < REALIZED) {
            throw new IllegalStateException("Player is not realized");
        }
        if (name == null) {
            throw new IllegalArgumentException("null control");
        }
        if ("VolumeControl".equals(name) ||
            "javax.microedition.media.control.VolumeControl".equals(name)) {
            return volume;
        }
        if (format == SEQUENCE && ("ToneControl".equals(name) ||
            "javax.microedition.media.control.ToneControl".equals(name))) {
            return tone;
        }
        return null;
    }

    public synchronized Control[] getControls() {
        checkOpen();
        if (state < REALIZED) {
            throw new IllegalStateException("Player is not realized");
        }
        return format == SEQUENCE ? new Control[] { volume, tone } :
            new Control[] { volume };
    }

    private final class PlayerVolume implements VolumeControl {
        public int setLevel(int requested) {
            synchronized (PlatformPlayer.this) {
                checkOpen();
                int clamped = Math.max(0, Math.min(100, requested));
                if (handle != 0) {
                    try {
                        setGain0(handle, muted ? 0 : clamped);
                    } catch (MediaException failure) {
                        throw new IllegalStateException(failure.toString());
                    }
                }
                level = clamped;
                if (handle != 0) {
                    postVolume0(handle);
                }
                return level;
            }
        }

        public int getLevel() {
            synchronized (PlatformPlayer.this) {
                checkOpen();
                return level;
            }
        }

        public void setMute(boolean requested) {
            synchronized (PlatformPlayer.this) {
                checkOpen();
                if (handle != 0) {
                    try {
                        setGain0(handle, requested ? 0 : level);
                    } catch (MediaException failure) {
                        throw new IllegalStateException(failure.toString());
                    }
                }
                muted = requested;
                if (handle != 0) {
                    postVolume0(handle);
                }
            }
        }

        public boolean isMuted() {
            synchronized (PlatformPlayer.this) {
                checkOpen();
                return muted;
            }
        }
    }

    private final class PlayerTone implements ToneControl {
        public void setSequence(byte[] sequence) {
            synchronized (PlatformPlayer.this) {
                checkOpen();
                if (state != REALIZED) {
                    throw new IllegalStateException("Tone player is prefetched");
                }
                if (sequence == null) {
                    throw new IllegalArgumentException("null tone sequence");
                }
                int previous = handle;
                try {
                    // Validate the replacement before discarding the old source.
                    int created = create0(SEQUENCE, sequence, 0, 0);
                    try {
                        await0(created);
                    } catch (MediaException failure) {
                        destroy0(created);
                        throw failure;
                    }
                    handle = created;
                    playback = 0;
                    if (previous == 0) {
                        synchronized (players) {
                            players.addElement(PlatformPlayer.this);
                        }
                    } else {
                        destroy0(previous);
                    }
                    postDuration0(handle, duration0(handle));
                } catch (MediaException failure) {
                    throw new IllegalArgumentException(failure.toString());
                }
            }
        }
    }

    public static void playTone(int note, int duration, int requested)
            throws MediaException {
        PlatformPlayer player = new PlatformPlayer(null, "audio/x-tone-seq");
        try {
            player.install(null, TONE, note, duration);
            player.state = REALIZED;
            player.level = Math.max(0, Math.min(100, requested));
            player.automaticClose = true;
            player.start();
        } catch (MediaException failure) {
            player.close();
            throw failure;
        }
    }

    private static native int create0(int format, byte[] bytes, int note,
                                      int duration) throws MediaException;
    private static native void await0(int handle) throws MediaException;
    private static native void prefetch0(int handle) throws MediaException;
    private static native long start0(int handle) throws MediaException;
    private static native void pause0(int handle) throws MediaException;
    private static native void release0(int handle) throws MediaException;
    private static native long seek0(int handle, long timeUs) throws MediaException;
    private static native void setGain0(int handle, int gain) throws MediaException;
    private static native long mediaTime0(int handle);
    private static native long duration0(int handle);
    private static native int format0(int handle);
    private static native void destroy0(int handle);
    private static native void postClosed0(int handle);
    private static native void postError0(int handle);
    private static native void postVolume0(int handle);
    private static native void postDuration0(int handle, long durationUs);
    private static native void initEvent0(MediaEvent event);
    private static native boolean nextEvent0(MediaEvent event);
}
