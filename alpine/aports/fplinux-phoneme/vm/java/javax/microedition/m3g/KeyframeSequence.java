// SPDX-License-Identifier: GPL-2.0-only
package javax.microedition.m3g;

import java.util.Hashtable;

public class KeyframeSequence extends Object3D {
    public static final int LINEAR = 176;
    public static final int SLERP = 177;
    public static final int SPLINE = 178;
    public static final int SQUAD = 179;
    public static final int STEP = 180;
    public static final int CONSTANT = 192;
    public static final int LOOP = 193;

    private final int interpolation;
    private final int components;
    private final int[] time;
    private final float[][] values;
    private int first, last, duration;
    private int repeat = CONSTANT;

    public KeyframeSequence(int frames, int components, int interpolation) {
        if (frames <= 0 || components <= 0 || interpolation < LINEAR ||
                interpolation > STEP ||
                ((interpolation == SLERP || interpolation == SQUAD) &&
                 components != 4)) {
            throw new IllegalArgumentException("keyframe format");
        }
        this.interpolation = interpolation;
        this.components = components;
        time = new int[frames];
        values = new float[frames][components];
        last = frames - 1;
    }

    public void setKeyframe(int index, int position, float[] vector) {
        if (position < 0 || vector.length < components) {
            throw new IllegalArgumentException("keyframe values");
        }
        time[index] = position;
        System.arraycopy(vector, 0, values[index], 0, components);
    }

    // A null vector asks for the time alone.
    public int getKeyframe(int index, float[] vector) {
        if (vector != null) {
            if (vector.length < components) throw new IllegalArgumentException("keyframe values");
            System.arraycopy(values[index], 0, vector, 0, components);
        }
        return time[index];
    }

    public int getKeyframeCount() {
        return time.length;
    }

    public int getComponentCount() {
        return components;
    }

    public int getInterpolationType() {
        return interpolation;
    }

    public int getRepeatMode() {
        return repeat;
    }

    public int getValidRangeFirst() {
        return first;
    }

    public int getValidRangeLast() {
        return last;
    }

    public void setDuration(int length) {
        if (length <= 0) {
            throw new IllegalArgumentException("animation duration");
        }
        duration = length;
    }

    public int getDuration() {
        return duration;
    }

    public void setRepeatMode(int mode) {
        if (mode != CONSTANT && mode != LOOP) {
            throw new IllegalArgumentException("repeat mode");
        }
        repeat = mode;
    }

    public void setValidRange(int start, int end) {
        if (start < 0 || end < 0 || start >= time.length || end >= time.length) {
            throw new IndexOutOfBoundsException();
        }
        first = start;
        last = end;
    }

    float[] sample(float position) {
        if (duration > 0 && repeat == LOOP) {
            position %= duration;
            if (position < 0) {
                position += duration;
            }
        }
        if (position <= time[first] || first == last) {
            return values[first];
        }
        int a = first;
        for (;;) {
            int b = a + 1 == time.length ? 0 : a + 1;
            if (a == last) {
                if (repeat == LOOP && duration > time[last] &&
                        position > time[last]) {
                    return between(last, first,
                        (position - time[last]) / (duration - time[last] + time[first]));
                }
                return values[last];
            }
            if (position <= time[b]) {
                return between(a, b, time[b] == time[a] ? 1 :
                    (position - time[a]) / (time[b] - time[a]));
            }
            a = b;
        }
    }

    private float[] between(int a, int b, float ratio) {
        if (interpolation == STEP) {
            return ratio < 1 ? values[a] : values[b];
        }
        float[] out = new float[components];
        if (interpolation == SLERP || interpolation == SQUAD) {
            slerp(values[a], values[b], ratio, out);
        } else if (interpolation == SPLINE) {
            int previous = a == first ? a : (a + time.length - 1) % time.length;
            int next = b == last ? b : (b + 1) % time.length;
            float t2 = ratio * ratio, t3 = t2 * ratio;
            for (int i = 0; i < components; i++) {
                float p = values[a][i];
                float q = values[b][i];
                float dp = (q - values[previous][i]) * .5f;
                float dq = (values[next][i] - p) * .5f;
                out[i] = (2 * t3 - 3 * t2 + 1) * p +
                    (t3 - 2 * t2 + ratio) * dp +
                    (-2 * t3 + 3 * t2) * q + (t3 - t2) * dq;
            }
        } else {
            for (int i = 0; i < components; i++) {
                out[i] = values[a][i] + (values[b][i] - values[a][i]) * ratio;
            }
        }
        return out;
    }

    private static void slerp(float[] a, float[] b, float ratio, float[] out) {
        float dot = 0;
        for (int i = 0; i < 4; i++) {
            dot += a[i] * b[i];
        }
        float sign = dot >= 0 ? 1 : -1;
        float cosine = Math.abs(dot);
        float left = 1 - ratio;
        float right = ratio;
        if (cosine < .9995f) {
            double min = 0, max = Math.PI / 2;
            for (int i = 0; i < 16; i++) {
                double middle = (min + max) / 2;
                if (Math.cos(middle) > cosine) {
                    min = middle;
                } else {
                    max = middle;
                }
            }
            double angle = (min + max) / 2;
            double divisor = Math.sin(angle);
            left = (float) (Math.sin((1 - ratio) * angle) / divisor);
            right = (float) (Math.sin(ratio * angle) / divisor);
        }
        for (int i = 0; i < 4; i++) {
            out[i] = left * a[i] + right * b[i] * sign;
        }
    }

    Object3D newCopy() {
        return new KeyframeSequence(time.length, components, interpolation);
    }

    void copyFields(Object3D copy, Hashtable copies) {
        super.copyFields(copy, copies);
        KeyframeSequence target = (KeyframeSequence) copy;
        target.duration = duration;
        target.repeat = repeat;
        target.first = first;
        target.last = last;
        for (int i = 0; i < time.length; i++) {
            target.setKeyframe(i, time[i], values[i]);
        }
    }
}
