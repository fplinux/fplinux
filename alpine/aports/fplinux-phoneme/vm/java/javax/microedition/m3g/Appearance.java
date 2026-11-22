// SPDX-License-Identifier: GPL-2.0-only
package javax.microedition.m3g;

import java.util.Hashtable;
import java.util.Vector;

public class Appearance extends Object3D {
    final Texture2D[] textures = new Texture2D[2];
    Material material;
    Fog fog;
    PolygonMode polygon;
    CompositingMode compositing;
    int layer;

    public void setPolygonMode(PolygonMode value) {
        polygon = value;
    }

    public PolygonMode getPolygonMode() {
        return polygon;
    }

    public void setTexture(int index, Texture2D value) {
        textures[index] = value;
    }

    public Texture2D getTexture(int index) {
        return textures[index];
    }

    public void setCompositingMode(CompositingMode value) {
        compositing = value;
    }

    public CompositingMode getCompositingMode() {
        return compositing;
    }

    public Material getMaterial() {
        return material;
    }

    public void setMaterial(Material value) {
        material = value;
    }

    public void setLayer(int index) {
        if (index < -63 || index > 64) throw new IllegalArgumentException("appearance layer");
        layer = index;
    }

    public int getLayer() {
        return layer;
    }

    public void setFog(Fog value) {
        fog = value;
    }

    public Fog getFog() {
        return fog;
    }

    void references(Vector objects) {
        super.references(objects);
        addReference(objects, material);
        addReference(objects, fog);
        addReference(objects, polygon);
        addReference(objects, compositing);
        for (int i = 0; i < textures.length; i++) addReference(objects, textures[i]);
    }

    Object3D newCopy() {
        return new Appearance();
    }

    void copyFields(Object3D copy, Hashtable copies) {
        super.copyFields(copy, copies);
        Appearance target = (Appearance) copy;
        target.material = material;
        target.fog = fog;
        target.polygon = polygon;
        target.compositing = compositing;
        target.layer = layer;
        System.arraycopy(textures, 0, target.textures, 0, textures.length);
    }
}
