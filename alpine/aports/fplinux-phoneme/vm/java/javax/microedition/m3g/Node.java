// SPDX-License-Identifier: GPL-2.0-only
package javax.microedition.m3g;

import java.util.Hashtable;

public abstract class Node extends Transformable {
    Node parent;
    boolean rendering = true;
    boolean picking = true;
    int scope = -1;
    float alpha = 1;

    public Node getParent() {
        return parent;
    }

    public void setRenderingEnable(boolean enabled) {
        rendering = enabled;
    }

    public boolean isRenderingEnabled() {
        return rendering;
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
    }
}
