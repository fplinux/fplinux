// SPDX-License-Identifier: GPL-2.0-only
package javax.microedition.m3g;

import java.util.Hashtable;
import java.util.Vector;

public class Mesh extends Node {
    final VertexBuffer vertices;
    final IndexBuffer[] indices;
    final Appearance[] appearances;

    public Mesh(VertexBuffer vertices, IndexBuffer triangles, Appearance look) {
        this(vertices, new IndexBuffer[] { triangles }, new Appearance[] { look });
    }

    public Mesh(VertexBuffer vertices, IndexBuffer[] surfaces, Appearance[] looks) {
        if (vertices == null || surfaces == null) {
            throw new NullPointerException();
        }
        if (surfaces.length == 0 || (looks != null && looks.length < surfaces.length)) {
            throw new IllegalArgumentException("mesh surfaces");
        }
        this.vertices = vertices;
        indices = new IndexBuffer[surfaces.length];
        appearances = new Appearance[surfaces.length];
        for (int i = 0; i < surfaces.length; i++) {
            if (surfaces[i] == null) {
                throw new NullPointerException();
            }
            indices[i] = surfaces[i];
            appearances[i] = looks == null ? null : looks[i];
        }
    }

    public Appearance getAppearance(int submesh) {
        return appearances[submesh];
    }

    public VertexBuffer getVertexBuffer() {
        return vertices;
    }

    public void setAppearance(int submesh, Appearance appearance) {
        appearances[submesh] = appearance;
    }

    public int getSubmeshCount() {
        return indices.length;
    }

    void references(Vector objects) {
        super.references(objects);
        objects.addElement(vertices);
        for (int i = 0; i < indices.length; i++) {
            objects.addElement(indices[i]);
            addReference(objects, appearances[i]);
        }
    }

    Object3D newCopy() {
        return new Mesh(vertices, indices, appearances);
    }
}
