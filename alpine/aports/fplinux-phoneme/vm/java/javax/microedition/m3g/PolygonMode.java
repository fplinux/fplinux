// SPDX-License-Identifier: GPL-2.0-only
package javax.microedition.m3g;

import java.util.Hashtable;

public class PolygonMode extends Object3D {
    public static final int CULL_BACK = 160;
    public static final int CULL_FRONT = 161;
    public static final int CULL_NONE = 162;
    public static final int SHADE_FLAT = 164;
    public static final int SHADE_SMOOTH = 165;
    public static final int WINDING_CCW = 168;
    public static final int WINDING_CW = 169;

    int culling = CULL_BACK;
    int shading = SHADE_SMOOTH;
    int winding = WINDING_CCW;
    boolean perspective = true;
    boolean twoSidedLighting, localCameraLighting;

    public void setPerspectiveCorrectionEnable(boolean enabled) {
        perspective = enabled;
    }

    public boolean isPerspectiveCorrectionEnabled() {
        return perspective;
    }

    public void setLocalCameraLightingEnable(boolean enabled) {
        localCameraLighting = enabled;
    }

    public boolean isLocalCameraLightingEnabled() {
        return localCameraLighting;
    }

    public void setTwoSidedLightingEnable(boolean enabled) {
        twoSidedLighting = enabled;
    }

    public boolean isTwoSidedLightingEnabled() {
        return twoSidedLighting;
    }

    public void setWinding(int mode) {
        if (mode != WINDING_CCW && mode != WINDING_CW) {
            throw new IllegalArgumentException("winding mode");
        }
        winding = mode;
    }

    public int getWinding() {
        return winding;
    }

    public void setCulling(int mode) {
        if (mode < CULL_BACK || mode > CULL_NONE) {
            throw new IllegalArgumentException("culling mode");
        }
        culling = mode;
    }

    public int getCulling() {
        return culling;
    }

    public void setShading(int mode) {
        if (mode != SHADE_FLAT && mode != SHADE_SMOOTH) {
            throw new IllegalArgumentException("shading mode");
        }
        shading = mode;
    }

    public int getShading() {
        return shading;
    }

    Object3D newCopy() {
        return new PolygonMode();
    }

    void copyFields(Object3D copy, Hashtable copies) {
        super.copyFields(copy, copies);
        PolygonMode target = (PolygonMode) copy;
        target.culling = culling;
        target.shading = shading;
        target.winding = winding;
        target.perspective = perspective;
        target.twoSidedLighting = twoSidedLighting;
        target.localCameraLighting = localCameraLighting;
    }
}
