// SPDX-License-Identifier: GPL-2.0-only
package com.vodafone.v10.graphics.j3d;

public class FigureLayout {
    final com.mascotcapsule.micro3d.v3.FigureLayout micro3d;

    public FigureLayout() {
        this(null, 512, 512, 0, 0);
    }

    public FigureLayout(AffineTrans transform, int scaleX, int scaleY, int centerX, int centerY) {
        micro3d = new com.mascotcapsule.micro3d.v3.FigureLayout(
            identityIfNull(transform), scaleX, scaleY, centerX, centerY);
    }

    public AffineTrans getAffineTrans() {
        return (AffineTrans) micro3d.getAffineTrans();
    }

    public void setAffineTrans(AffineTrans transform) {
        micro3d.setAffineTrans(identityIfNull(transform));
    }

    public int getScaleX() {
        return micro3d.getScaleX();
    }

    public int getScaleY() {
        return micro3d.getScaleY();
    }

    public void setScale(int scaleX, int scaleY) {
        micro3d.setScale(scaleX, scaleY);
    }

    public int getCenterX() {
        return micro3d.getCenterX();
    }

    public int getCenterY() {
        return micro3d.getCenterY();
    }

    public void setCenter(int x, int y) {
        micro3d.setCenter(x, y);
    }

    private static AffineTrans identityIfNull(AffineTrans transform) {
        if (transform == null) {
            transform = new AffineTrans();
            transform.setIdentity();
        }
        return transform;
    }
}
