// SPDX-License-Identifier: GPL-2.0-only
package javax.microedition.m3g;

import java.util.Hashtable;

public class CompositingMode extends Object3D {
    public static final int ALPHA = 64;
    public static final int ALPHA_ADD = 65;
    public static final int MODULATE = 66;
    public static final int MODULATE_X2 = 67;
    public static final int REPLACE = 68;

    boolean depthTest = true, depthWrite = true;
    boolean colorWrite = true, alphaWrite = true;
    int blend = REPLACE;
    float alphaThreshold, depthOffsetFactor, depthOffsetUnits;

    public void setDepthTestEnable(boolean enabled) {
        depthTest = enabled;
    }

    public boolean isDepthTestEnabled() {
        return depthTest;
    }

    public void setDepthWriteEnable(boolean enabled) {
        depthWrite = enabled;
    }

    public boolean isDepthWriteEnabled() {
        return depthWrite;
    }

    public void setColorWriteEnable(boolean enabled) {
        colorWrite = enabled;
    }

    public boolean isColorWriteEnabled() {
        return colorWrite;
    }

    public void setAlphaWriteEnable(boolean enabled) {
        alphaWrite = enabled;
    }

    public boolean isAlphaWriteEnabled() {
        return alphaWrite;
    }

    public void setDepthOffset(float factor, float units) {
        depthOffsetFactor = factor;
        depthOffsetUnits = units;
    }

    public float getDepthOffsetFactor() {
        return depthOffsetFactor;
    }

    public float getDepthOffsetUnits() {
        return depthOffsetUnits;
    }

    public float getAlphaThreshold() {
        return alphaThreshold;
    }

    public int getBlending() {
        return blend;
    }

    public void setAlphaThreshold(float threshold) {
        if (!(threshold >= 0 && threshold <= 1)) {
            throw new IllegalArgumentException("alpha threshold");
        }
        alphaThreshold = threshold;
    }

    public void setBlending(int mode) {
        if (mode < ALPHA || mode > REPLACE) throw new IllegalArgumentException("blending mode");
        blend = mode;
    }

    Object3D newCopy() {
        return new CompositingMode();
    }

    void copyFields(Object3D copy, Hashtable copies) {
        super.copyFields(copy, copies);
        CompositingMode target = (CompositingMode) copy;
        target.depthTest = depthTest;
        target.depthWrite = depthWrite;
        target.colorWrite = colorWrite;
        target.alphaWrite = alphaWrite;
        target.blend = blend;
        target.alphaThreshold = alphaThreshold;
        target.depthOffsetFactor = depthOffsetFactor;
        target.depthOffsetUnits = depthOffsetUnits;
    }
}
