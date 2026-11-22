// SPDX-License-Identifier: GPL-2.0-only
package javax.microedition.lcdui;

/** LCDUI bridge for Nokia images with an alpha plane. */
public final class FPLinuxNokiaImage {
    private FPLinuxNokiaImage() {
    }

    public static Image create(int width, int height, int argb) {
        return Image.createMutableARGBImage(width, height, argb);
    }

    public static Image decode(byte[] data, int offset, int length) {
        return Image.createMutableImage(data, offset, length);
    }

    public static boolean hasAlpha(Graphics graphics) {
        Image image = graphics.targetImage();
        return image != null && image.getImageData().hasAlpha();
    }

    public static void setARGBColor(Graphics graphics, int argb) {
        synchronized (graphics) {
            graphics.setColor(argb & 0xffffff);
            graphics.setAlpha(argb >>> 24);
        }
    }

    public static int alpha(Graphics graphics) {
        return graphics.getAlpha();
    }

    public static int width(Graphics graphics) {
        return graphics.targetWidth();
    }

    public static int height(Graphics graphics) {
        return graphics.targetHeight();
    }

    public static void read(Graphics graphics, int[] pixels, int offset, int stride,
                            int x, int y, int width, int height) {
        synchronized (graphics) {
            long translatedX = (long) x + graphics.getTranslateX();
            long translatedY = (long) y + graphics.getTranslateY();
            if (width < 0 || height < 0 || translatedX < 0 ||
                    translatedX + width > width(graphics) || translatedY < 0 ||
                    translatedY + height > height(graphics)) {
                throw new IllegalArgumentException("pixel region");
            }
            Image image = graphics.targetImage();
            if (image == null) {
                readScreen0(pixels, offset, stride, (int) translatedX,
                            (int) translatedY, width, height);
            } else if (Math.abs((long) stride) >= width) {
                image.getRGB(pixels, offset, stride, (int) translatedX,
                             (int) translatedY, width, height);
            } else {
                for (int row = 0; row < height; row++) {
                    image.getRGB(pixels, offset + row * stride, width,
                        (int) translatedX, (int) translatedY + row, width, 1);
                }
            }
        }
    }

    /** Replace a region after a renderer has already composited its pixels. */
    public static void writeComposited(Graphics graphics, int[] pixels,
                                       int offset, int stride, int x, int y,
                                       int width, int height) {
        synchronized (graphics) {
            Image image = graphics.targetImage();
            if (image == null || !image.getImageData().hasAlpha()) {
                graphics.drawRGB(pixels, offset, stride,
                    x - graphics.getTranslateX(), y - graphics.getTranslateY(),
                    width, height, false);
                return;
            }
            image.getImageData().replaceARGB(pixels, offset, stride, x, y,
                                             width, height);
        }
    }

    private static native void readScreen0(int[] pixels, int offset, int stride,
                                           int x, int y, int width, int height);
}
