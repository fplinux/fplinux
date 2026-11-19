// SPDX-License-Identifier: GPL-2.0-only
package javax.microedition.m3g;

import javax.microedition.lcdui.Image;

public class Image2D extends Object3D {
    public static final int ALPHA = 96;
    public static final int LUMINANCE = 97;
    public static final int LUMINANCE_ALPHA = 98;
    public static final int RGB = 99;
    public static final int RGBA = 100;

    final int format;
    final Image image;
    private int[] pixels;

    public Image2D(int format, Object provider) {
        if (format < ALPHA || format > RGBA || !(provider instanceof Image)) {
            throw new IllegalArgumentException("image format or source");
        }
        this.format = format;
        image = (Image) provider;
    }

    public int getWidth() {
        return image.getWidth();
    }

    public int getHeight() {
        return image.getHeight();
    }

    int[] pixels() {
        if (pixels == null) {
            int width = getWidth();
            pixels = new int[width * getHeight()];
            image.getRGB(pixels, 0, width, 0, 0, width, getHeight());
            if (format == RGB) {
                for (int i = 0; i < pixels.length; i++) {
                    pixels[i] |= 0xff000000;
                }
            }
        }
        return pixels;
    }

    Object3D newCopy() {
        return new Image2D(format, image);
    }
}
