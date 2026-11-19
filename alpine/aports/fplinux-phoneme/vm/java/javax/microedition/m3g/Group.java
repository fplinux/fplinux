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
