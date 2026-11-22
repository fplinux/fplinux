// SPDX-License-Identifier: GPL-2.0-only
package com.nokia.mid.ui;

import javax.microedition.lcdui.Image;

public interface DirectGraphics {
    int FLIP_HORIZONTAL = 0x2000;
    int FLIP_VERTICAL = 0x4000;
    int ROTATE_90 = 90;
    int ROTATE_180 = 180;
    int ROTATE_270 = 270;
    int TYPE_USHORT_444_RGB = 444;
    int TYPE_USHORT_4444_ARGB = 4444;
    int TYPE_USHORT_555_RGB = 555;
    int TYPE_USHORT_1555_ARGB = 1555;
    int TYPE_USHORT_565_RGB = 565;
    int TYPE_INT_888_RGB = 888;
    int TYPE_INT_8888_ARGB = 8888;

    void drawImage(Image image, int x, int y, int anchor, int manipulation);

    /**
     * Sets the color and alpha of the underlying Graphics. Primitives and
     * text blend over the pixels inside the clip. Alpha 255 uses regular
     * drawing and alpha 0 leaves the target untouched. The Graphics color
     * setters restore alpha 255.
     */
    void setARGBColor(int argbColor);

    /** Returns the current color alpha of the underlying Graphics. */
    int getAlphaComponent();

    void drawTriangle(int x1, int y1, int x2, int y2, int x3, int y3, int argbColor);

    void fillTriangle(int x1, int y1, int x2, int y2, int x3, int y3, int argbColor);

    void drawPolygon(int[] xPoints, int xOffset, int[] yPoints, int yOffset,
                     int nPoints, int argbColor);

    void fillPolygon(int[] xPoints, int xOffset, int[] yPoints, int yOffset,
                     int nPoints, int argbColor);

    int getNativePixelFormat();

    void drawPixels(int[] pixels, boolean transparency, int offset, int scanlength,
                    int x, int y, int width, int height, int manipulation,
                    int format);

    void drawPixels(short[] pixels, boolean transparency, int offset, int scanlength,
                    int x, int y, int width, int height, int manipulation,
                    int format);

    void getPixels(int[] pixels, int offset, int scanlength, int x, int y,
                   int width, int height, int format);

    void getPixels(short[] pixels, int offset, int scanlength, int x, int y,
                   int width, int height, int format);
}
