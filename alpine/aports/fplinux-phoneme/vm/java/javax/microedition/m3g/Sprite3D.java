// SPDX-License-Identifier: GPL-2.0-only
package javax.microedition.m3g;

import java.util.Hashtable;
import java.util.Vector;

public class Sprite3D extends Node {
    private final boolean scaled;
    private Image2D image;
    private Appearance appearance;
    private int cropX, cropY, cropWidth, cropHeight;

    public Sprite3D(boolean scaled, Image2D image, Appearance appearance) {
        this.scaled = scaled;
        setImage(image);
        this.appearance = appearance;
    }

    public boolean isScaled() {
        return scaled;
    }

    public void setImage(Image2D value) {
        if (value == null) throw new NullPointerException();
        image = value;
        setCrop(0, 0, image.getWidth(), image.getHeight());
    }

    public Image2D getImage() {
        return image;
    }

    public void setAppearance(Appearance value) {
        appearance = value;
    }

    public Appearance getAppearance() {
        return appearance;
    }

    public void setCrop(int x, int y, int width, int height) {
        if (width == Integer.MIN_VALUE || height == Integer.MIN_VALUE) {
            throw new IllegalArgumentException("sprite crop dimensions");
        }
        cropX = x;
        cropY = y;
        cropWidth = width;
        cropHeight = height;
    }

    public int getCropX() {
        return cropX;
    }

    public int getCropY() {
        return cropY;
    }

    public int getCropWidth() {
        return cropWidth;
    }

    public int getCropHeight() {
        return cropHeight;
    }

    void references(Vector objects) {
        super.references(objects);
        addReference(objects, image);
        addReference(objects, appearance);
    }

    Object3D newCopy() {
        return new Sprite3D(scaled, image, appearance);
    }

    void copyFields(Object3D copy, Hashtable copies) {
        super.copyFields(copy, copies);
        ((Sprite3D) copy).setCrop(cropX, cropY, cropWidth, cropHeight);
    }
}
