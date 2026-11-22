// SPDX-License-Identifier: GPL-2.0-only
package javax.microedition.m3g;

import java.util.Hashtable;

public class Light extends Node {
    public static final int AMBIENT = 128;
    public static final int DIRECTIONAL = 129;
    public static final int OMNI = 130;
    public static final int SPOT = 131;

    int mode = DIRECTIONAL;
    int color = 0xffffff;
    float intensity = 1;
    // The renderer does not light with omni and spot lights; their attenuation
    // and spot settings are only kept for the getters.
    private float constant = 1, linear, quadratic;
    private float spotAngle = 45, spotExponent;

    public void setMode(int value) {
        if (value < AMBIENT || value > SPOT) {
            throw new IllegalArgumentException("light mode");
        }
        mode = value;
    }

    public int getMode() {
        return mode;
    }

    public void setColor(int rgb) {
        color = rgb & 0x00ffffff;
    }

    public int getColor() {
        return color;
    }

    public void setIntensity(float value) {
        if (value < 0) {
            throw new IllegalArgumentException("light intensity");
        }
        intensity = value;
    }

    public float getIntensity() {
        return intensity;
    }

    public void setAttenuation(float constantTerm, float linearTerm, float quadraticTerm) {
        if (constantTerm < 0 || linearTerm < 0 || quadraticTerm < 0 ||
                (constantTerm == 0 && linearTerm == 0 && quadraticTerm == 0)) {
            throw new IllegalArgumentException("light attenuation");
        }
        constant = constantTerm;
        linear = linearTerm;
        quadratic = quadraticTerm;
    }

    public float getConstantAttenuation() {
        return constant;
    }

    public float getLinearAttenuation() {
        return linear;
    }

    public float getQuadraticAttenuation() {
        return quadratic;
    }

    public void setSpotAngle(float angle) {
        if (!(angle >= 0 && angle <= 90)) throw new IllegalArgumentException("spot angle");
        spotAngle = angle;
    }

    public float getSpotAngle() {
        return spotAngle;
    }

    public void setSpotExponent(float exponent) {
        if (!(exponent >= 0 && exponent <= 128)) throw new IllegalArgumentException("spot exponent");
        spotExponent = exponent;
    }

    public float getSpotExponent() {
        return spotExponent;
    }

    void animated(int property, float[] values) {
        if (property == AnimationTrack.INTENSITY) {
            setIntensity(values[0]);
        } else {
            super.animated(property, values);
        }
    }

    Object3D newCopy() {
        return new Light();
    }

    void copyFields(Object3D copy, Hashtable copies) {
        super.copyFields(copy, copies);
        Light target = (Light) copy;
        target.mode = mode;
        target.color = color;
        target.intensity = intensity;
        target.constant = constant;
        target.linear = linear;
        target.quadratic = quadratic;
        target.spotAngle = spotAngle;
        target.spotExponent = spotExponent;
    }
}
