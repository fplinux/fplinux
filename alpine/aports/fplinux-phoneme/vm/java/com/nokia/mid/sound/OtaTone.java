// SPDX-License-Identifier: GPL-2.0-only
package com.nokia.mid.sound;

import java.io.ByteArrayOutputStream;
import javax.microedition.media.control.ToneControl;

/** Translates monophonic OTA ringing tones to the system tone sequencer. */
final class OtaTone {
    private static final int[] TEMPOS = {
        25, 28, 31, 35, 40, 45, 50, 56, 63, 70, 80, 90, 100, 112, 125, 140,
        160, 180, 200, 225, 250, 285, 320, 355, 400, 450, 500, 565, 635,
        715, 800, 900
    };
    private static final int[] DURATION_NUMERATORS = { 1, 3, 7, 2 };
    private static final int[] DURATION_DENOMINATORS = { 1, 2, 4, 3 };
    private static final int MAX_SEQUENCE_BYTES = 131072;

    private final byte[] data;
    private final ByteArrayOutputStream output = new ByteArrayOutputStream();
    private int bit;
    private int octave = 1;
    private int tempo = 63;
    private int style;
    private boolean hasNotes;
    private boolean endless;

    private OtaTone(byte[] data) {
        this.data = data;
    }

    static OtaTone decode(byte[] data) {
        OtaTone result = new OtaTone(data);
        result.readSong();
        return result;
    }

    static byte[] singleNote(int frequency, long durationMs) {
        if (frequency < 0 || frequency > 12544 || durationMs <= 0 ||
                durationMs > (MAX_SEQUENCE_BYTES / 2 - 4) * 127L * 4L) {
            throw new IllegalArgumentException("tone frequency or duration");
        }
        int pitch = ToneControl.SILENCE;
        if (frequency != 0) {
            // MIDI tones use equal temperament. Pick the closest semitone.
            double middle = 8.41536811;
            pitch = 0;
            while (pitch < 127 && frequency > middle) {
                middle *= 1.059463094;
                pitch++;
            }
        }
        OtaTone result = new OtaTone(null);
        result.writeHeader();
        result.note(pitch, Math.max(1, (int) ((durationMs + 2) / 4)));
        return result.sequence();
    }

    byte[] sequence() {
        return output.toByteArray();
    }

    boolean endless() {
        return endless;
    }

    private int read(int count) {
        if ((long) bit + count > (long) data.length * 8) {
            throw new IllegalArgumentException("truncated OTA tone");
        }
        int value = 0;
        for (int i = 0; i < count; i++, bit++) {
            value = (value << 1) | ((data[bit >>> 3] >>> (7 - (bit & 7))) & 1);
        }
        return value;
    }

    private void readSong() {
        int parts = read(8);
        if ((parts != 2 && parts != 3) || read(8) != 0x4a) {
            throw new IllegalArgumentException("OTA tone header");
        }
        boolean unicode = parts == 3;
        if ((unicode && read(8) != 0x44) || read(7) != 0x1d) {
            throw new IllegalArgumentException("OTA tone command");
        }
        int type = read(3);
        if (type == 1) {
            int titleLength = read(4);
            for (int i = 0; i < titleLength; i++) {
                read(unicode ? 16 : 8);
            }
        } else if (type != 2) {
            throw new IllegalArgumentException("unsupported OTA song type");
        }

        writeHeader();
        command(ToneControl.SET_VOLUME, 47);

        int[] starts = new int[4];
        int[] lengths = new int[4];
        int patterns = read(8);
        for (int i = 0; i < patterns; i++) {
            if (read(3) != 0) {
                throw new IllegalArgumentException("OTA pattern header");
            }
            int id = read(2);
            int repeats = read(4);
            int length = read(8);
            int next = bit;
            if (length != 0) {
                starts[id] = bit;
                lengths[id] = length;
                readPattern(length);
                next = bit;
            } else if (lengths[id] == 0) {
                throw new IllegalArgumentException("undefined OTA pattern");
            }
            if (repeats == 15) {
                if (patterns != 1 || length == 0) {
                    throw new IllegalArgumentException("unsupported OTA infinite pattern");
                }
                endless = true;
                repeats = 0;
            } else if (length == 0) {
                repeats++;
            }
            for (int repeat = 0; repeat < repeats; repeat++) {
                bit = starts[id];
                readPattern(lengths[id]);
            }
            bit = next;
        }
        if (!hasNotes) {
            throw new IllegalArgumentException("empty OTA tone");
        }
    }

    private void writeHeader() {
        command(ToneControl.VERSION, 1);
        // A full note is 120 ticks at 500 BPM, or 4 ms per tick.
        command(ToneControl.TEMPO, 125);
        command(ToneControl.RESOLUTION, 120);
    }

    private void readPattern(int instructions) {
        for (int i = 0; i < instructions; i++) {
            switch (read(3)) {
                case 1:
                    readNote();
                    break;
                case 2:
                    octave = read(2);
                    break;
                case 3:
                    style = read(2);
                    if (style == 3) {
                        throw new IllegalArgumentException("OTA tone style");
                    }
                    break;
                case 4:
                    tempo = TEMPOS[read(5)];
                    break;
                case 5:
                    command(ToneControl.SET_VOLUME, (read(4) * 100 + 7) / 15);
                    break;
                default:
                    throw new IllegalArgumentException("OTA tone instruction");
            }
        }
    }

    private void readNote() {
        int pitch = read(4);
        int duration = read(3);
        int modifier = read(2);
        if (pitch > 12 || duration > 5) {
            throw new IllegalArgumentException("OTA note");
        }
        int denominator = tempo * (1 << duration) *
                          DURATION_DENOMINATORS[modifier];
        int ticks = Math.max(1, (60000 * DURATION_NUMERATORS[modifier] +
                                denominator / 2) / denominator);
        if (pitch == 0) {
            note(ToneControl.SILENCE, ticks);
        } else {
            int gap = style == 1 ? 0 : style == 2 ? ticks / 2 : ticks / 12;
            note(59 + pitch + octave * 12, ticks - gap);
            note(ToneControl.SILENCE, gap);
        }
    }

    private void note(int pitch, int ticks) {
        while (ticks > 0) {
            int count = Math.min(ticks, 127);
            command(pitch, count);
            ticks -= count;
            hasNotes = true;
        }
    }

    private void command(int code, int value) {
        if (output.size() == MAX_SEQUENCE_BYTES) {
            throw new IllegalArgumentException("OTA sequence is too long");
        }
        output.write(code);
        output.write(value);
    }
}
