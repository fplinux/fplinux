// SPDX-License-Identifier: GPL-2.0-only
package javax.microedition.m3g;

public abstract class IndexBuffer extends Object3D {
    int[] indices;

    public int getIndexCount() {
        return indices.length;
    }

    public void getIndices(int[] target) {
        System.arraycopy(indices, 0, target, 0, indices.length);
    }
}
