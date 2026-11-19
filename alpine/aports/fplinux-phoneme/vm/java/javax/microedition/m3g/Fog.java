// SPDX-License-Identifier: GPL-2.0-only
package javax.microedition.m3g;

import java.util.Hashtable;

public class Fog extends Object3D {
    public static final int EXPONENTIAL = 80;
    public static final int LINEAR = 81;

    private int mode = LINEAR;
    private int color;
    private float near, far = 1, density = 1;

    public void setMode(int value) {
        if (value != EXPONENTIAL && value != LINEAR) throw new IllegalArgumentException("fog mode");
        mode = value;
    }

    public int getMode() {
        return mode;
    }

    public void setColor(int rgb) {
        color = rgb & 0x00ffffff;
    }

    public int getColor() {
        return color;
    }

    public void setLinear(float from, float to) {
        near = from;
        far = to;
    }

    public float getNearDistance() {
        return near;
    }

    public float getFarDistance() {
        return far;
    }

    public void setDensity(float value) {
        if (!(value >= 0)) throw new IllegalArgumentException("fog density");
        density = value;
    }

    public float getDensity() {
        return density;
    }

    float visibility(float distance) {
        if (mode == EXPONENTIAL) return exp0(-distance * density);
        if (near == far) return distance <= near ? 1 : 0;
        return Math.max(0, Math.min(1, (far - distance) / (far - near)));
    }

    Object3D newCopy() {
        return new Fog();
    }

    void copyFields(Object3D copy, Hashtable copies) {
        super.copyFields(copy, copies);
        Fog target = (Fog) copy;
        target.mode = mode;
        target.color = color;
        target.near = near;
        target.far = far;
        target.density = density;
    }

    private static native float exp0(float value);
}
