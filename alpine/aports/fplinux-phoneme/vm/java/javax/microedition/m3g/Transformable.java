// SPDX-License-Identifier: GPL-2.0-only
package javax.microedition.m3g;

import java.util.Hashtable;

public abstract class Transformable extends Object3D {
    final Transform orientation = new Transform();
    final Transform generic = new Transform();
    private float x, y, z;
    private float sx = 1, sy = 1, sz = 1;

    public void setTranslation(float tx, float ty, float tz) {
        x = tx;
        y = ty;
        z = tz;
    }

    public void translate(float tx, float ty, float tz) {
        setTranslation(x + tx, y + ty, z + tz);
    }

    public void getTranslation(float[] translation) {
        if (translation.length < 3) throw new IllegalArgumentException("translation");
        translation[0] = x;
        translation[1] = y;
        translation[2] = z;
    }

    public void setScale(float x, float y, float z) {
        sx = x;
        sy = y;
        sz = z;
    }

    public void scale(float x, float y, float z) {
        setScale(sx * x, sy * y, sz * z);
    }

    public void getScale(float[] scale) {
        if (scale.length < 3) throw new IllegalArgumentException("scale");
        scale[0] = sx;
        scale[1] = sy;
        scale[2] = sz;
    }

    public void setOrientation(float angle, float x, float y, float z) {
        Transform updated = new Transform();
        updated.postRotate(angle, x, y, z);
        orientation.set(updated);
    }

    public void postRotate(float angle, float x, float y, float z) {
        orientation.postRotate(angle, x, y, z);
    }

    public void getOrientation(float[] angleAxis) {
        if (angleAxis.length < 4) throw new IllegalArgumentException("orientation");
        float[] matrix = orientation.matrix;
        float trace = matrix[0] + matrix[5] + matrix[10];
        float w;
        if (trace >= 0) {
            float denominator = 2 * (float) Math.sqrt(trace + 1);
            w = .25f * denominator;
            angleAxis[1] = (matrix[9] - matrix[6]) / denominator;
            angleAxis[2] = (matrix[2] - matrix[8]) / denominator;
            angleAxis[3] = (matrix[4] - matrix[1]) / denominator;
        } else {
            // Start with the largest quaternion component near a half-turn.
            int axis = matrix[0] > matrix[5] ? 0 : 1;
            if (matrix[10] > matrix[5 * axis]) axis = 2;
            int first = (axis + 1) % 3;
            int second = (axis + 2) % 3;
            float denominator = 2 * (float) Math.sqrt(1 + matrix[5 * axis] -
                matrix[5 * first] - matrix[5 * second]);
            w = (matrix[4 * second + first] - matrix[4 * first + second]) / denominator;
            angleAxis[axis + 1] = .25f * denominator;
            angleAxis[first + 1] = (matrix[4 * axis + first] + matrix[4 * first + axis]) /
                denominator;
            angleAxis[second + 1] = (matrix[4 * axis + second] + matrix[4 * second + axis]) /
                denominator;
        }

        float length = (float) Math.sqrt(angleAxis[1] * angleAxis[1] +
            angleAxis[2] * angleAxis[2] + angleAxis[3] * angleAxis[3]);
        if (length == 0) {
            angleAxis[0] = 0;
            angleAxis[3] = 1;
            return;
        }
        angleAxis[0] = angleDegrees0(length, Math.abs(w));
        float signedLength = w < 0 ? -length : length;
        for (int i = 1; i <= 3; i++) angleAxis[i] /= signedLength;
    }

    private static native float angleDegrees0(float halfSine, float halfCosine);

    public void setTransform(Transform value) {
        if (value == null) {
            generic.setIdentity();
        } else {
            generic.set(value);
        }
    }

    public void getTransform(Transform result) {
        result.set(generic);
    }

    public void getCompositeTransform(Transform result) {
        result.setIdentity();
        result.postTranslate(x, y, z);
        result.postMultiply(orientation);
        result.postScale(sx, sy, sz);
        result.postMultiply(generic);
    }

    void animated(int property, float[] values) {
        switch (property) {
            case AnimationTrack.TRANSLATION:
                setTranslation(values[0], values[1], values[2]);
                break;
            case AnimationTrack.SCALE:
                setScale(values[0], values.length == 1 ? values[0] : values[1],
                         values.length == 1 ? values[0] : values[2]);
                break;
            case AnimationTrack.ORIENTATION:
                orientation.setIdentity();
                orientation.postRotateQuat(values[0], values[1], values[2], values[3]);
                break;
            default:
                super.animated(property, values);
        }
    }

    void copyFields(Object3D copy, Hashtable copies) {
        super.copyFields(copy, copies);
        Transformable target = (Transformable) copy;
        target.setTranslation(x, y, z);
        target.setScale(sx, sy, sz);
        target.orientation.set(orientation);
        target.generic.set(generic);
    }
}
