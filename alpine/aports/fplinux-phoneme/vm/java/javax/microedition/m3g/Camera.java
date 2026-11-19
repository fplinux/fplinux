// SPDX-License-Identifier: GPL-2.0-only
package javax.microedition.m3g;

import java.util.Hashtable;

public class Camera extends Node {
    public static final int GENERIC = 48;
    public static final int PARALLEL = 49;
    public static final int PERSPECTIVE = 50;

    final Transform projection = new Transform();
    private int projectionMode = GENERIC;
    private float extent, aspect, near, far;

    public void setPerspective(float fovy, float aspectRatio, float nearPlane,
                               float farPlane) {
        if (fovy <= 0 || fovy >= 180 || aspectRatio <= 0 ||
                nearPlane <= 0 || farPlane <= nearPlane) {
            throw new IllegalArgumentException("perspective projection");
        }
        float zoom = 1 / (float) Math.tan(fovy * Math.PI / 360);
        float depth = farPlane - nearPlane;
        projection.set(new float[] {
            zoom / aspectRatio, 0, 0, 0,
            0, zoom, 0, 0,
            0, 0, -(farPlane + nearPlane) / depth,
            -2 * farPlane * nearPlane / depth,
            0, 0, -1, 0,
        });
        remember(PERSPECTIVE, fovy, aspectRatio, nearPlane, farPlane);
    }

    public void setParallel(float height, float aspectRatio, float nearPlane,
                            float farPlane) {
        if (height <= 0 || aspectRatio <= 0 || farPlane <= nearPlane) {
            throw new IllegalArgumentException("parallel projection");
        }
        float depth = farPlane - nearPlane;
        projection.set(new float[] {
            2 / (height * aspectRatio), 0, 0, 0,
            0, 2 / height, 0, 0,
            0, 0, -2 / depth, -(farPlane + nearPlane) / depth,
            0, 0, 0, 1,
        });
        remember(PARALLEL, height, aspectRatio, nearPlane, farPlane);
    }

    public void setGeneric(Transform value) {
        projection.set(value);
        projectionMode = GENERIC;
    }

    public int getProjection(Transform result) {
        if (result != null) {
            result.set(projection);
        }
        return projectionMode;
    }

    private void remember(int mode, float size, float ratio, float front, float back) {
        projectionMode = mode;
        extent = size;
        aspect = ratio;
        near = front;
        far = back;
    }

    void animated(int property, float[] values) {
        float size = extent, front = near, back = far;
        if (property == AnimationTrack.FIELD_OF_VIEW && projectionMode != GENERIC) {
            size = values[0];
        } else if (property == AnimationTrack.NEAR_DISTANCE && projectionMode != GENERIC) {
            front = values[0];
        } else if (property == AnimationTrack.FAR_DISTANCE && projectionMode != GENERIC) {
            back = values[0];
        } else {
            super.animated(property, values);
            return;
        }
        if (projectionMode == PERSPECTIVE) {
            setPerspective(size, aspect, front, back);
        } else {
            setParallel(size, aspect, front, back);
        }
    }

    Object3D newCopy() {
        return new Camera();
    }

    void copyFields(Object3D copy, Hashtable copies) {
        super.copyFields(copy, copies);
        Camera target = (Camera) copy;
        target.projection.set(projection);
        target.remember(projectionMode, extent, aspect, near, far);
    }
}
