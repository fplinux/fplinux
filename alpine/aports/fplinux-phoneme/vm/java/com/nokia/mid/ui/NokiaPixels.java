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

    /**
     * Converts a checked region of 16-bit pixels to ARGB rows. A channel of
     * n bits is widened to 8 by repeating its top bits below it; each pixel
     * is converted in one expression, so a row costs no calls.
     */
    static int[] fromShorts(short[] pixels, int offset, int stride, int width,
                            int height, int format) {
        int[] argb = new int[width * height];
        int target = 0;
        for (int row = 0; row < height; row++) {
            int source = offset + row * stride;
            int end = target + width;
            switch (format) {
                case DirectGraphics.TYPE_USHORT_4444_ARGB:
                    // Nibbles spread one per byte, then each n becomes n * 17.
                    while (target < end) {
                        int value = pixels[source++];
                        argb[target++] = ((value & 0xf000) << 12 |
                            (value & 0xf00) << 8 | (value & 0xf0) << 4 |
                            value & 0xf) * 0x11;
                    }
                    break;
                case DirectGraphics.TYPE_USHORT_444_RGB:
                    while (target < end) {
                        int value = pixels[source++];
                        argb[target++] = 0xff000000 | ((value & 0xf00) << 8 |
                            (value & 0xf0) << 4 | value & 0xf) * 0x11;
                    }
                    break;
                case DirectGraphics.TYPE_USHORT_565_RGB:
                    while (target < end) {
                        int value = pixels[source++];
                        int red = value >> 8 & 0xf8;
                        int green = value >> 3 & 0xfc;
                        int blue = value << 3 & 0xf8;
                        argb[target++] = 0xff000000 | (red | red >> 5) << 16 |
                            (green | green >> 6) << 8 | blue | blue >> 5;
                    }
                    break;
                default:
                    boolean masked = format == DirectGraphics.TYPE_USHORT_1555_ARGB;
                    while (target < end) {
                        int value = pixels[source++];
                        int alpha = masked && (value & 0x8000) == 0 ? 0 : 0xff000000;
                        int red = value >> 7 & 0xf8;
                        int green = value >> 2 & 0xf8;
                        int blue = value << 3 & 0xf8;
                        argb[target++] = alpha | (red | red >> 5) << 16 |
                            (green | green >> 5) << 8 | blue | blue >> 5;
                    }
                    break;
            }
        }
        return argb;
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
