// SPDX-License-Identifier: GPL-2.0-only
package javax.microedition.m3g;

import java.util.Hashtable;

public class AnimationController extends Object3D {
    private int lower, upper;
    private float speed = 1, weight = 1, reference;
    private int referenceTime;

    public void setActiveInterval(int first, int last) {
        if (first > last) {
            throw new IllegalArgumentException("animation interval");
        }
        lower = first;
        upper = last;
    }

    public int getActiveIntervalStart() {
        return lower;
    }

    public int getActiveIntervalEnd() {
        return upper;
    }

    public void setPosition(float position, int worldTime) {
        reference = position;
        referenceTime = worldTime;
    }

    public int getRefWorldTime() {
        return referenceTime;
    }

    public float getPosition(int worldTime) {
        return reference + speed * ((long) worldTime - referenceTime);
    }

    public void setSpeed(float value, int worldTime) {
        setPosition(getPosition(worldTime), worldTime);
        speed = value;
    }

    public float getSpeed() {
        return speed;
    }

    public void setWeight(float value) {
        if (value < 0) {
            throw new IllegalArgumentException("animation weight");
        }
        weight = value;
    }

    public float getWeight() {
        return weight;
    }

    Object3D newCopy() {
        return new AnimationController();
    }

    void copyFields(Object3D copy, Hashtable copies) {
        super.copyFields(copy, copies);
        AnimationController target = (AnimationController) copy;
        target.lower = lower;
        target.upper = upper;
        target.speed = speed;
        target.weight = weight;
        target.setPosition(reference, referenceTime);
    }
}
