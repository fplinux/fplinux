// SPDX-License-Identifier: GPL-2.0-only
package javax.microedition.m3g;

import java.util.Hashtable;
import java.util.Vector;

public class World extends Group {
    private Camera activeCamera;
    private Background background;

    public Camera getActiveCamera() {
        return activeCamera;
    }

    public void setActiveCamera(Camera camera) {
        activeCamera = camera;
    }

    public Background getBackground() {
        return background;
    }

    public void setBackground(Background value) {
        background = value;
    }

    void references(Vector objects) {
        super.references(objects);
        addReference(objects, activeCamera);
        addReference(objects, background);
    }

    Object3D newCopy() {
        return new World();
    }

    void copyFields(Object3D copy, Hashtable copies) {
        super.copyFields(copy, copies);
        World target = (World) copy;
        Camera mapped = activeCamera == null ? null : (Camera) copies.get(activeCamera);
        target.activeCamera = mapped == null ? activeCamera : mapped;
        target.background = background;
    }
}
