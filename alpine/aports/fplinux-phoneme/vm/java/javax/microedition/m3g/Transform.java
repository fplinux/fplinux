// SPDX-License-Identifier: GPL-2.0-only
package javax.microedition.m3g;

public class Transform {
    private static final float[] IDENTITY = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1,
    };

    final float[] matrix = new float[16];

    public Transform() {
        setIdentity();
    }

    public Transform(Transform other) {
        set(other);
    }

    public void setIdentity() {
        System.arraycopy(IDENTITY, 0, matrix, 0, 16);
    }

    public void set(Transform other) {
        System.arraycopy(other.matrix, 0, matrix, 0, 16);
    }

    public void set(float[] source) {
        if (source.length < 16) {
            throw new IllegalArgumentException("transform elements");
        }
        System.arraycopy(source, 0, matrix, 0, 16);
    }

    public void get(float[] target) {
        if (target.length < 16) {
            throw new IllegalArgumentException("transform elements");
        }
        System.arraycopy(matrix, 0, target, 0, 16);
    }

    public void postMultiply(Transform other) {
        multiply0(matrix, other.matrix);
    }

    public void postTranslate(float x, float y, float z) {
        for (int i = 0; i < 16; i += 4) {
            matrix[i + 3] += matrix[i] * x + matrix[i + 1] * y + matrix[i + 2] * z;
        }
    }

    public void postScale(float x, float y, float z) {
        for (int i = 0; i < 16; i += 4) {
            matrix[i] *= x;
            matrix[i + 1] *= y;
            matrix[i + 2] *= z;
        }
    }

    public void postRotate(float angle, float x, float y, float z) {
        if (angle == 0) {
            return;
        }
        double length = Math.sqrt(x * x + y * y + z * z);
        if (length == 0) {
            throw new IllegalArgumentException("rotation axis");
        }
        double radians = angle * Math.PI / 360;
        double normalized = Math.sin(radians) / length;
        postRotateQuat((float) (x * normalized), (float) (y * normalized),
                       (float) (z * normalized), (float) Math.cos(radians));
    }

    public void postRotateQuat(float x, float y, float z, float w) {
        float magnitude = x * x + y * y + z * z + w * w;
        if (magnitude == 0) {
            throw new IllegalArgumentException("rotation quaternion");
        }
        float factor = 2 / magnitude;
        float xx = x * x * factor, yy = y * y * factor, zz = z * z * factor;
        float xy = x * y * factor, xz = x * z * factor, yz = y * z * factor;
        float wx = w * x * factor, wy = w * y * factor, wz = w * z * factor;
        Transform rotation = new Transform();
        rotation.set(new float[] {
            1 - yy - zz, xy - wz, xz + wy, 0,
            xy + wz, 1 - xx - zz, yz - wx, 0,
            xz - wy, yz + wx, 1 - xx - yy, 0,
            0, 0, 0, 1,
        });
        postMultiply(rotation);
    }

    public void invert() {
        if (!invert0(matrix)) {
            throw new ArithmeticException("singular transform");
        }
    }

    public void transpose() {
        for (int row = 0; row < 4; row++) {
            for (int column = row + 1; column < 4; column++) {
                float value = matrix[4 * row + column];
                matrix[4 * row + column] = matrix[4 * column + row];
                matrix[4 * column + row] = value;
            }
        }
    }

    public void transform(float[] vectors) {
        if (vectors.length % 4 != 0) {
            throw new IllegalArgumentException("transform vectors");
        }
        for (int i = 0; i < vectors.length; i += 4) {
            apply(vectors[i], vectors[i + 1], vectors[i + 2], vectors[i + 3], vectors, i);
        }
    }

    void apply(float x, float y, float z, float w, float[] out) {
        apply(x, y, z, w, out, 0);
    }

    void apply(float x, float y, float z, float w, float[] out, int offset) {
        for (int i = 0; i < 4; i++) {
            int row = 4 * i;
            out[offset + i] = matrix[row] * x + matrix[row + 1] * y +
                matrix[row + 2] * z + matrix[row + 3] * w;
        }
    }

    // Row-major 4x4 matrices of 16 floats: left = left * right.
    private static native void multiply0(float[] left, float[] right);

    // Gauss-Jordan elimination with partial pivoting; false leaves the
    // matrix unchanged when it is singular.
    private static native boolean invert0(float[] matrix);
}
