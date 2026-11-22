// SPDX-License-Identifier: GPL-2.0-only
package javax.microedition.m3g;

import java.util.Hashtable;
import java.util.Vector;

public class Texture2D extends Transformable {
    public static final int FILTER_BASE_LEVEL = 208;
    public static final int FILTER_LINEAR = 209;
    public static final int FILTER_NEAREST = 210;
    public static final int FUNC_ADD = 224;
    public static final int FUNC_BLEND = 225;
    public static final int FUNC_DECAL = 226;
    public static final int FUNC_MODULATE = 227;
    public static final int FUNC_REPLACE = 228;
    public static final int WRAP_CLAMP = 240;
    public static final int WRAP_REPEAT = 241;

    Image2D image;
    int blending = FUNC_MODULATE;
    int wrapS = WRAP_REPEAT, wrapT = WRAP_REPEAT;
    private int levelFilter = FILTER_BASE_LEVEL;
    int imageFilter = FILTER_NEAREST;
    int blendColor;

    public Texture2D(Image2D image) {
        this.image = requireImage(image);
    }

    public void setImage(Image2D image) {
        this.image = requireImage(image);
    }

    private static Image2D requireImage(Image2D image) {
        if (image == null) {
            throw new NullPointerException();
        }
        return image;
    }

    public Image2D getImage() {
        return image;
    }

    public void setBlending(int function) {
        if (function < FUNC_ADD || function > FUNC_REPLACE) {
            throw new IllegalArgumentException("texture function");
        }
        blending = function;
    }

    public int getBlending() {
        return blending;
    }

    public void setBlendColor(int rgb) {
        blendColor = rgb & 0x00ffffff;
    }

    public int getBlendColor() {
        return blendColor;
    }

    public int getWrappingS() {
        return wrapS;
    }

    public int getWrappingT() {
        return wrapT;
    }

    public void setWrapping(int horizontal, int vertical) {
        if ((horizontal != WRAP_CLAMP && horizontal != WRAP_REPEAT) ||
                (vertical != WRAP_CLAMP && vertical != WRAP_REPEAT)) {
            throw new IllegalArgumentException("texture wrapping");
        }
        wrapS = horizontal;
        wrapT = vertical;
    }

    public void setFiltering(int levelFilter, int imageFilter) {
        if ((levelFilter != FILTER_BASE_LEVEL && levelFilter != FILTER_LINEAR &&
                levelFilter != FILTER_NEAREST) ||
                (imageFilter != FILTER_LINEAR && imageFilter != FILTER_NEAREST)) {
            throw new IllegalArgumentException("texture filtering");
        }
        // The renderer samples the base image and preserves the requested level hint.
        this.levelFilter = levelFilter;
        this.imageFilter = imageFilter;
    }

    public int getLevelFilter() {
        return levelFilter;
    }

    public int getImageFilter() {
        return imageFilter;
    }

    void references(Vector objects) {
        super.references(objects);
        objects.addElement(image);
    }

    Object3D newCopy() {
        return new Texture2D(image);
    }

    void copyFields(Object3D copy, Hashtable copies) {
        super.copyFields(copy, copies);
        Texture2D target = (Texture2D) copy;
        target.blending = blending;
        target.wrapS = wrapS;
        target.wrapT = wrapT;
        target.levelFilter = levelFilter;
        target.imageFilter = imageFilter;
        target.blendColor = blendColor;
    }
}
