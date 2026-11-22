// SPDX-License-Identifier: GPL-2.0-only
package com.nokia.mid.ui;

import javax.microedition.lcdui.FPLinuxNokiaImage;
import javax.microedition.lcdui.Graphics;
import javax.microedition.lcdui.Image;
import javax.microedition.lcdui.game.Sprite;

public class DirectUtils {
    private DirectUtils() {
    }

    public static Image createImage(int width, int height, int argb) {
        return FPLinuxNokiaImage.create(width, height, argb);
    }

    public static Image createImage(byte[] data, int offset, int length) {
        return FPLinuxNokiaImage.decode(data, offset, length);
    }

    public static DirectGraphics getDirectGraphics(Graphics graphics) {
        if (graphics == null) {
            throw new NullPointerException();
        }
        return new ImageGraphics(graphics);
    }

    private static final class ImageGraphics implements DirectGraphics {
        // One row per counterclockwise quarter turn. Columns select the flips
        // applied after rotation: none, horizontal, vertical, both.
        private static final int[][] TRANSFORMS = {
            { Sprite.TRANS_NONE, Sprite.TRANS_MIRROR,
              Sprite.TRANS_MIRROR_ROT180, Sprite.TRANS_ROT180 },
            { Sprite.TRANS_ROT270, Sprite.TRANS_MIRROR_ROT90,
              Sprite.TRANS_MIRROR_ROT270, Sprite.TRANS_ROT90 },
            { Sprite.TRANS_ROT180, Sprite.TRANS_MIRROR_ROT180,
              Sprite.TRANS_MIRROR, Sprite.TRANS_NONE },
            { Sprite.TRANS_ROT90, Sprite.TRANS_MIRROR_ROT270,
              Sprite.TRANS_MIRROR_ROT90, Sprite.TRANS_ROT270 },
        };
        private final Graphics graphics;

        ImageGraphics(Graphics graphics) {
            this.graphics = graphics;
        }

        private static int transformOf(int manipulation) {
            int rotation = manipulation & ~(FLIP_HORIZONTAL | FLIP_VERTICAL);
            if (rotation < 0 || rotation > ROTATE_270 || rotation % 90 != 0) {
                throw new IllegalArgumentException("image manipulation");
            }
            int flips = (manipulation & (FLIP_HORIZONTAL | FLIP_VERTICAL)) >>> 13;
            return TRANSFORMS[rotation / 90][flips];
        }

        public void drawImage(Image image, int x, int y, int anchor,
                              int manipulation) {
            int transform = transformOf(manipulation);
            if (transform == Sprite.TRANS_NONE) {
                graphics.drawImage(image, x, y, anchor);
            } else {
                graphics.drawRegion(image, 0, 0, image.getWidth(),
                                    image.getHeight(), transform, x, y, anchor);
            }
        }

        public int getNativePixelFormat() {
            return TYPE_USHORT_565_RGB;
        }

        public void setARGBColor(int argbColor) {
            FPLinuxNokiaImage.setARGBColor(graphics, argbColor);
        }

        public int getAlphaComponent() {
            return FPLinuxNokiaImage.alpha(graphics);
        }

        public void drawTriangle(int x1, int y1, int x2, int y2, int x3, int y3,
                                 int argbColor) {
            NokiaShapes.paint(graphics, new int[] { x1, x2, x3 }, 0,
                              new int[] { y1, y2, y3 }, 0, 3, argbColor, true, false);
        }

        public void fillTriangle(int x1, int y1, int x2, int y2, int x3, int y3,
                                 int argbColor) {
            NokiaShapes.paint(graphics, new int[] { x1, x2, x3 }, 0,
                              new int[] { y1, y2, y3 }, 0, 3, argbColor, true, true);
        }

        public void drawPolygon(int[] xPoints, int xOffset, int[] yPoints,
                                int yOffset, int nPoints, int argbColor) {
            NokiaShapes.paint(graphics, xPoints, xOffset, yPoints, yOffset,
                              nPoints, argbColor, true, false);
        }

        public void fillPolygon(int[] xPoints, int xOffset, int[] yPoints,
                                int yOffset, int nPoints, int argbColor) {
            NokiaShapes.paint(graphics, xPoints, xOffset, yPoints, yOffset,
                              nPoints, argbColor, false, true);
        }

        public void drawPixels(int[] pixels, boolean transparency, int offset,
                               int stride, int x, int y, int width, int height,
                               int manipulation, int format) {
            NokiaPixels.checkIntFormat(format);
            int transform = transformOf(manipulation);
            if (NokiaPixels.checkRegion(pixels.length, offset, stride,
                                       width, height) != 0) {
                NokiaPixels.draw(graphics, pixels, offset, stride, x, y, width,
                    height, transform, transparency && format == TYPE_INT_8888_ARGB);
            }
        }

        public void drawPixels(short[] pixels, boolean transparency, int offset,
                               int stride, int x, int y, int width, int height,
                               int manipulation, int format) {
            NokiaPixels.checkShortFormat(format);
            int transform = transformOf(manipulation);
            int count = NokiaPixels.checkRegion(pixels.length, offset, stride,
                                                width, height);
            if (count == 0) {
                return;
            }
            int[] argb = NokiaPixels.fromShorts(pixels, offset, stride, width,
                                                height, format);
            NokiaPixels.draw(graphics, argb, 0, width, x, y, width, height,
                             transform, transparency);
        }

        public void getPixels(int[] pixels, int offset, int stride, int x, int y,
                              int width, int height, int format) {
            NokiaPixels.checkIntFormat(format);
            if (NokiaPixels.checkRegion(pixels.length, offset, stride, width,
                                        height) == 0) {
                return;
            }
            FPLinuxNokiaImage.read(graphics, pixels, offset, stride, x, y,
                                  width, height);
            if (format == TYPE_INT_888_RGB) {
                for (int row = 0; row < height; row++) {
                    int start = offset + row * stride;
                    for (int col = 0; col < width; col++) {
                        pixels[start + col] &= 0xffffff;
                    }
                }
            }
        }

        public void getPixels(short[] pixels, int offset, int stride, int x,
                              int y, int width, int height, int format) {
            NokiaPixels.checkShortFormat(format);
            int count = NokiaPixels.checkRegion(pixels.length, offset, stride,
                                                width, height);
            if (count == 0) {
                return;
            }
            int[] argb = new int[count];
            FPLinuxNokiaImage.read(graphics, argb, 0, width, x, y, width, height);
            for (int row = 0; row < height; row++) {
                for (int col = 0; col < width; col++) {
                    pixels[offset + row * stride + col] =
                        NokiaPixels.toShort(argb[row * width + col], format);
                }
            }
        }
    }
}
