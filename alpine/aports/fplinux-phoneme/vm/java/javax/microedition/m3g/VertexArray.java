// SPDX-License-Identifier: GPL-2.0-only
package javax.microedition.m3g;

import java.util.Hashtable;

public class VertexArray extends Object3D {
    final int vertices, components, componentSize;
    final short[] data;

    public VertexArray(int vertices, int components, int componentSize) {
        if (vertices < 1 || vertices > 65535 || components < 2 || components > 4 ||
                (componentSize != 1 && componentSize != 2)) {
            throw new IllegalArgumentException("vertex array format");
        }
        this.vertices = vertices;
        this.components = components;
        this.componentSize = componentSize;
        data = new short[vertices * components];
    }

    public int getVertexCount() {
        return vertices;
    }

    public int getComponentCount() {
        return components;
    }

    public void set(int firstVertex, int numVertices, byte[] values) {
        int count = checkRange(firstVertex, numVertices, values.length, 1);
        int offset = firstVertex * components;
        for (int i = 0; i < count; i++) data[offset + i] = values[i];
    }

    public void set(int firstVertex, int numVertices, short[] values) {
        int count = checkRange(firstVertex, numVertices, values.length, 2);
        System.arraycopy(values, 0, data, firstVertex * components, count);
    }

    public int getComponentType() {
        return componentSize;
    }

    public void get(int firstVertex, int numVertices, byte[] values) {
        int count = checkReadRange(firstVertex, numVertices, values.length, 1);
        int offset = firstVertex * components;
        for (int i = 0; i < count; i++) values[i] = (byte) data[offset + i];
    }

    public void get(int firstVertex, int numVertices, short[] values) {
        int count = checkReadRange(firstVertex, numVertices, values.length, 2);
        System.arraycopy(data, firstVertex * components, values, 0, count);
    }

    private int checkReadRange(int first, int count, int length, int size) {
        if (size != componentSize) throw new IllegalStateException("vertex component size");
        return checkRange(first, count, length, size);
    }

    private int checkRange(int first, int count, int length, int size) {
        if (size != componentSize || count < 0 || count > vertices ||
                length / components < count) throw new IllegalArgumentException("vertex values");
        if (first < 0 || first > vertices - count) throw new IndexOutOfBoundsException();
        return count * components;
    }

    int get(int index, int axis) {
        return axis < components ? data[index * components + axis] : 0;
    }

    Object3D newCopy() {
        return new VertexArray(vertices, components, componentSize);
    }

    void copyFields(Object3D copy, Hashtable copies) {
        super.copyFields(copy, copies);
        System.arraycopy(data, 0, ((VertexArray) copy).data, 0, data.length);
    }
}
