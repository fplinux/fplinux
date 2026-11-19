// SPDX-License-Identifier: GPL-2.0-only
package javax.microedition.m3g;

import java.util.Hashtable;
import java.util.Vector;

public class MorphingMesh extends Mesh {
    private final VertexBuffer[] targets;
    private final float[] weights;

    public MorphingMesh(VertexBuffer base, VertexBuffer[] targets, IndexBuffer submesh,
                        Appearance appearance) {
        this(base, targets, new IndexBuffer[] { submesh }, new Appearance[] { appearance });
    }

    public MorphingMesh(VertexBuffer base, VertexBuffer[] targets, IndexBuffer[] submeshes,
                        Appearance[] appearances) {
        super(base, submeshes, appearances);
        if (targets == null) throw new NullPointerException();
        if (targets.length == 0) throw new IllegalArgumentException("morph targets");
        this.targets = new VertexBuffer[targets.length];
        for (int i = 0; i < targets.length; i++) {
            if (targets[i] == null) throw new NullPointerException();
            this.targets[i] = targets[i];
        }
        weights = new float[targets.length];
    }

    public VertexBuffer getMorphTarget(int index) {
        return targets[index];
    }

    public int getMorphTargetCount() {
        return targets.length;
    }

    public void setWeights(float[] values) {
        checkWeightCount(values);
        System.arraycopy(values, 0, weights, 0, weights.length);
    }

    public void getWeights(float[] values) {
        checkWeightCount(values);
        System.arraycopy(weights, 0, values, 0, weights.length);
    }

    private void checkWeightCount(float[] values) {
        if (values == null) throw new NullPointerException();
        if (values.length < weights.length) throw new IllegalArgumentException("morph weights");
    }

    void animated(int property, float[] values) {
        if (property == AnimationTrack.MORPH_WEIGHTS) {
            setWeights(values);
        } else {
            super.animated(property, values);
        }
    }

    void prepareMorphing() {
        VertexBuffer first = targets[0];
        checkBaseFormat(vertices.positions, first.positions);
        checkBaseFormat(vertices.normals, first.normals);
        for (int i = 1; i < targets.length; i++) {
            checkFormat(first.positions, targets[i].positions);
            checkFormat(first.normals, targets[i].normals);
        }
    }

    private static void checkBaseFormat(VertexArray base, VertexArray target) {
        if (target != null) checkFormat(base, target);
    }

    private static void checkFormat(VertexArray a, VertexArray b) {
        if (a != b && (a == null || b == null || a.vertices != b.vertices ||
                a.components != b.components || a.componentSize != b.componentSize)) {
            throw new IllegalStateException("morph target vertex format");
        }
    }

    // Apply raw target deltas to the decoded base position and the optional base normal.
    void morph(int index, float[] position, float[] normal) {
        for (int i = 0; i < targets.length; i++) {
            float weight = weights[i];
            if (weight == 0) continue;
            VertexBuffer target = targets[i];
            if (target.positions != null && target.positions != vertices.positions) {
                addDelta(index, position, vertices.positions, target.positions,
                         weight * vertices.position[0]);
            }
            if (normal != null && target.normals != null && target.normals != vertices.normals) {
                addDelta(index, normal, vertices.normals, target.normals, weight);
            }
        }
    }

    private static void addDelta(int index, float[] value, VertexArray base,
                                 VertexArray target, float weight) {
        for (int axis = 0; axis < 3; axis++) {
            value[axis] += weight * (target.get(index, axis) - base.get(index, axis));
        }
    }

    void references(Vector objects) {
        super.references(objects);
        for (int i = 0; i < targets.length; i++) objects.addElement(targets[i]);
    }

    Object3D newCopy() {
        return new MorphingMesh(vertices, targets, indices, appearances);
    }

    void copyFields(Object3D copy, Hashtable copies) {
        super.copyFields(copy, copies);
        ((MorphingMesh) copy).setWeights(weights);
    }
}
