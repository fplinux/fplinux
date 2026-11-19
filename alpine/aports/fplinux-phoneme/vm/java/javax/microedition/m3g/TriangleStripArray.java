// SPDX-License-Identifier: GPL-2.0-only
package javax.microedition.m3g;

public class TriangleStripArray extends IndexBuffer {
    final int[] strips;
    final int maxIndex;

    public TriangleStripArray(int firstIndex, int[] lengths) {
        if (lengths.length == 0) {
            throw new IllegalArgumentException("triangle strip length");
        }
        if (firstIndex < 0 || firstIndex > 65535) {
            throw new IndexOutOfBoundsException("triangle strip index");
        }
        int total = 0;
        int available = 65535 - firstIndex;
        strips = new int[lengths.length];
        for (int i = 0; i < strips.length; i++) {
            int length = lengths[i];
            if (length < 3) {
                throw new IllegalArgumentException("triangle strip length");
            }
            if (length > available - total) {
                throw new IndexOutOfBoundsException("triangle strip index");
            }
            strips[i] = length;
            total += length;
        }
        indices = new int[total];
        for (int i = 0; i < total; i++) {
            indices[i] = firstIndex + i;
        }
        maxIndex = firstIndex + total - 1;
    }

    public TriangleStripArray(int[] indices, int[] lengths) {
        int total = 0;
        strips = new int[lengths.length];
        for (int i = 0; i < strips.length; i++) {
            if (lengths[i] < 3 || lengths[i] > indices.length - total) {
                throw new IllegalArgumentException("triangle strip length");
            }
            strips[i] = lengths[i];
            total += strips[i];
        }
        this.indices = new int[total];
        int largest = -1;
        for (int i = 0; i < total; i++) {
            if (indices[i] < 0 || indices[i] > 65535) {
                throw new IllegalArgumentException("triangle strip index");
            }
            this.indices[i] = indices[i];
            largest = Math.max(largest, indices[i]);
        }
        maxIndex = largest;
    }

    Object3D newCopy() {
        return new TriangleStripArray(indices, strips);
    }
}
