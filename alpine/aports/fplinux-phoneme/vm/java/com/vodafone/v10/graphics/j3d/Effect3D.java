// SPDX-License-Identifier: GPL-2.0-only
package com.vodafone.v10.graphics.j3d;

public class Effect3D {
    public static final int NORMAL_SHADING = com.mascotcapsule.micro3d.v3.Effect3D.NORMAL_SHADING;
    public static final int TOON_SHADING = com.mascotcapsule.micro3d.v3.Effect3D.TOON_SHADING;

    final com.mascotcapsule.micro3d.v3.Effect3D micro3d;
    private Light light;

    public Effect3D() {
        micro3d = new com.mascotcapsule.micro3d.v3.Effect3D();
    }

    public Effect3D(Light light, int shading, boolean transparency, Texture sphereMap) {
        micro3d = new com.mascotcapsule.micro3d.v3.Effect3D(
            light == null ? null : light.micro3d, shading, transparency, sphereMap);
        this.light = light;
    }

    public Light getLight() {
        return light;
    }

    public void setLight(Light light) {
        micro3d.setLight(light == null ? null : light.micro3d);
        this.light = light;
    }

    public int getShading() {
        return micro3d.getShading();
    }

    public void setShading(int shading) {
        micro3d.setShading(shading);
    }

    public int getThreshold() {
        return micro3d.getThreshold();
    }

    public int getThresholdHigh() {
        return micro3d.getThresholdHigh();
    }

    public int getThresholdLow() {
        return micro3d.getThresholdLow();
    }

    public void setThreshold(int threshold, int high, int low) {
        micro3d.setThreshold(threshold, high, low);
    }

    public boolean isSemiTransparentEnabled() {
        return micro3d.isSemiTransparentEnabled();
    }

    public void setSemiTransparentEnabled(boolean enabled) {
        micro3d.setSemiTransparentEnabled(enabled);
    }

    public Texture getSphereMap() {
        return (Texture) micro3d.getSphereMap();
    }

    public void setSphereMap(Texture sphereMap) {
        micro3d.setSphereMap(sphereMap);
    }
}
