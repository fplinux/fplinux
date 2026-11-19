// SPDX-License-Identifier: GPL-2.0-only
package com.nokia.mid.ui;

import javax.microedition.lcdui.FPLinuxNokiaImage;
import javax.microedition.lcdui.Graphics;

final class NokiaShapes {
    private NokiaShapes() {
    }

    static void paint(Graphics graphics, int[] xPoints, int xOffset,
                      int[] yPoints, int yOffset, int count, int argb,
                      boolean outline, boolean fill) {
        if (xPoints == null || yPoints == null) {
            throw new NullPointerException();
        }
        if (count <= 0) {
            return;
        }
        if (xOffset < 0 || xOffset > xPoints.length - count ||
                yOffset < 0 || yOffset > yPoints.length - count) {
            throw new ArrayIndexOutOfBoundsException();
        }
        if (argb >>> 24 == 0 || (!outline && count < 3)) {
            return;
        }

        int minX = xPoints[xOffset];
        int minY = yPoints[yOffset];
        int maxX = minX;
        int maxY = minY;
        for (int point = 1; point < count; point++) {
            int x = xPoints[xOffset + point];
            int y = yPoints[yOffset + point];
            minX = Math.min(minX, x);
            minY = Math.min(minY, y);
            maxX = Math.max(maxX, x);
            maxY = Math.max(maxY, y);
        }

        synchronized (graphics) {
            int translateX = graphics.getTranslateX();
            int translateY = graphics.getTranslateY();
            int deviceClipX = graphics.getClipX() + translateX;
            int deviceClipY = graphics.getClipY() + translateY;
            long clipX = (long) deviceClipX - translateX;
            long clipY = (long) deviceClipY - translateY;
            long left = Math.max(clipX, minX);
            long top = Math.max(clipY, minY);
            long right = Math.min(clipX + graphics.getClipWidth(), (long) maxX + 1);
            long bottom = Math.min(clipY + graphics.getClipHeight(), (long) maxY + 1);
            if (right <= left || bottom <= top) {
                return;
            }
            int width = (int) (right - left);
            int height = (int) (bottom - top);
            int[] ink = new int[width * height];
            if (fill) {
                // Sample triangles on the same integer grid as their outline.
                fill(ink, width, height, left, top, argb, outline ? 0 : 0.5,
                     xPoints, xOffset, yPoints, yOffset, count);
            }
            if (outline) {
                int previous = count - 1;
                for (int point = 0; point < count; point++) {
                    drawLine(ink, width, height, left, top, argb,
                             xPoints[xOffset + previous], yPoints[yOffset + previous],
                             xPoints[xOffset + point], yPoints[yOffset + point]);
                    previous = point;
                }
            }
            draw(graphics, ink, (int) left, (int) top, width, height, argb);
        }
    }

    private static void fill(int[] ink, int width, int height, long left, long top,
                             int argb, double sampleOffset, int[] xPoints,
                             int xOffset, int[] yPoints, int yOffset, int count) {
        double[] crossings = new double[count];
        for (int row = 0; row < height; row++) {
            double sampleY = top + row + sampleOffset;
            int used = 0;
            int previous = count - 1;
            for (int point = 0; point < count; point++) {
                int low = previous;
                int high = point;
                if (yPoints[yOffset + low] > yPoints[yOffset + high]) {
                    low = point;
                    high = previous;
                }
                int lowY = yPoints[yOffset + low];
                int highY = yPoints[yOffset + high];
                if (sampleY >= lowY && sampleY < highY) {
                    int lowX = xPoints[xOffset + low];
                    int highX = xPoints[xOffset + high];
                    double crossing = lowX + (sampleY - lowY) *
                        (highX - (double) lowX) / (highY - (double) lowY);
                    int index = used++;
                    while (index > 0 && crossings[index - 1] > crossing) {
                        crossings[index] = crossings[index - 1];
                        index--;
                    }
                    crossings[index] = crossing;
                }
                previous = point;
            }
            // Pair crossings to fill each half-open span.
            for (int index = 0; index + 1 < used; index += 2) {
                int start = (int) Math.max(0, Math.ceil(crossings[index] - left - sampleOffset));
                int end = (int) Math.min(width, Math.ceil(crossings[index + 1] - left - sampleOffset));
                for (int col = start; col < end; col++) {
                    ink[row * width + col] = argb;
                }
            }
        }
    }

    private static void drawLine(int[] ink, int width, int height, long left,
                                 long top, int argb, long x1, long y1,
                                 long x2, long y2) {
        boolean steep = Math.abs(y2 - y1) > Math.abs(x2 - x1);
        long major1 = steep ? y1 : x1;
        long major2 = steep ? y2 : x2;
        long minor1 = steep ? x1 : y1;
        long minor2 = steep ? x2 : y2;
        long majorOrigin = steep ? top : left;
        long minorOrigin = steep ? left : top;
        int majorSize = steep ? height : width;
        int minorSize = steep ? width : height;
        if (major1 > major2) {
            long swap = major1;
            major1 = major2;
            major2 = swap;
            swap = minor1;
            minor1 = minor2;
            minor2 = swap;
        }
        long start = Math.max(majorOrigin, major1);
        long end = Math.min(majorOrigin + majorSize - 1, major2);
        double slope = major2 == major1 ? 0 : (double) (minor2 - minor1) / (major2 - major1);
        for (long major = start; major <= end; major++) {
            long minor = minor1 + (long) Math.floor((major - major1) * slope + 0.5) - minorOrigin;
            if (minor >= 0 && minor < minorSize) {
                int along = (int) (major - majorOrigin);
                int x = steep ? (int) minor : along;
                int y = steep ? along : (int) minor;
                ink[y * width + x] = argb;
            }
        }
    }

    private static void draw(Graphics graphics, int[] ink, int x, int y,
                             int width, int height, int argb) {
        int alpha = argb >>> 24;
        if (FPLinuxNokiaImage.hasAlpha(graphics)) {
            NokiaPixels.draw(graphics, ink, 0, width, x, y, width, height, 0, true);
        } else if (alpha == 255) {
            graphics.drawRGB(ink, 0, width, x, y, width, height, true);
        } else {
            // GXJ drawRGB reduces source alpha on RGB565 targets. Blend once
            // with the full alpha before writing the clipped pixels back.
            int[] dest = new int[ink.length];
            FPLinuxNokiaImage.read(graphics, dest, 0, width, x, y, width, height);
            for (int i = 0; i < ink.length; i++) {
                if (ink[i] != 0) {
                    int pixel = dest[i];
                    int red = blend(argb >>> 16, pixel >>> 16, alpha);
                    int green = blend(argb >>> 8, pixel >>> 8, alpha);
                    int blue = blend(argb, pixel, alpha);
                    dest[i] = red << 16 | green << 8 | blue;
                }
            }
            graphics.drawRGB(dest, 0, width, x, y, width, height, false);
        }
    }

    private static int blend(int source, int dest, int alpha) {
        return ((source & 255) * alpha + (dest & 255) * (255 - alpha) + 127) / 255;
    }
}
