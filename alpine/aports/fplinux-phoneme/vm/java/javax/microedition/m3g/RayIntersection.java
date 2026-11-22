// SPDX-License-Identifier: GPL-2.0-only
package javax.microedition.m3g;

public class RayIntersection {
    // The pick fills these: the nearest hit on node, in the coordinates of the
    // node. The ray is in the coordinates of the picked group.
    Node node;
    float distance;
    int submesh;
    final float[] normal = new float[3];
    final float[] textureS = new float[2];
    final float[] textureT = new float[2];
    final float[] ray = new float[6];

    public RayIntersection() {}

    public Node getIntersected() {
        return node;
    }

    public float getDistance() {
        return distance;
    }

    public int getSubmeshIndex() {
        return submesh;
    }

    public float getNormalX() {
        return normal[0];
    }

    public float getNormalY() {
        return normal[1];
    }

    public float getNormalZ() {
        return normal[2];
    }

    public float getTextureS(int index) {
        return textureS[index];
    }

    public float getTextureT(int index) {
        return textureT[index];
    }

    public void getRay(float[] result) {
        if (result.length < ray.length) throw new IllegalArgumentException("ray");
        System.arraycopy(ray, 0, result, 0, ray.length);
    }

    void set(RayIntersection other) {
        node = other.node;
        distance = other.distance;
        submesh = other.submesh;
        System.arraycopy(other.normal, 0, normal, 0, normal.length);
        System.arraycopy(other.textureS, 0, textureS, 0, textureS.length);
        System.arraycopy(other.textureT, 0, textureT, 0, textureT.length);
        System.arraycopy(other.ray, 0, ray, 0, ray.length);
    }
}
