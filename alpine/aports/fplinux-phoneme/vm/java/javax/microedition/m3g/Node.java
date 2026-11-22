// SPDX-License-Identifier: GPL-2.0-only
package javax.microedition.m3g;

import java.util.Hashtable;

public abstract class Node extends Transformable {
    public static final int NONE = 144;
    public static final int ORIGIN = 145;
    public static final int X_AXIS = 146;
    public static final int Y_AXIS = 147;
    public static final int Z_AXIS = 148;

    Node parent;
    boolean rendering = true;
    boolean picking = true;
    int scope = -1;
    float alpha = 1;
    // Alignment settings are kept for the getters; rendering does not apply them.
    private Node zReference, yReference;
    private int zTarget = NONE, yTarget = NONE;

    public Node getParent() {
        return parent;
    }

    public void setScope(int value) {
        scope = value;
    }

    public int getScope() {
        return scope;
    }

    public void setAlphaFactor(float factor) {
        if (!(factor >= 0 && factor <= 1)) throw new IllegalArgumentException("alpha factor");
        alpha = factor;
    }

    public float getAlphaFactor() {
        return alpha;
    }

    public void setAlignment(Node zRef, int zTargetAxis, Node yRef, int yTargetAxis) {
        if (zTargetAxis < NONE || zTargetAxis > Z_AXIS || yTargetAxis < NONE ||
                yTargetAxis > Z_AXIS || zRef == this || yRef == this) {
            throw new IllegalArgumentException("alignment");
        }
        zReference = zRef;
        zTarget = zTargetAxis;
        yReference = yRef;
        yTarget = yTargetAxis;
    }

    public Node getAlignmentReference(int axis) {
        checkAlignmentAxis(axis);
        return axis == Z_AXIS ? zReference : yReference;
    }

    public int getAlignmentTarget(int axis) {
        checkAlignmentAxis(axis);
        return axis == Z_AXIS ? zTarget : yTarget;
    }

    private static void checkAlignmentAxis(int axis) {
        if (axis != Y_AXIS && axis != Z_AXIS) throw new IllegalArgumentException("alignment axis");
    }

    public boolean getTransformTo(Node target, Transform transform) {
        if (target == null || transform == null) throw new NullPointerException();
        if (root() != target.root()) return false;
        Transform result = target.worldTransform();
        try {
            result.invert();
        } catch (ArithmeticException singular) {
            return false;
        }
        result.postMultiply(worldTransform());
        transform.set(result);
        return true;
    }

    private Node root() {
        Node node = this;
        while (node.parent != null) node = node.parent;
        return node;
    }

    // Tests the node's geometry against a ray given in the coordinates of the
    // picked group, where model maps the node's own coordinates to those of the
    // group. A closer hit replaces the one held in best.
    void pickNode(int pickScope, Transform model, float[] ray, RayIntersection best) {}

    final void pickWithin(int pickScope, Transform parentModel, float[] ray,
                          RayIntersection best) {
        if (!picking) return;
        Transform model = new Transform();
        getCompositeTransform(model);
        Transform toGroup = new Transform(parentModel);
        toGroup.postMultiply(model);
        pickNode(pickScope, toGroup, ray, best);
    }

    public void setRenderingEnable(boolean enabled) {
        rendering = enabled;
    }

    public boolean isRenderingEnabled() {
        return rendering;
    }

    public void setPickingEnable(boolean enabled) {
        picking = enabled;
    }

    public boolean isPickingEnabled() {
        return picking;
    }

    Transform worldTransform() {
        Transform local = new Transform();
        getCompositeTransform(local);
        if (parent == null) {
            return local;
        }
        Transform result = parent.worldTransform();
        result.postMultiply(local);
        return result;
    }

    void animated(int property, float[] values) {
        if (property == AnimationTrack.VISIBILITY) {
            rendering = values[0] >= .5f;
        } else if (property == AnimationTrack.PICKABILITY) {
            picking = values[0] >= .5f;
        } else if (property == AnimationTrack.ALPHA) {
            alpha = Math.min(1, Math.max(0, values[0]));
        } else {
            super.animated(property, values);
        }
    }

    void copyFields(Object3D copy, Hashtable copies) {
        copyNodeFields((Node) copy, copies);
    }

    final void copyNodeFields(Node target, Hashtable copies) {
        super.copyFields(target, copies);
        target.rendering = rendering;
        target.picking = picking;
        target.scope = scope;
        target.alpha = alpha;
        target.zReference = zReference;
        target.zTarget = zTarget;
        target.yReference = yReference;
        target.yTarget = yTarget;
    }
}
