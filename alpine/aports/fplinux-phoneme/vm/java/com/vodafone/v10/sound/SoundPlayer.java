// SPDX-License-Identifier: GPL-2.0-only
package com.vodafone.v10.sound;

/** Hands out independent tracks that play one sound at a time. */
public class SoundPlayer {
    private static final SoundPlayer PLAYER = new SoundPlayer();

    private SoundPlayer() {
    }

    public static SoundPlayer getPlayer() {
        return PLAYER;
    }

    public SoundTrack getTrack() {
        return new SoundTrack();
    }
}
