// SPDX-License-Identifier: GPL-2.0-only
package javax.microedition.m3g;

import java.util.Hashtable;
import java.util.Vector;

public class AnimationTrack extends Object3D {
    public static final int ALPHA = 256;
    public static final int COLOR = 258;
    public static final int FAR_DISTANCE = 263;
    public static final int FIELD_OF_VIEW = 264;
    public static final int INTENSITY = 265;
    public static final int MORPH_WEIGHTS = 266;
    public static final int NEAR_DISTANCE = 267;
    public static final int ORIENTATION = 268;
    public static final int PICKABILITY = 269;
    public static final int SCALE = 270;
    public static final int TRANSLATION = 275;
    public static final int VISIBILITY = 276;

    final KeyframeSequence sequence;
    final int property;
    private AnimationController controller;

    public AnimationTrack(KeyframeSequence values, int targetProperty) {
        if (values == null) {
            throw new NullPointerException();
        }
        if (targetProperty < ALPHA || targetProperty > VISIBILITY) {
            throw new IllegalArgumentException("animation property");
        }
        sequence = values;
        property = targetProperty;
    }

    public AnimationController getController() {
        return controller;
    }

    public void setController(AnimationController value) {
        controller = value;
    }

    public KeyframeSequence getKeyframeSequence() {
        return sequence;
    }

    public int getTargetProperty() {
        return property;
    }

    void references(Vector objects) {
        super.references(objects);
        addReference(objects, controller);
        objects.addElement(sequence);
    }

    Object3D newCopy() {
        return new AnimationTrack(sequence, property);
    }

    void copyFields(Object3D copy, Hashtable copies) {
        super.copyFields(copy, copies);
        ((AnimationTrack) copy).controller = controller;
    }
}
