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
    private final int width, height;
    private final boolean mutable;
    // Set for an image made from a platform Image; its texels are read on first use.
    private final Image image;
    // ARGB texels as the renderer reads them: ALPHA keeps only the alpha channel,
    // luminance formats repeat the level in the color channels.
    private int[] pixels;

    public Image2D(int format, Object provider) {
        if (format < ALPHA || format > RGBA || !(provider instanceof Image)) {
            throw new IllegalArgumentException("image format or source");
        }
        this.format = format;
        image = (Image) provider;
        width = image.getWidth();
        height = image.getHeight();
        mutable = false;
    }

    public Image2D(int format, int width, int height, byte[] image) {
        this(format, width, height, texels(format, width, height, image, null), false);
    }

    public Image2D(int format, int width, int height, byte[] image, byte[] palette) {
        this(format, width, height, texels(format, width, height, image, palette), false);
    }

    public Image2D(int format, int width, int height) {
        this(format, width, height, white(format, width, height), true);
    }

    private Image2D(int format, int width, int height, int[] pixels, boolean mutable) {
        this.format = format;
        this.width = width;
        this.height = height;
        this.pixels = pixels;
        this.mutable = mutable;
        image = null;
    }

    public int getFormat() {
        return format;
    }

    public int getWidth() {
        return width;
    }

    public int getHeight() {
        return height;
    }

    public boolean isMutable() {
        return mutable;
    }

    public void set(int x, int y, int w, int h, byte[] values) {
        if (values == null) throw new NullPointerException();
        if (!mutable) throw new IllegalStateException("immutable image");
        int stride = bytesPerPixel(format);
        if (x < 0 || y < 0 || w < 0 || h < 0 || (long) x + w > width || (long) y + h > height ||
                (long) w * h * stride > values.length) {
            throw new IllegalArgumentException("image region");
        }
        for (int row = 0; row < h; row++) {
            for (int column = 0; column < w; column++) {
                pixels[(y + row) * width + x + column] =
                    texel(format, values, (row * w + column) * stride);
            }
        }
    }

    int[] pixels() {
        if (pixels == null) {
            pixels = new int[width * height];
            image.getRGB(pixels, 0, width, 0, 0, width, height);
            if (format == RGB) {
                for (int i = 0; i < pixels.length; i++) {
                    pixels[i] |= 0xff000000;
                }
            }
        }
        return pixels;
    }

    private static int bytesPerPixel(int format) {
        switch (format) {
            case ALPHA:
            case LUMINANCE:
                return 1;
            case LUMINANCE_ALPHA:
                return 2;
            case RGB:
                return 3;
            case RGBA:
                return 4;
            default:
                throw new IllegalArgumentException("image format");
        }
    }

    private static int checkedCount(int format, int width, int height) {
        bytesPerPixel(format);
        if (width <= 0 || height <= 0 || (long) width * height > Integer.MAX_VALUE) {
            throw new IllegalArgumentException("image size");
        }
        return width * height;
    }

    private static int[] white(int format, int width, int height) {
        int[] white = new int[checkedCount(format, width, height)];
        for (int i = 0; i < white.length; i++) white[i] = 0xffffffff;
        return white;
    }

    private static int[] texels(int format, int width, int height, byte[] image, byte[] palette) {
        if (image == null) throw new NullPointerException();
        int stride = bytesPerPixel(format);
        checkedCount(format, width, height);
        long count = (long) width * height;
        if (palette == null) {
            if (count * stride > image.length) throw new IllegalArgumentException("image data");
        } else {
            if (count > image.length || palette.length == 0 || palette.length % stride != 0 ||
                    palette.length > 256 * stride) {
                throw new IllegalArgumentException("image data or palette");
            }
        }
        int[] argb = new int[(int) count];
        for (int i = 0; i < argb.length; i++) {
            if (palette == null) {
                argb[i] = texel(format, image, i * stride);
            } else {
                int index = image[i] & 255;
                if (index * stride >= palette.length) {
                    throw new IllegalArgumentException("palette index");
                }
                argb[i] = texel(format, palette, index * stride);
            }
        }
        return argb;
    }

    private static int texel(int format, byte[] data, int offset) {
        int first = data[offset] & 255;
        switch (format) {
            case ALPHA:
                return first << 24 | 0xffffff;
            case LUMINANCE:
                return 0xff000000 | first * 0x010101;
            case LUMINANCE_ALPHA:
                return (data[offset + 1] & 255) << 24 | first * 0x010101;
            case RGB:
                return 0xff000000 | first << 16 | (data[offset + 1] & 255) << 8 |
                    data[offset + 2] & 255;
            default:
                return (data[offset + 3] & 255) << 24 | first << 16 |
                    (data[offset + 1] & 255) << 8 | data[offset + 2] & 255;
        }
    }

    Object3D newCopy() {
        if (image != null) return new Image2D(format, image);
        int[] copy = pixels;
        if (mutable) {
            copy = new int[pixels.length];
            System.arraycopy(pixels, 0, copy, 0, pixels.length);
        }
        return new Image2D(format, width, height, copy, mutable);
    }
}
