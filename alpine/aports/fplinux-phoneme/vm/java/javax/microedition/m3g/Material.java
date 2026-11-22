// SPDX-License-Identifier: GPL-2.0-only
package javax.microedition.m3g;

import java.util.Hashtable;

public class Material extends Object3D {
    public static final int AMBIENT = 1024;
    public static final int DIFFUSE = 2048;
    public static final int EMISSIVE = 4096;
    public static final int SPECULAR = 8192;

    int ambient = 0x333333;
    int diffuse = 0xffcccccc;
    int emissive, specular;
    float shininess;
    boolean vertexColorTracking;

    public void setShininess(float value) {
        if (!(value >= 0 && value <= 128)) throw new IllegalArgumentException("shininess");
        shininess = value;
    }

    public float getShininess() {
        return shininess;
    }

    public void setVertexColorTrackingEnable(boolean enabled) {
        vertexColorTracking = enabled;
    }

    public boolean isVertexColorTrackingEnabled() {
        return vertexColorTracking;
    }

    public int getColor(int target) {
        switch (target) {
            case AMBIENT: return ambient;
            case DIFFUSE: return diffuse;
            case EMISSIVE: return emissive;
            case SPECULAR: return specular;
            default: throw new IllegalArgumentException("material color");
        }
    }

    public void setColor(int target, int argb) {
        if (target == 0 || (target & ~(AMBIENT | DIFFUSE | EMISSIVE | SPECULAR)) != 0) {
            throw new IllegalArgumentException("material color");
        }
        int rgb = argb & 0x00ffffff;
        if ((target & AMBIENT) != 0) ambient = rgb;
        if ((target & DIFFUSE) != 0) diffuse = argb;
        if ((target & EMISSIVE) != 0) emissive = rgb;
        if ((target & SPECULAR) != 0) specular = rgb;
    }

    Object3D newCopy() {
        return new Material();
    }

    void copyFields(Object3D copy, Hashtable copies) {
        super.copyFields(copy, copies);
        Material target = (Material) copy;
        target.ambient = ambient;
        target.diffuse = diffuse;
        target.emissive = emissive;
        target.specular = specular;
        target.shininess = shininess;
        target.vertexColorTracking = vertexColorTracking;
    }
}
