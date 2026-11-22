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

    public IndexBuffer getIndexBuffer(int submesh) {
        return indices[submesh];
    }

    public void setAppearance(int submesh, Appearance appearance) {
        appearances[submesh] = appearance;
    }

    public int getSubmeshCount() {
        return indices.length;
    }

    // Tests the triangles of the submeshes that have an appearance, except
    // those their polygon mode culls as seen along the ray. Skinned and
    // morphed vertices are in their current pose.
    void pickNode(int pickScope, Transform model, float[] ray, RayIntersection best) {
        int count = vertices.getVertexCount();
        if ((scope & pickScope) == 0 || count == 0) return;
        Transform inverse = new Transform(model);
        try {
            inverse.invert();
        } catch (ArithmeticException singular) {
            return;
        }
        float[] local = new float[8];
        inverse.apply(ray[0], ray[1], ray[2], 1, local, 0);
        inverse.apply(ray[3], ray[4], ray[5], 0, local, 4);
        // A mirroring transform turns the faces the other way round.
        boolean mirrored = determinant3x3(model.matrix) < 0;

        SkinnedMesh skin = this instanceof SkinnedMesh ? (SkinnedMesh) this : null;
        MorphingMesh morph = this instanceof MorphingMesh ? (MorphingMesh) this : null;
        if (skin != null) skin.prepareSkinning();
        if (morph != null) morph.prepareMorphing();
        float[] points = new float[3 * count];
        float[] point = new float[3];
        VertexArray baseNormals = vertices.normals;
        float[] normals = (skin != null || morph != null) && baseNormals != null ?
            new float[3 * count] : null;
        float[] normal = normals == null ? null : new float[3];
        for (int i = 0; i < count; i++) {
            vertices.point(i, point);
            if (normal != null) {
                for (int axis = 0; axis < 3; axis++) normal[axis] = baseNormals.get(i, axis);
            }
            if (skin != null) skin.skin(i, point, normal);
            else if (morph != null) morph.morph(i, point, normal);
            System.arraycopy(point, 0, points, 3 * i, 3);
            if (normal != null) System.arraycopy(normal, 0, normals, 3 * i, 3);
        }
        for (int submesh = 0; submesh < indices.length; submesh++) {
            TriangleStripArray triangles = (TriangleStripArray) indices[submesh];
            Appearance look = appearances[submesh];
            if (look == null || triangles.maxIndex >= count) continue;
            int culling = look.polygon == null ? PolygonMode.CULL_BACK : look.polygon.culling;
            boolean clockwise = look.polygon != null &&
                look.polygon.winding == PolygonMode.WINDING_CW;
            int offset = 0;
            for (int strip = 0; strip < triangles.strips.length; strip++) {
                int length = triangles.strips[strip];
                for (int k = 0; k + 2 < length; k++) {
                    int a = triangles.indices[offset + k];
                    int b = triangles.indices[offset + k + 1];
                    int c = triangles.indices[offset + k + 2];
                    // Odd triangles of a strip are wound the other way round.
                    if ((k & 1) != 0) {
                        int swap = a;
                        a = b;
                        b = swap;
                    }
                    pickTriangle(submesh, a, b, c, points, normals, local, culling,
                                 clockwise != mirrored, best);
                }
                offset += length;
            }
        }
    }

    private static float determinant3x3(float[] m) {
        return m[0] * (m[5] * m[10] - m[6] * m[9]) - m[1] * (m[4] * m[10] - m[6] * m[8]) +
            m[2] * (m[4] * m[9] - m[5] * m[8]);
    }

    // Moeller-Trumbore on the ray in mesh coordinates, whose distances equal
    // those in the group's coordinates. Seen along the ray, a, b and c run
    // counterclockwise round a front face, or clockwise when reversed is set.
    private void pickTriangle(int submesh, int a, int b, int c, float[] points, float[] normals,
                              float[] ray, int culling, boolean reversed, RayIntersection best) {
        int ia = 3 * a, ib = 3 * b, ic = 3 * c;
        float e1x = points[ib] - points[ia];
        float e1y = points[ib + 1] - points[ia + 1];
        float e1z = points[ib + 2] - points[ia + 2];
        float e2x = points[ic] - points[ia];
        float e2y = points[ic + 1] - points[ia + 1];
        float e2z = points[ic + 2] - points[ia + 2];
        float px = ray[5] * e2z - ray[6] * e2y;
        float py = ray[6] * e2x - ray[4] * e2z;
        float pz = ray[4] * e2y - ray[5] * e2x;
        float determinant = e1x * px + e1y * py + e1z * pz;
        if (determinant == 0) return;
        // The determinant is positive when a, b and c run counterclockwise.
        boolean front = (determinant > 0) != reversed;
        if ((culling == PolygonMode.CULL_BACK && !front) ||
                (culling == PolygonMode.CULL_FRONT && front)) {
            return;
        }
        float tx = ray[0] - points[ia];
        float ty = ray[1] - points[ia + 1];
        float tz = ray[2] - points[ia + 2];
        float u = (tx * px + ty * py + tz * pz) / determinant;
        if (!(u >= 0 && u <= 1)) return;
        float qx = ty * e1z - tz * e1y;
        float qy = tz * e1x - tx * e1z;
        float qz = tx * e1y - ty * e1x;
        float v = (ray[4] * qx + ray[5] * qy + ray[6] * qz) / determinant;
        if (!(v >= 0 && u + v <= 1)) return;
        float distance = (e2x * qx + e2y * qy + e2z * qz) / determinant;
        if (!(distance >= 0 && distance <= best.distance) ||
                (best.node != null && distance == best.distance)) {
            return;
        }

        best.node = this;
        best.distance = distance;
        best.submesh = submesh;
        float wa = 1 - u - v;
        if (normals != null) {
            for (int axis = 0; axis < 3; axis++) {
                best.normal[axis] = wa * normals[ia + axis] + u * normals[ib + axis] +
                    v * normals[ic + axis];
            }
        } else if (vertices.normals != null) {
            for (int axis = 0; axis < 3; axis++) {
                best.normal[axis] = wa * vertices.normals.get(a, axis) +
                    u * vertices.normals.get(b, axis) + v * vertices.normals.get(c, axis);
            }
        } else {
            best.normal[0] = e1y * e2z - e1z * e2y;
            best.normal[1] = e1z * e2x - e1x * e2z;
            best.normal[2] = e1x * e2y - e1y * e2x;
        }
        Graphics3D.normalize(best.normal);
        float[] corners = new float[6];
        for (int unit = 0; unit < 2; unit++) {
            VertexArray coordinates = vertices.texCoords[unit];
            if (coordinates == null) {
                best.textureS[unit] = best.textureT[unit] = 0;
                continue;
            }
            Texture2D texture = appearances[submesh].textures[unit];
            Transform transform = null;
            if (texture != null) {
                transform = new Transform();
                texture.getCompositeTransform(transform);
            }
            float[] scaleBias = vertices.texTransform[unit];
            textureCorner(coordinates, scaleBias, transform, a, corners, 0);
            textureCorner(coordinates, scaleBias, transform, b, corners, 2);
            textureCorner(coordinates, scaleBias, transform, c, corners, 4);
            best.textureS[unit] = wa * corners[0] + u * corners[2] + v * corners[4];
            best.textureT[unit] = wa * corners[1] + u * corners[3] + v * corners[5];
        }
    }

    // S and T of one vertex as the renderer computes them before it
    // interpolates across the triangle: scaled and biased, then transformed
    // with R (0 for two components) and divided by Q at the vertex.
    private static void textureCorner(VertexArray coordinates, float[] scaleBias,
                                      Transform transform, int vertex, float[] st,
                                      int offset) {
        float s = scaleBias[0] * coordinates.get(vertex, 0) + scaleBias[1];
        float t = scaleBias[0] * coordinates.get(vertex, 1) + scaleBias[2];
        if (transform == null) {
            st[offset] = s;
            st[offset + 1] = t;
            return;
        }
        float r = coordinates.components > 2 ?
            scaleBias[0] * coordinates.get(vertex, 2) + scaleBias[3] : 0;
        float[] stq = new float[4];
        transform.apply(s, t, r, 1, stq, 0);
        st[offset] = stq[0] / stq[3];
        st[offset + 1] = stq[1] / stq[3];
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
