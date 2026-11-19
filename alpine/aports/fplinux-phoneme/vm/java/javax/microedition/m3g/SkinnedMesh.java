// SPDX-License-Identifier: GPL-2.0-only
package javax.microedition.m3g;

import java.util.Hashtable;
import java.util.Vector;

public class SkinnedMesh extends Mesh {
    private static final int MAX_VERTICES = 65535;

    private Group skeleton;
    private final Vector bones = new Vector();
    private Influence[] influences = new Influence[0];
    private int vertexLimit;

    public SkinnedMesh(VertexBuffer vertices, IndexBuffer triangles, Appearance look,
                       Group skeleton) {
        this(vertices, new IndexBuffer[] { triangles }, new Appearance[] { look }, skeleton);
    }

    public SkinnedMesh(VertexBuffer vertices, IndexBuffer[] surfaces, Appearance[] looks,
                       Group skeleton) {
        this(vertices, surfaces, looks);
        attachSkeleton(skeleton);
    }

    private SkinnedMesh(VertexBuffer vertices, IndexBuffer[] surfaces, Appearance[] looks) {
        super(vertices, surfaces, looks);
    }

    private void attachSkeleton(Group group) {
        if (group == null) throw new NullPointerException();
        if (group instanceof World || group.parent != null) {
            throw new IllegalArgumentException("skinned mesh skeleton");
        }
        skeleton = group;
        group.parent = this;
    }

    public Group getSkeleton() {
        return skeleton;
    }

    private void requireBone(Node node) {
        if (node == null) throw new NullPointerException();
        for (Node ancestor = node; ancestor != null; ancestor = ancestor.parent) {
            if (ancestor == skeleton) return;
        }
        throw new IllegalArgumentException("bone outside skeleton");
    }

    private Bone findBone(Node node) {
        for (int i = 0; i < bones.size(); i++) {
            Bone bone = (Bone) bones.elementAt(i);
            if (bone.node == node) return bone;
        }
        return null;
    }

    public void addTransform(Node node, int weight, int firstVertex, int numVertices) {
        requireBone(node);
        if (weight <= 0 || numVertices <= 0) {
            throw new IllegalArgumentException("bone weight or vertex count");
        }
        if (firstVertex < 0 || firstVertex > MAX_VERTICES - numVertices) {
            throw new IndexOutOfBoundsException("bone vertex range");
        }

        Transform bind = new Transform();
        boneToMesh(node, bind, new Transform());
        bind.invert();
        Bone bone = findBone(node);
        if (bone == null) {
            bone = new Bone(node, bind);
            bones.addElement(bone);
        } else {
            bone.bind.set(bind);
        }

        int end = firstVertex + numVertices;
        if (end > influences.length) {
            int capacity = Math.max(end, Math.min(MAX_VERTICES, influences.length * 2));
            Influence[] grown = new Influence[capacity];
            System.arraycopy(influences, 0, grown, 0, vertexLimit);
            influences = grown;
        }
        vertexLimit = Math.max(vertexLimit, end);
        for (int i = firstVertex; i < end; i++) {
            Influence head = influences[i];
            Influence entry = head;
            long total = weight;
            for (Influence current = head; current != null; current = current.next) {
                total += current.weight;
                if (current.bone == bone) entry = current;
            }
            if (entry == null || entry.bone != bone) {
                entry = new Influence(bone, head);
                influences[i] = entry;
            }
            entry.weight += weight;
            for (Influence current = influences[i]; current != null; current = current.next) {
                current.fraction = (float) current.weight / total;
            }
        }
    }

    public void getBoneTransform(Node node, Transform transform) {
        requireBone(node);
        if (transform == null) throw new NullPointerException();
        Bone bone = findBone(node);
        if (bone == null) transform.setIdentity();
        else transform.set(bone.bind);
    }

    public int getBoneVertices(Node node, int[] indices, float[] weights) {
        requireBone(node);
        Bone bone = findBone(node);
        if (bone == null) return 0;
        int count = 0;
        for (int i = 0; i < vertexLimit; i++) {
            for (Influence entry = influences[i]; entry != null; entry = entry.next) {
                if (entry.bone != bone) continue;
                if (indices != null && weights != null) {
                    if (count >= indices.length || count >= weights.length) {
                        throw new IllegalArgumentException("bone vertex output arrays");
                    }
                    indices[count] = i;
                    weights[count] = entry.fraction;
                }
                count++;
            }
        }
        return count;
    }

    private void boneToMesh(Node node, Transform result, Transform local) {
        result.setIdentity();
        for (Node ancestor = node; ancestor != this; ancestor = ancestor.parent) {
            if (ancestor == null) throw new IllegalStateException("detached bone");
            ancestor.getCompositeTransform(local);
            local.postMultiply(result);
            result.set(local);
        }
    }

    void prepareSkinning() {
        if (vertexLimit > vertices.getVertexCount()) {
            throw new IllegalStateException("skinned mesh vertex range");
        }
        if (bones.size() == 0) return;
        Transform local = new Transform();
        for (int i = 0; i < bones.size(); i++) {
            Bone bone = (Bone) bones.elementAt(i);
            boneToMesh(bone.node, bone.pose, local);
            // Work in mesh space so the scene transform is applied once by the renderer.
            bone.pose.postMultiply(bone.bind);
            if (vertices.normals != null) {
                bone.normal.set(bone.pose);
                bone.normal.invert();
            }
        }
    }

    void skin(int index, float[] position, float[] normal) {
        if (index >= vertexLimit || influences[index] == null) return;
        float x = position[0], y = position[1], z = position[2];
        position[0] = position[1] = position[2] = 0;
        float nx = 0, ny = 0, nz = 0;
        if (normal != null) {
            nx = normal[0];
            ny = normal[1];
            nz = normal[2];
            normal[0] = normal[1] = normal[2] = 0;
        }
        for (Influence entry = influences[index]; entry != null; entry = entry.next) {
            float[] pose = entry.bone.pose.matrix;
            float fraction = entry.fraction;
            for (int axis = 0; axis < 3; axis++) {
                int row = axis * 4;
                position[axis] += fraction * (pose[row] * x + pose[row + 1] * y +
                                              pose[row + 2] * z + pose[row + 3]);
            }
            if (normal != null) {
                // Multiply by the transpose of the inverse, ignoring translation.
                float[] inverse = entry.bone.normal.matrix;
                for (int axis = 0; axis < 3; axis++) {
                    normal[axis] += fraction * (inverse[axis] * nx + inverse[axis + 4] * ny +
                                                inverse[axis + 8] * nz);
                }
            }
        }
    }

    void references(Vector objects) {
        super.references(objects);
        objects.addElement(skeleton);
    }

    Object3D newCopy() {
        return new SkinnedMesh(vertices, indices, appearances);
    }

    void copyFields(Object3D copy, Hashtable copies) {
        super.copyFields(copy, copies);
        SkinnedMesh target = (SkinnedMesh) copy;
        target.attachSkeleton((Group) skeleton.duplicate(copies));
        Hashtable mapped = new Hashtable();
        for (int i = 0; i < bones.size(); i++) {
            Bone bone = (Bone) bones.elementAt(i);
            Bone cloned = new Bone((Node) bone.node.duplicate(copies), new Transform(bone.bind));
            target.bones.addElement(cloned);
            mapped.put(bone, cloned);
        }
        target.vertexLimit = vertexLimit;
        target.influences = new Influence[vertexLimit];
        for (int i = 0; i < vertexLimit; i++) {
            for (Influence entry = influences[i]; entry != null; entry = entry.next) {
                Influence cloned = new Influence((Bone) mapped.get(entry.bone), target.influences[i]);
                cloned.weight = entry.weight;
                cloned.fraction = entry.fraction;
                target.influences[i] = cloned;
            }
        }
    }

    private static final class Bone {
        final Node node;
        final Transform bind;
        final Transform pose = new Transform();
        final Transform normal = new Transform();

        Bone(Node node, Transform bind) {
            this.node = node;
            this.bind = bind;
        }
    }

    private static final class Influence {
        final Bone bone;
        final Influence next;
        long weight;
        float fraction;

        Influence(Bone bone, Influence next) {
            this.bone = bone;
            this.next = next;
        }
    }
}
