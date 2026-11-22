/* SPDX-License-Identifier: GPL-2.0-only */
package javax.microedition.media;

/** A clock used to synchronize playback. */
public interface TimeBase {
    /** Returns non-negative, nondecreasing time in microseconds. */
    long getTime();
}
