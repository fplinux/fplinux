// SPDX-License-Identifier: GPL-2.0-only
package com.vodafone.v10.graphics.j3d;

public class AffineTrans extends com.mascotcapsule.micro3d.v3.AffineTrans {
    public AffineTrans() {
    }

    public AffineTrans(int[][] matrix) {
        super(matrix);
    }

    public Vector3D transPoint(Vector3D point) {
        return new Vector3D(super.transPoint(point));
    }

    public void multiply(AffineTrans transform) {
        super.multiply(transform);
    }

    public void multiply(AffineTrans left, AffineTrans right) {
        super.multiply(left, right);
    }

    public void rotationV(Vector3D axis, int angle) {
        super.rotationV(axis, angle);
    }

    public void setViewTrans(Vector3D position, Vector3D look, Vector3D up) {
        super.setViewTrans(position, look, up);
    }
}
