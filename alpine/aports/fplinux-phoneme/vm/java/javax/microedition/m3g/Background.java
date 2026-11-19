// SPDX-License-Identifier: GPL-2.0-only
package javax.microedition.m3g;

import java.util.Hashtable;
import java.util.Vector;

public class Background extends Object3D {
    public static final int BORDER = 32;
    public static final int REPEAT = 33;

    int color;
    Image2D image;
    int x, y, width, height;
    int modeX = BORDER, modeY = BORDER;
    boolean depthClear = true, colorClear = true;

    public Image2D getImage() {
        return image;
    }

    public void setImage(Image2D value) {
        image = value;
        if (value != null && width == 0 && height == 0) {
            setCrop(0, 0, value.getWidth(), value.getHeight());
        }
    }

    public void setImageMode(int horizontal, int vertical) {
        if ((horizontal != BORDER && horizontal != REPEAT) ||
                (vertical != BORDER && vertical != REPEAT)) {
            throw new IllegalArgumentException("background image mode");
        }
        modeX = horizontal;
        modeY = vertical;
    }

    public void setColor(int argb) {
        color = argb;
    }

    public void setColorClearEnable(boolean enabled) {
        colorClear = enabled;
    }

    public void setDepthClearEnable(boolean enabled) {
        depthClear = enabled;
    }

    public void setCrop(int x, int y, int width, int height) {
        if (width < 0 || height < 0) {
            throw new IllegalArgumentException("background crop");
        }
        this.x = x;
        this.y = y;
        this.width = width;
        this.height = height;
    }

    void references(Vector objects) {
        super.references(objects);
        addReference(objects, image);
    }

    Object3D newCopy() {
        return new Background();
    }

    void copyFields(Object3D copy, Hashtable copies) {
        super.copyFields(copy, copies);
        Background target = (Background) copy;
        target.color = color;
        target.image = image;
        target.setImageMode(modeX, modeY);
        target.setCrop(x, y, width, height);
        target.depthClear = depthClear;
        target.colorClear = colorClear;
    }
}
