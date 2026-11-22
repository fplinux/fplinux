// SPDX-License-Identifier: GPL-2.0-only
package javax.microedition.m3g;

import java.util.Hashtable;
import java.util.Vector;

public class Group extends Node {
    private final Vector children = new Vector();

    public void addChild(Node child) {
        if (child == null) {
            throw new NullPointerException();
        }
        if (child.parent == this) {
            return;
        }
        if (child.parent != null) {
            throw new IllegalArgumentException("node already has a parent");
        }
        for (Node node = this; node != null; node = node.parent) {
            if (node == child) {
                throw new IllegalArgumentException("group cycle");
            }
        }
        children.addElement(child);
        child.parent = this;
    }

    public void removeChild(Node child) {
        if (children.removeElement(child)) {
            child.parent = null;
        }
    }

    public Node getChild(int index) {
        return (Node) children.elementAt(index);
    }

    public int getChildCount() {
        return children.size();
    }

    // The ray starts at (ox, oy, oz) in this group's coordinates and has the
    // distance 1 at one direction vector from there. The group's own transform
    // does not apply. A null ri asks only whether the ray hits anything.
    public boolean pick(int scope, float ox, float oy, float oz, float dx, float dy, float dz,
                        RayIntersection ri) {
        if (dx == 0 && dy == 0 && dz == 0) throw new IllegalArgumentException("pick direction");
        return pickRay(scope, new float[] { ox, oy, oz, dx, dy, dz }, ri);
    }

    // The point (x, y) is in the camera's viewport, from (0, 0) at its upper left
    // to (1, 1). The ray starts on the near plane (distance 0) and passes the far
    // plane at distance 1 without ending there.
    public boolean pick(int scope, float x, float y, Camera camera, RayIntersection ri) {
        if (camera == null) throw new NullPointerException();
        Transform toGroup = new Transform();
        if (!camera.getTransformTo(this, toGroup)) {
            throw new IllegalStateException("camera outside the group's scene");
        }
        Transform unproject = new Transform(camera.projection);
        unproject.invert();
        float[] ends = new float[8];
        unproject.apply(2 * x - 1, 1 - 2 * y, -1, 1, ends, 0);
        unproject.apply(2 * x - 1, 1 - 2 * y, 1, 1, ends, 4);
        for (int end = 0; end < 8; end += 4) {
            toGroup.apply(ends[end] / ends[end + 3], ends[end + 1] / ends[end + 3],
                          ends[end + 2] / ends[end + 3], 1, ends, end);
        }
        float[] ray = {
            ends[0], ends[1], ends[2],
            ends[4] - ends[0], ends[5] - ends[1], ends[6] - ends[2],
        };
        return pickRay(scope, ray, ri);
    }

    private boolean pickRay(int scope, float[] ray, RayIntersection result) {
        if (!picking) return false;
        RayIntersection nearest = new RayIntersection();
        nearest.distance = Float.MAX_VALUE;
        pickNode(scope, new Transform(), ray, nearest);
        if (nearest.node == null) return false;
        if (result != null) {
            System.arraycopy(ray, 0, nearest.ray, 0, 6);
            result.set(nearest);
        }
        return true;
    }

    void pickNode(int pickScope, Transform model, float[] ray, RayIntersection best) {
        for (int i = 0; i < children.size(); i++) {
            getChild(i).pickWithin(pickScope, model, ray, best);
        }
    }

    void references(Vector objects) {
        super.references(objects);
        for (int i = 0; i < children.size(); i++) {
            objects.addElement(children.elementAt(i));
        }
    }

    Object3D newCopy() {
        return new Group();
    }

    void copyFields(Object3D copy, Hashtable copies) {
        super.copyFields(copy, copies);
        Group target = (Group) copy;
        for (int i = 0; i < children.size(); i++) {
            target.addChild((Node) getChild(i).duplicate(copies));
        }
    }
}
