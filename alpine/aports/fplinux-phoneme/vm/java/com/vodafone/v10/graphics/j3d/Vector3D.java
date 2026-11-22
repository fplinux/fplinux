// SPDX-License-Identifier: GPL-2.0-only
package com.vodafone.v10.graphics.j3d;

public class Vector3D extends com.mascotcapsule.micro3d.v3.Vector3D {
    public Vector3D() {
    }

    public Vector3D(int x, int y, int z) {
        super(x, y, z);
    }

    Vector3D(com.mascotcapsule.micro3d.v3.Vector3D vector) {
        super(vector);
    }

    public int innerProduct(Vector3D vector) {
        return super.innerProduct(vector);
    }

    public void outerProduct(Vector3D vector) {
        super.outerProduct(vector);
    }

    public static int innerProduct(Vector3D left, Vector3D right) {
        return com.mascotcapsule.micro3d.v3.Vector3D.innerProduct(left, right);
    }

    public static Vector3D outerProduct(Vector3D left, Vector3D right) {
        return new Vector3D(com.mascotcapsule.micro3d.v3.Vector3D.outerProduct(left, right));
    }
}
