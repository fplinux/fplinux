// SPDX-License-Identifier: GPL-2.0-only
package javax.microedition.m3g;

import java.util.Hashtable;

public class Light extends Node {
    public static final int AMBIENT = 128;
    public static final int DIRECTIONAL = 129;
    public static final int OMNI = 130;
    public static final int SPOT = 131;

    int mode = DIRECTIONAL;
    int color = 0xffffff;
    float intensity = 1;

    public void setMode(int value) {
        if (value < AMBIENT || value > SPOT) {
            throw new IllegalArgumentException("light mode");
        }
        mode = value;
    }

    public void setIntensity(float value) {
        if (value < 0) {
            throw new IllegalArgumentException("light intensity");
        }
        intensity = value;
    }

    void animated(int property, float[] values) {
        if (property == AnimationTrack.INTENSITY) {
            setIntensity(values[0]);
        } else {
            super.animated(property, values);
        }
    }

    Object3D newCopy() {
        return new Light();
    }

    void copyFields(Object3D copy, Hashtable copies) {
        super.copyFields(copy, copies);
        Light target = (Light) copy;
        target.mode = mode;
        target.color = color;
        target.intensity = intensity;
    }
}
