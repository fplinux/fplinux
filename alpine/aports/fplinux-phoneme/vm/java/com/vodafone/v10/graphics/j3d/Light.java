// SPDX-License-Identifier: GPL-2.0-only
package com.vodafone.v10.graphics.j3d;

public class Light {
    final com.mascotcapsule.micro3d.v3.Light micro3d = new com.mascotcapsule.micro3d.v3.Light();

    public Light() {
        micro3d.setDirection(new Vector3D(micro3d.getDirection()));
    }

    public Light(Vector3D direction, int dirIntensity, int ambIntensity) {
        micro3d.setDirection(new Vector3D(direction));
        micro3d.setDirIntensity(dirIntensity);
        micro3d.setAmbIntensity(ambIntensity);
    }

    public Vector3D getDirection() {
        return (Vector3D) micro3d.getDirection();
    }

    public void setDirection(Vector3D direction) {
        micro3d.setDirection(direction);
    }

    public int getDirIntensity() {
        return micro3d.getDirIntensity();
    }

    public void setDirIntensity(int intensity) {
        micro3d.setDirIntensity(intensity);
    }

    public int getAmbIntensity() {
        return micro3d.getAmbIntensity();
    }

    public void setAmbIntensity(int intensity) {
        micro3d.setAmbIntensity(intensity);
    }
}
