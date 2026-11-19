// SPDX-License-Identifier: GPL-2.0-only
package com.nokia.mid.ui;

import javax.microedition.lcdui.FPLinuxNokiaImage;
import javax.microedition.lcdui.Graphics;
import javax.microedition.lcdui.Image;

final class NokiaPixels {
    private NokiaPixels() {
    }

    static int checkRegion(int length, int offset, int stride, int width,
                           int height) {
        if (width < 0 || height < 0) {
            throw new IllegalArgumentException("pixel dimensions");
        }
        if (width == 0 || height == 0) {
            return 0;
        }
        long last = offset + (long) (height - 1) * stride;
        long count = (long) width * height;
        if (Math.min((long) offset, last) < 0 ||
                Math.max((long) offset, last) + width > length ||
                count > Integer.MAX_VALUE) {
            throw new ArrayIndexOutOfBoundsException();
        }
        return (int) count;
    }

    static void checkIntFormat(int format) {
        if (format != DirectGraphics.TYPE_INT_888_RGB &&
                format != DirectGraphics.TYPE_INT_8888_ARGB) {
            throw new IllegalArgumentException("integer pixel format");
        }
    }

    static void checkShortFormat(int format) {
        switch (format) {
            case DirectGraphics.TYPE_USHORT_444_RGB:
            case DirectGraphics.TYPE_USHORT_4444_ARGB:
            case DirectGraphics.TYPE_USHORT_555_RGB:
            case DirectGraphics.TYPE_USHORT_1555_ARGB:
            case DirectGraphics.TYPE_USHORT_565_RGB:
                return;
            default:
                throw new IllegalArgumentException("short pixel format");
        }
    }

    static void draw(Graphics graphics, int[] values, int offset, int stride,
                     int x, int y, int width, int height, int transform,
                     boolean alpha) {
        boolean direct = transform == 0 && !FPLinuxNokiaImage.hasAlpha(graphics);
        if (direct && stride >= width) {
            graphics.drawRGB(values, offset, stride, x, y, width, height, alpha);
            return;
        }
        int[] pixels = values;
        if (offset != 0 || stride != width) {
            pixels = new int[width * height];
            for (int row = 0; row < height; row++) {
                System.arraycopy(values, offset + row * stride,
                                 pixels, row * width, width);
            }
        }
        if (direct) {
            graphics.drawRGB(pixels, 0, width, x, y, width, height, alpha);
            return;
        }
        Image image = Image.createRGBImage(pixels, width, height, alpha);
        graphics.drawRegion(image, 0, 0, width, height, transform, x, y,
                            Graphics.TOP | Graphics.LEFT);
    }

    private static int expandFour(int channel) {
        return (channel & 15) * 17;
    }

    private static int expandFive(int channel) {
        int value = channel & 31;
        return value << 3 | value >>> 2;
    }

    private static int expandSix(int channel) {
        int value = channel & 63;
        return value << 2 | value >>> 4;
    }

    static int fromShort(int value, int format) {
        int alpha = 255;
        int red, green, blue;
        switch (format) {
            case DirectGraphics.TYPE_USHORT_4444_ARGB:
            case DirectGraphics.TYPE_USHORT_444_RGB:
                if (format == DirectGraphics.TYPE_USHORT_4444_ARGB) {
                    alpha = expandFour(value >>> 12);
                }
                red = expandFour(value >>> 8);
                green = expandFour(value >>> 4);
                blue = expandFour(value);
                break;
            case DirectGraphics.TYPE_USHORT_565_RGB:
                red = expandFive(value >>> 11);
                green = expandSix(value >>> 5);
                blue = expandFive(value);
                break;
            default:
                if (format == DirectGraphics.TYPE_USHORT_1555_ARGB) {
                    alpha = (value & 0x8000) == 0 ? 0 : 255;
                }
                red = expandFive(value >>> 10);
                green = expandFive(value >>> 5);
                blue = expandFive(value);
                break;
        }
        return alpha << 24 | red << 16 | green << 8 | blue;
    }

    static short toShort(int argb, int format) {
        int color;
        switch (format) {
            case DirectGraphics.TYPE_USHORT_4444_ARGB:
            case DirectGraphics.TYPE_USHORT_444_RGB:
                color = (argb >>> 12) & 0x0f00;
                color |= (argb >>> 8) & 0x00f0;
                color |= (argb >>> 4) & 0x000f;
                if (format == DirectGraphics.TYPE_USHORT_4444_ARGB) {
                    color |= (argb >>> 16) & 0xf000;
                }
                break;
            case DirectGraphics.TYPE_USHORT_565_RGB:
                color = (argb >>> 8) & 0xf800;
                color |= (argb >>> 5) & 0x07e0;
                color |= (argb >>> 3) & 0x001f;
                break;
            default:
                color = (argb >>> 9) & 0x7c00;
                color |= (argb >>> 6) & 0x03e0;
                color |= (argb >>> 3) & 0x001f;
                if (format == DirectGraphics.TYPE_USHORT_1555_ARGB) {
                    color |= (argb >>> 16) & 0x8000;
                }
                break;
        }
        return (short) color;
    }
}
