// SPDX-License-Identifier: GPL-2.0-only
package fplinux.fixtures.canvas;

import com.mascotcapsule.micro3d.v3.Effect3D;
import com.mascotcapsule.micro3d.v3.FigureLayout;
import com.mascotcapsule.micro3d.v3.Graphics3D;
import com.mascotcapsule.micro3d.v3.Light;
import com.mascotcapsule.micro3d.v3.Texture;
import com.nokia.mid.ui.DirectGraphics;
import com.nokia.mid.ui.DirectUtils;
import com.nokia.mid.ui.FullCanvas;
import com.sun.midp.io.Base64;
import java.io.ByteArrayInputStream;
import java.io.IOException;
import javax.microedition.lcdui.Canvas;
import javax.microedition.lcdui.Command;
import javax.microedition.lcdui.Display;
import javax.microedition.lcdui.Font;
import javax.microedition.lcdui.Graphics;
import javax.microedition.lcdui.FPLinuxNokiaImage;
import javax.microedition.lcdui.Image;
import javax.microedition.lcdui.game.Sprite;
import javax.microedition.midlet.MIDlet;

/**
 * Canvas/GameCanvas probe. All steps run on a thread of their own so the
 * event thread is never blocked; every output line starts with "PM-CANVAS ".
 */
public class CanvasProbeMIDlet extends MIDlet implements Runnable {
    private static final int POLL_MS = 20;
    private static final int TIMEOUT_MS = 5000;
    private static final String[] ACTION_NAMES = {
        "UP", "DOWN", "LEFT", "RIGHT", "FIRE",
        "GAME_A", "GAME_B", "GAME_C", "GAME_D"
    };
    private static final int[] ACTIONS = {
        Canvas.UP, Canvas.DOWN, Canvas.LEFT, Canvas.RIGHT, Canvas.FIRE,
        Canvas.GAME_A, Canvas.GAME_B, Canvas.GAME_C, Canvas.GAME_D
    };
    // Cyrillic word "Klavishi" as escapes
    private static final String SAMPLE =
        "Keys \u041a\u043b\u0430\u0432\u0438\u0448\u0438";
    private static final String RED_PNG =
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVQI12P4z8DwHwAFAAH/" +
        "cpxSZwAAAABJRU5ErkJggg==";
    private static final String ALPHA_PNG =
        "iVBORw0KGgoAAAANSUhEUgAAAAMAAAABCAYAAAAb4BS0AAAAFUlEQVQI12NgYGBg+M/A" +
        "0MDwn+E/AA6BA36DaD1gAAAAAElFTkSuQmCC";

    private Display display;
    private boolean started;

    protected void startApp() {
        if (started) {
            return;
        }
        started = true;
        display = Display.getDisplay(this);
        new Thread(this).start();
    }

    protected void pauseApp() {
    }

    protected void destroyApp(boolean unconditional) {
    }

    private static void say(String line) {
        System.out.println("PM-CANVAS " + line);
    }

    public void run() {
        try {
            probe();
        } catch (Throwable t) {
            say("exception " + t.getClass().getName());
        }
        say("done");
        notifyDestroyed();
    }

    private void probe() throws InterruptedException, IOException {
        say("start");
        ProbeCanvas canvas = new ProbeCanvas();
        canvas.setTitle("Probe");
        display.setCurrent(canvas);
        waitFor(canvas, "show", 0);
        waitFor(canvas, "paint", 0);
        say("shown size=" + canvas.getWidth() + "x" + canvas.getHeight());

        keys(canvas);
        say("hasRepeatEvents " + canvas.hasRepeatEvents());
        say("hasPointerEvents " + canvas.hasPointerEvents());
        say("isDoubleBuffered " + canvas.isDoubleBuffered());

        fullScreen(canvas, true, "fullscreen");
        fullScreen(canvas, false, "normal");

        gameSmoke(canvas);
        showFull();
        nokiaCanvas();
        nokiaImages();
        regionTransforms();
        nokiaPixels();
        nokiaDecodedImage();
        nokiaShapes();
        nokiaTextValidation();
        vodafoneTypes();
        mascotPixels();
        mascotTexturedPixels();
        say(M3gProbe.check());
        imageStream();
        font();
    }

    private static class NokiaCanvas extends FullCanvas {
        volatile int clipHeight;
        boolean screenPixel;

        protected void paint(Graphics graphics) {
            graphics.setColor(0x00ff00);
            graphics.fillRect(0, 0, 1, 1);
            int[] rgb = new int[1];
            DirectUtils.getDirectGraphics(graphics).getPixels(rgb, 0, 1, 0, 0,
                1, 1, DirectGraphics.TYPE_INT_8888_ARGB);
            screenPixel = rgb[0] == 0xff00ff00 &&
                FPLinuxNokiaImage.width(graphics) == getWidth() &&
                FPLinuxNokiaImage.height(graphics) == getHeight();
            clipHeight = graphics.getClipHeight();
        }
    }

    private void nokiaCanvas() throws InterruptedException {
        NokiaCanvas canvas = new NokiaCanvas();
        boolean commandRejected = false;
        boolean listenerRejected = false;
        try {
            canvas.addCommand(new Command("Exit", Command.EXIT, 0));
        } catch (IllegalStateException expected) {
            commandRejected = true;
        }
        try {
            canvas.setCommandListener(null);
        } catch (IllegalStateException expected) {
            listenerRejected = true;
        }

        display.setCurrent(canvas);
        for (int waited = 0; waited < TIMEOUT_MS && canvas.clipHeight == 0;
             waited += POLL_MS) {
            Thread.sleep(POLL_MS);
        }
        say("nokia size=" + canvas.getWidth() + "x" + canvas.getHeight()
            + " clip=" + canvas.clipHeight + " commandRejected="
            + commandRejected + " listenerRejected=" + listenerRejected
            + " screenPixel=" + canvas.screenPixel);
    }

    private void nokiaImages() {
        int anchor = Graphics.TOP | Graphics.LEFT;
        Image sheet = Image.createRGBImage(new int[] {
            0xffff0000, 0xff00ff00,
            0xff0000ff, 0,
            0xffffffff, 0xffffff00,
        }, 2, 3, true);
        Image tile = DirectUtils.createImage(4, 4, 0);
        Graphics graphics = tile.getGraphics();
        graphics.setClip(1, 1, 2, 2);
        graphics.translate(1, 1);
        DirectUtils.getDirectGraphics(graphics).drawImage(sheet, 3, 2,
            Graphics.RIGHT | Graphics.BOTTOM,
            DirectGraphics.ROTATE_90 | DirectGraphics.FLIP_HORIZONTAL);

        int[] pixels = new int[16];
        int[] expected = {
            0, 0, 0, 0,
            0, 0xff808000, 0, 0,
            0, 0xff808080, 0xff000080, 0,
            0, 0, 0, 0,
        };
        tile.getRGB(pixels, 0, 4, 0, 0, 4, 4);
        boolean transform = true;
        for (int i = 0; i < pixels.length; i++) {
            if ((pixels[i] & 0xff808080) != expected[i]) {
                transform = false;
                break;
            }
        }

        Image backdrop = Image.createImage(4, 4);
        backdrop.getRGB(pixels, 0, 4, 0, 0, 4, 4);
        boolean white = true;
        for (int i = 0; i < pixels.length; i++) {
            if ((pixels[i] & 0xfff8fcf8) != 0xfff8fcf8) {
                white = false;
                break;
            }
        }
        say("image opaque pixels are white=" + white);
        Graphics backdropGraphics = backdrop.getGraphics();
        backdropGraphics.setColor(0xff00ff);
        backdropGraphics.fillRect(0, 0, 4, 4);
        backdropGraphics.drawImage(tile, 0, 0, anchor);
        backdrop.getRGB(pixels, 0, 4, 0, 0, 4, 4);
        boolean blit = (pixels[5] & 0xff808080) == 0xff808000 &&
                       (pixels[6] & 0xff808080) == 0xff800080 &&
                       (pixels[7] & 0xff808080) == 0xff800080;

        Image background = DirectUtils.createImage(1, 1, 0x400000ff);
        Image ink = DirectUtils.createImage(1, 1, 0x80ff0000);
        Graphics pixel = background.getGraphics();
        pixel.drawImage(ink, 0, 0, anchor);
        background.getRGB(pixels, 0, 1, 0, 0, 1, 1);
        boolean partial = (pixels[0] & 0xff808080) == 0xa0800000;

        Image opaque = Image.createRGBImage(new int[] { 0xff00ff00 }, 1, 1, false);
        pixel.drawImage(opaque, 0, 0, anchor);
        background.getRGB(pixels, 0, 1, 0, 0, 1, 1);
        boolean replaced = (pixels[0] & 0xff808080) == 0xff008000;
        say("nokia image transform=" + transform + " blit=" + blit
            + " partial=" + partial + " replaced=" + replaced);
    }

    // Pixels of the source A B C / D E F after each Sprite transform, row by
    // row, and the width of that layout.
    private static final int REGION_A = 0xffff0000;
    private static final int REGION_B = 0xff00ff00;
    private static final int REGION_C = 0xff0000ff;
    private static final int REGION_D = 0xffffff00;
    private static final int REGION_E = 0xff00ffff;
    private static final int REGION_F = 0xffff00ff;
    private static final int REGION_BACKDROP = 0xff000000;

    private static int[] regionLayout(int transform) {
        int a = REGION_A, b = REGION_B, c = REGION_C;
        int d = REGION_D, e = REGION_E, f = REGION_F;
        switch (transform) {
        case Sprite.TRANS_NONE: return new int[] { a, b, c, d, e, f };
        case Sprite.TRANS_MIRROR_ROT180: return new int[] { d, e, f, a, b, c };
        case Sprite.TRANS_MIRROR: return new int[] { c, b, a, f, e, d };
        case Sprite.TRANS_ROT180: return new int[] { f, e, d, c, b, a };
        case Sprite.TRANS_MIRROR_ROT270: return new int[] { a, d, b, e, c, f };
        case Sprite.TRANS_ROT90: return new int[] { d, a, e, b, f, c };
        case Sprite.TRANS_ROT270: return new int[] { c, f, b, e, a, d };
        default: return new int[] { f, c, e, b, d, a }; // TRANS_MIRROR_ROT90
        }
    }

    private static int[] drawRegion(Image source, int sourceX, int transform, int clipY) {
        Image target = Image.createImage(5, 5);
        Graphics graphics = target.getGraphics();
        graphics.setColor(REGION_BACKDROP);
        graphics.fillRect(0, 0, 5, 5);
        graphics.setClip(0, clipY, 5, 5 - clipY);
        graphics.drawRegion(source, sourceX, 0, 3, 2, transform, 1, 1,
            Graphics.TOP | Graphics.LEFT);
        int[] pixels = new int[25];
        target.getRGB(pixels, 0, 5, 0, 0, 5, 5);
        return pixels;
    }

    // Compares the RGB565 bits of the 5x5 target with the layout drawn at
    // (1, 1); the clipped rows and the transparent pixel keep the backdrop.
    private static boolean regionMatches(int[] pixels, int transform, int transparent,
                                         int clipY) {
        int[] layout = regionLayout(transform);
        int width = (transform & 4) != 0 ? 2 : 3;
        int height = 6 / width;
        for (int y = 0; y < 5; y++) {
            for (int x = 0; x < 5; x++) {
                int expected = REGION_BACKDROP;
                if (y >= clipY && x >= 1 && x <= width && y >= 1 && y <= height) {
                    int value = layout[(y - 1) * width + x - 1];
                    expected = value == transparent ? REGION_BACKDROP : value;
                }
                if (((pixels[y * 5 + x] ^ expected) & 0xf8fcf8) != 0) {
                    return false;
                }
            }
        }
        return true;
    }

    private void regionTransforms() {
        Image opaque = Image.createRGBImage(new int[] {
            REGION_A, REGION_B, REGION_C, REGION_D, REGION_E, REGION_F,
        }, 3, 2, false);
        Image keyed = Image.createRGBImage(new int[] {
            REGION_A, REGION_B, REGION_C, REGION_D, 0, REGION_F,
        }, 3, 2, true);
        Image wide = Image.createRGBImage(new int[] {
            0, REGION_A, REGION_B, REGION_C, 0, REGION_D, REGION_E, REGION_F,
        }, 4, 2, false);
        boolean transforms = true;
        boolean keyedTransforms = true;
        for (int transform = 0; transform < 8; transform++) {
            transforms &= regionMatches(drawRegion(opaque, 0, transform, 0),
                transform, -1, 0);
            keyedTransforms &= regionMatches(drawRegion(keyed, 0, transform, 0),
                transform, REGION_E, 0);
        }
        boolean clipped = regionMatches(drawRegion(wide, 1, Sprite.TRANS_ROT90, 2),
            Sprite.TRANS_ROT90, -1, 2);
        say("region transforms=" + transforms + " keyed=" + keyedTransforms
            + " clipped=" + clipped);
    }

    private void nokiaPixels() {
        Image image = DirectUtils.createImage(3, 2, 0);
        Graphics graphics = image.getGraphics();
        DirectGraphics direct = DirectUtils.getDirectGraphics(graphics);
        graphics.translate(1, 0);
        short[] source = { 0, (short) 0xff00, 0, 0, (short) 0xf0f0, (short) 0xf00f };
        direct.drawPixels(source, true, 4, -3, 0, 0, 2, 2, 0,
                          DirectGraphics.TYPE_USHORT_4444_ARGB);
        int[] raw = new int[6];
        image.getRGB(raw, 0, 3, 0, 0, 3, 2);
        boolean literal = (raw[1] & 0xff808080) == 0xff008000 &&
            (raw[2] & 0xff808080) == 0xff000080 &&
            (raw[4] & 0xff808080) == 0xff800000 && raw[5] == 0;
        graphics.setClip(0, 0, 1, 1);
        short[] result = { 7, 7, 7, 7, 7, 7 };
        direct.getPixels(result, 4, -3, 0, 0, 2, 2,
                         DirectGraphics.TYPE_USHORT_4444_ARGB);
        boolean shortPixels = result[0] == 7 && result[3] == 7 &&
            result[1] == (short) 0xff00 && result[2] == 0 &&
            result[4] == (short) 0xf0f0 && result[5] == (short) 0xf00f;

        Image opaque = Image.createImage(2, 1);
        DirectGraphics target = DirectUtils.getDirectGraphics(opaque.getGraphics());
        target.drawPixels(new int[] { 0xffff0000, 0x000000ff }, true, 0, 2,
                          0, 0, 2, 1, DirectGraphics.FLIP_HORIZONTAL,
                          DirectGraphics.TYPE_INT_8888_ARGB);
        int[] rgb = new int[2];
        target.getPixels(rgb, 0, 2, 0, 0, 2, 1, DirectGraphics.TYPE_INT_888_RGB);
        boolean intPixels = (rgb[0] & 0xfff8fcf8) == 0x00f8fcf8 &&
            (rgb[1] & 0xfff8fcf8) == 0x00f80000;
        Image stride = Image.createImage(2, 2);
        DirectGraphics rows = DirectUtils.getDirectGraphics(stride.getGraphics());
        rows.drawPixels(new int[] { 0xffff0000, 0xff0000ff, 0,
                                    0xff00ff00, 0xffffffff },
            true, 3, -3, 0, 0, 2, 2, 0, DirectGraphics.TYPE_INT_8888_ARGB);
        stride.getRGB(raw, 0, 2, 0, 0, 2, 2);
        boolean backwards = (raw[0] & 0xff808080) == 0xff008000 &&
            (raw[1] & 0xff808080) == 0xff808080 &&
            (raw[2] & 0xff808080) == 0xff800000 &&
            (raw[3] & 0xff808080) == 0xff000080;
        say("nokia pixels short=" + shortPixels + " literal=" + literal +
            " int=" + intPixels + " backwards=" + backwards);
    }

    private void mascotPixels() {
        Image image = DirectUtils.createImage(12, 12, 0);
        Graphics graphics = image.getGraphics();
        graphics.translate(2, 1);
        graphics.setClip(1, 1, 8, 8);
        Graphics3D engine = new Graphics3D();
        FigureLayout layout = new FigureLayout();
        layout.setScale(4096, 4096);
        Effect3D effect = new Effect3D();
        int command = Graphics3D.PRIMITVE_POINTS | Graphics3D.PDATA_COLOR_PER_FACE;
        int[] point = { 0, 0, 32 };
        engine.bind(graphics);
        engine.renderPrimitives(null, 3, 3, layout, effect, command, 1, point,
                                null, null, new int[] { 0xff0000 });
        engine.flush();
        Image green = Image.createRGBImage(new int[] { 0xff00ff00 }, 1, 1, false);
        graphics.drawImage(green, 5, 5, Graphics.TOP | Graphics.LEFT);
        engine.renderPrimitives(null, 4, 4, layout, effect, command, 1, point,
                                null, null, new int[] { 0x0000ff });
        engine.flush();
        engine.release(graphics);
        engine.dispose();

        int[] pixels = new int[144];
        image.getRGB(pixels, 0, 12, 0, 0, 12, 12);
        boolean alpha = (pixels[4 * 12 + 5] & 0xff808080) == 0xff800000 &&
            (pixels[5 * 12 + 6] & 0xff808080) == 0xff000080 && pixels[0] == 0;
        boolean overlay = (pixels[6 * 12 + 7] & 0xff808080) == 0xff008000;
        boolean graphicsRestored = graphics.getTranslateX() == 2 &&
            graphics.getTranslateY() == 1 && graphics.getClipX() == 1 &&
            graphics.getClipY() == 1 && graphics.getClipWidth() == 8 &&
            graphics.getClipHeight() == 8;
        say("mascot pixels alpha=" + alpha + " overlay=" + overlay +
            " graphicsRestored=" + graphicsRestored);
    }

    private Texture indexedTexture() {
        int paletteOffset = 54;
        int rasterOffset = paletteOffset + 129 * 4;
        byte[] bmp = new byte[rasterOffset + 8];
        bmp[0] = 'B';
        bmp[1] = 'M';
        bmp[2] = (byte) bmp.length;
        bmp[3] = (byte) (bmp.length >> 8);
        bmp[10] = (byte) rasterOffset;
        bmp[11] = (byte) (rasterOffset >> 8);
        bmp[14] = 40; // DIB header size
        bmp[18] = 4; // width
        bmp[22] = 2; // height
        bmp[26] = 1; // planes
        bmp[28] = 8; // bits per pixel
        bmp[46] = (byte) 129; // palette entries
        bmp[paletteOffset + 2] = (byte) 255; // red at index 0
        bmp[paletteOffset + 4 + 1] = (byte) 255; // green at index 1
        bmp[paletteOffset + 128 * 4] = (byte) 255; // blue at index 128
        // The BMP stores the bottom row first.
        bmp[rasterOffset] = (byte) 128;
        bmp[rasterOffset + 2] = 1;
        bmp[rasterOffset + 3] = (byte) 128;
        bmp[rasterOffset + 5] = 1;
        bmp[rasterOffset + 6] = (byte) 128;
        return new Texture(bmp, true);
    }

    private void mascotTexturedPixels() {
        Image image = DirectUtils.createImage(12, 10, 0);
        Graphics graphics = image.getGraphics();
        graphics.setClip(2, 2, 6, 3);
        Graphics3D engine = new Graphics3D();
        FigureLayout layout = new FigureLayout();
        layout.setScale(4096, 4096);
        Light light = new Light();
        light.setDirIntensity(0);
        light.setAmbientIntensity(2048);
        Effect3D effect = new Effect3D();
        effect.setLight(light);
        Texture texture = indexedTexture();
        int command = Graphics3D.PRIMITVE_TRIANGLES | Graphics3D.PDATA_TEXURE_COORD |
            Graphics3D.PDATA_NORMAL_PER_FACE | Graphics3D.PATTR_LIGHTING;
        int[] vertices = { 0, 0, 32, 12, 0, 32, 0, 8, 32 };
        int[] normals = { 0, 0, 4096 };
        int[] uv = { 0, 0, 12, 0, 0, 8 };
        int[] row = new int[12];

        engine.bind(graphics);
        engine.renderPrimitives(texture, 0, 0, layout, effect, command | Graphics3D.PATTR_COLORKEY,
                                1, vertices, normals, uv, null);
        engine.flush();
        image.getRGB(row, 0, 12, 0, 2, 12, 1);
        int pixelMask = 0xfff8fcf8; // Stored RGB565 bits and alpha.
        boolean keyed = (row[2] & pixelMask) == 0xff000080 && row[3] == 0 &&
            (row[4] & pixelMask) == 0xff000080 && row[5] == 0 &&
            (row[6] & pixelMask) == 0xff008000;
        boolean clipped = row[1] == 0;
        image.getRGB(row, 0, 12, 0, 3, 12, 1);
        boolean textureRows = (row[2] & pixelMask) == 0xff008000 &&
            (row[3] & pixelMask) == 0xff000080 && (row[4] & pixelMask) == 0xff000080;
        image.getRGB(row, 0, 12, 0, 1, 12, 1);
        clipped &= row[2] == 0;

        engine.renderPrimitives(texture, 0, 0, layout, effect, command, 1, vertices, normals,
                                uv, null);
        engine.flush();
        engine.release(graphics);
        engine.dispose();
        texture.dispose();
        image.getRGB(row, 0, 12, 0, 2, 12, 1);
        boolean zeroIndexDrawn = (row[3] & pixelMask) == 0xff800000;
        clipped &= row[8] == 0;
        say("mascot texture keyed=" + keyed + " rows=" + textureRows + " clipped=" + clipped +
            " zeroIndexDrawn=" + zeroIndexDrawn);
    }

    private void nokiaDecodedImage() throws IOException {
        byte[] png = Base64.decode(ALPHA_PNG);
        byte[] data = new byte[png.length + 6];
        int offset = 3;
        System.arraycopy(png, 0, data, offset, png.length);

        Image immutable = Image.createImage(data, offset, png.length);
        Image image = DirectUtils.createImage(data, offset, png.length);
        DirectGraphics direct = DirectUtils.getDirectGraphics(image.getGraphics());
        short[] pixels = new short[3];
        direct.getPixels(pixels, 0, 3, 0, 0, 3, 1, DirectGraphics.TYPE_USHORT_4444_ARGB);
        boolean decoded = pixels[0] == 0 && pixels[1] == (short) 0x8f00
            && pixels[2] == (short) 0xf0f0;

        direct.drawPixels(new int[] { 0xff0000ff }, true, 0, 1, 0, 0, 1, 1, 0,
            DirectGraphics.TYPE_INT_8888_ARGB);
        direct.getPixels(pixels, 0, 3, 0, 0, 3, 1, DirectGraphics.TYPE_USHORT_4444_ARGB);
        boolean written = pixels[0] == (short) 0xf00f &&
            pixels[1] == (short) 0x8f00 && pixels[2] == (short) 0xf0f0;
        say("nokia decoded image alpha=" + decoded + " written=" + written
            + " mutableOnlyOnNokia=" + (image.isMutable() && !immutable.isMutable()));
    }

    private void nokiaTextValidation() {
        Image image = Image.createImage(2, 2);
        Graphics graphics = image.getGraphics();
        DirectGraphics direct = DirectUtils.getDirectGraphics(graphics);
        direct.setARGBColor(0);
        boolean range = false;
        try {
            graphics.drawChars(new char[] { 'x' }, -1, 1, 0, 0, Graphics.TOP | Graphics.LEFT);
        } catch (ArrayIndexOutOfBoundsException expected) {
            range = true;
        }
        direct.setARGBColor(0x80000000);
        boolean anchor = false;
        try {
            graphics.drawString("x", -10, -10, Graphics.VCENTER | Graphics.LEFT);
        } catch (IllegalArgumentException expected) {
            anchor = true;
        }
        say("nokia text range=" + range + " clippedAnchor=" + anchor);
    }

    private void vodafoneTypes() {
        com.vodafone.v10.graphics.j3d.AffineTrans transform =
            new com.vodafone.v10.graphics.j3d.AffineTrans();
        com.vodafone.v10.graphics.j3d.FigureLayout layout =
            new com.vodafone.v10.graphics.j3d.FigureLayout(transform, 512, 512, 0, 0);
        com.vodafone.v10.graphics.j3d.Vector3D direction =
            new com.vodafone.v10.graphics.j3d.Vector3D(1, 2, 3);
        com.vodafone.v10.graphics.j3d.Light light =
            new com.vodafone.v10.graphics.j3d.Light(direction, 4096, 0);
        com.vodafone.v10.graphics.j3d.Effect3D effect =
            new com.vodafone.v10.graphics.j3d.Effect3D();
        effect.setLight(light);
        say("vodafone layout=" + (layout.getAffineTrans() == transform)
            + " light=" + (effect.getLight() == light)
            + " direction=" + (light.getDirection().z == 3));
    }

    private void nokiaShapes() {
        Image contour = DirectUtils.createImage(4, 4, 0);
        Graphics graphics = contour.getGraphics();
        graphics.setColor(0x123456);
        DirectGraphics direct = DirectUtils.getDirectGraphics(graphics);
        direct.drawTriangle(0, 0, 3, 0, 0, 3, 0x80ff0000);
        int red = 0x80ff0000;
        int[] triangle = {
            red, red, red, red,
            red,   0, red,   0,
            red, red,   0,   0,
            red,   0,   0,   0,
        };
        boolean corner = matchesPixels(contour, triangle);
        direct.fillTriangle(0, 0, 3, 0, 0, 3, 0x0000ff00);
        boolean transparent = matchesPixels(contour, triangle);

        Image concave = DirectUtils.createImage(6, 6, 0);
        Graphics clipped = concave.getGraphics();
        clipped.setColor(0x654321);
        clipped.translate(1, 1);
        clipped.setClip(0, 1, 4, 3);
        DirectGraphics polygons = DirectUtils.getDirectGraphics(clipped);
        polygons.fillPolygon(new int[] { 99, 0, 4, 4, 3, 3, 1, 1, 0 }, 1,
                             new int[] { 99, 99, 0, 0, 4, 4, 1, 1, 4, 4 }, 2,
                             8, 0x8000ff00);
        int green = 0x8000ff00;
        int[] concavePixels = {
            0,     0, 0, 0,     0, 0,
            0,     0, 0, 0,     0, 0,
            0, green, 0, 0, green, 0,
            0, green, 0, 0, green, 0,
            0, green, 0, 0, green, 0,
            0,     0, 0, 0,     0, 0,
        };
        boolean evenOdd = matchesPixels(concave, concavePixels);
        // Identical, overlapping contours cancel under the even-odd rule.
        polygons.fillPolygon(new int[] { 0, 4, 4, 0, 0, 4, 4, 0 }, 0,
                             new int[] { 0, 0, 4, 4, 0, 0, 4, 4 }, 0,
                             8, 0xffffffff);
        evenOdd = evenOdd && matchesPixels(concave, concavePixels);

        Image line = DirectUtils.createImage(3, 2, 0);
        DirectGraphics segments = DirectUtils.getDirectGraphics(line.getGraphics());
        segments.drawPolygon(new int[] { 99, Integer.MIN_VALUE, Integer.MAX_VALUE }, 1,
                             new int[] { 1, 1 }, 0, 2, 0x800000ff);
        int blue = 0x800000ff;
        boolean segment = matchesPixels(line, new int[] { 0, 0, 0, blue, blue, blue });

        Image opaque = Image.createImage(4, 4);
        Graphics background = opaque.getGraphics();
        background.setColor(0);
        background.fillRect(0, 0, 4, 4);
        DirectUtils.getDirectGraphics(background)
            .fillTriangle(0, 0, 3, 0, 0, 3, 0x80ff0000);
        int blended = 0xff840000;
        int black = 0xff000000;
        boolean opaqueBlend = matchesPixels(opaque, new int[] {
            blended, blended, blended, blended,
            blended, blended, blended,   black,
            blended, blended,   black,   black,
            blended,   black,   black,   black,
        });
        boolean colorPreserved = graphics.getColor() == 0x123456 &&
            clipped.getColor() == 0x654321 && background.getColor() == 0;
        say("nokia shapes corner=" + corner + " transparent=" + transparent
            + " evenOdd=" + evenOdd + " segment=" + segment
            + " opaqueBlend=" + opaqueBlend + " colorPreserved=" + colorPreserved);
    }

    private static boolean matchesPixels(Image image, int[] expected) {
        int width = image.getWidth();
        int[] actual = new int[expected.length];
        image.getRGB(actual, 0, width, 0, 0, width, image.getHeight());
        for (int i = 0; i < actual.length; i++) {
            if (actual[i] != expected[i]) {
                return false;
            }
        }
        return true;
    }

    private static final class TrackingStream extends ByteArrayInputStream {
        boolean closed;

        TrackingStream(byte[] bytes) {
            super(bytes);
        }

        public void close() {
            closed = true;
        }
    }

    private void imageStream() throws IOException {
        TrackingStream valid = new TrackingStream(Base64.decode(RED_PNG));
        Image decoded = Image.createImage(valid);
        TrackingStream broken = new TrackingStream(new byte[] { 42 });
        boolean failed = false;
        try {
            Image.createImage(broken);
        } catch (IOException expected) {
            failed = true;
        }
        say("image stream size=" + decoded.getWidth() + "x" + decoded.getHeight()
            + " openAfterDecode=" + !valid.closed
            + " openAfterError=" + (failed && !broken.closed));
    }

    /** setFullScreenMode(true) from showNotify(): Display follows. */
    private void showFull() throws InterruptedException {
        ProbeShowFullCanvas c = new ProbeShowFullCanvas();
        display.setCurrent(c);
        waitFor(c, "paint", 0);
        Thread.sleep(200);
        say("shownotify full size=" + c.getWidth() + "x" + c.getHeight()
            + " clip=" + c.lastClipH);
    }

    /** Polls until a canvas condition holds; prints "timeout <what>" if not. */
    private void waitFor(ProbeCanvas c, String what, int notHeight)
            throws InterruptedException {
        for (int waited = 0; waited < TIMEOUT_MS; waited += POLL_MS) {
            boolean ok;
            if (what.equals("show")) {
                ok = c.showCount > 0;
            } else if (what.equals("paint")) {
                ok = c.paintCount > 0;
            } else {
                ok = c.lastH != -1 && c.lastH != notHeight;
            }
            if (ok) {
                return;
            }
            Thread.sleep(POLL_MS);
        }
        say("timeout " + what);
    }

    private void keys(Canvas c) {
        int[] codes = new int[19];
        int n = 0;
        for (int k = -1; k >= -7; k--) {
            codes[n++] = k;
        }
        for (int k = 48; k <= 57; k++) {
            codes[n++] = k;
        }
        codes[n++] = 42;
        codes[n++] = 35;
        for (int i = 0; i < codes.length; i++) {
            String action;
            String name;
            try {
                action = Integer.toString(c.getGameAction(codes[i]));
            } catch (IllegalArgumentException e) {
                action = "IAE";
            }
            try {
                name = c.getKeyName(codes[i]);
            } catch (IllegalArgumentException e) {
                name = "IAE";
            }
            say("key " + codes[i] + " action=" + action + " name=" + name);
        }
        for (int i = 0; i < ACTIONS.length; i++) {
            String code;
            try {
                code = Integer.toString(c.getKeyCode(ACTIONS[i]));
            } catch (IllegalArgumentException e) {
                code = "IAE";
            }
            say("code " + ACTION_NAMES[i] + " " + code);
        }
    }

    private void fullScreen(ProbeCanvas c, boolean on, String label)
            throws InterruptedException {
        int oldHeight = c.getHeight();
        c.lastW = -1;
        c.lastH = -1;
        c.setFullScreenMode(on);
        waitFor(c, "sizeChanged " + label, oldHeight);
        say(label + " size=" + c.getWidth() + "x" + c.getHeight()
            + " sizeChanged=" + c.lastW + "x" + c.lastH);
    }

    private void gameSmoke(ProbeCanvas first) throws InterruptedException {
        ProbeGameCanvas game = new ProbeGameCanvas();
        display.setCurrent(game);
        boolean ok = false;
        for (int waited = 0; waited < TIMEOUT_MS && !ok; waited += POLL_MS) {
            ok = game.showCount > 0 && first.hideCount > 0;
            if (!ok) {
                Thread.sleep(POLL_MS);
            }
        }
        if (!ok) {
            say("timeout game show");
        }
        say("hide count=" + first.hideCount);
        int drawCalls;
        for (drawCalls = 0; drawCalls < 10; drawCalls++) {
            game.drawFrame(drawCalls);
        }
        say("headless GameCanvas size=" + game.getWidth() + "x" + game.getHeight()
            + " drawCalls=" + drawCalls + " keyStates=" + game.getKeyStates());
    }

    private void font() {
        Font f = Font.getDefaultFont();
        char[] chars = SAMPLE.toCharArray();
        boolean height = f.getHeight() > 0;
        boolean width = f.stringWidth(SAMPLE)
            == f.charsWidth(chars, 0, chars.length);
        boolean substring = f.substringWidth(SAMPLE, 0, SAMPLE.length())
                == f.stringWidth(SAMPLE)
            && f.substringWidth(SAMPLE, 2, 5)
                == f.stringWidth(SAMPLE.substring(2, 7));
        say("font height=" + height + " width=" + width + " substring="
            + substring);
    }
}
