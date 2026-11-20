// SPDX-License-Identifier: GPL-2.0-only
package fplinux.fixtures.canvas;

import com.nokia.mid.ui.DirectUtils;
import java.io.IOException;
import javax.microedition.lcdui.Graphics;
import javax.microedition.lcdui.Image;
import javax.microedition.m3g.AnimationController;
import javax.microedition.m3g.AnimationTrack;
import javax.microedition.m3g.Appearance;
import javax.microedition.m3g.Camera;
import javax.microedition.m3g.CompositingMode;
import javax.microedition.m3g.Fog;
import javax.microedition.m3g.Graphics3D;
import javax.microedition.m3g.Group;
import javax.microedition.m3g.Image2D;
import javax.microedition.m3g.KeyframeSequence;
import javax.microedition.m3g.Light;
import javax.microedition.m3g.Loader;
import javax.microedition.m3g.Material;
import javax.microedition.m3g.Mesh;
import javax.microedition.m3g.MorphingMesh;
import javax.microedition.m3g.Object3D;
import javax.microedition.m3g.PolygonMode;
import javax.microedition.m3g.SkinnedMesh;
import javax.microedition.m3g.Sprite3D;
import javax.microedition.m3g.Texture2D;
import javax.microedition.m3g.Transform;
import javax.microedition.m3g.TriangleStripArray;
import javax.microedition.m3g.VertexArray;
import javax.microedition.m3g.VertexBuffer;

final class M3gProbe {
    // M3G 1.0 header and a zlib section with controller 42, plus an outer prefix and suffix.
    private static final byte[] CONTROLLER = {
        102, 112, -85, 74, 83, 82, 49, 56, 52, -69, 13, 10,
        26, 10, 0, 30, 0, 0, 0, 17, 0, 0, 0, 0,
        12, 0, 0, 0, 1, 0, 0, 82, 0, 0, 0, 82,
        0, 0, 0, 0, -31, 0, -75, 9, 1, 40, 0, 0,
        0, 41, 0, 0, 0, 120, -100, 99, 84, 97, 96, 96,
        -48, 98, 64, 6, 13, -10, 16, 12, 3, 19, 28, -72,
        24, 24, 24, 0, 45, -4, 2, -88, -37, 8, -116, -115,
        101, 110, 100,
    };

    private M3gProbe() {}

    static String check() throws IOException {
        Object3D[] objects = Loader.load(CONTROLLER, 2);
        boolean compressed = objects.length == 1 && objects[0].getUserID() == 42 &&
            ((AnimationController) objects[0]).getPosition(15) == 9.5f;
        AnimationController control = new AnimationController();
        KeyframeSequence times = new KeyframeSequence(2, 3, KeyframeSequence.LINEAR);
        times.setKeyframe(0, 0, new float[] { 0, 0, 0 });
        times.setKeyframe(1, 100, new float[] { 100, 0, 0 });
        times.setDuration(100);
        AnimationTrack track = new AnimationTrack(times, AnimationTrack.TRANSLATION);
        track.setController(control);
        Group group = new Group();
        group.addAnimationTrack(track);
        group.animate(25);
        boolean animated = x(group) == 25;
        control.setActiveInterval(30, 50);
        group.animate(37);
        group.animate(50);
        animated &= x(group) == 37;

        Image target = DirectUtils.createImage(10, 10, 0);
        Graphics graphics = target.getGraphics();
        graphics.translate(1, 1);
        graphics.setClip(1, 2, 4, 4);
        Graphics3D renderer = Graphics3D.getInstance();
        Appearance translucent = new Appearance();
        CompositingMode blend = new CompositingMode();
        blend.setBlending(CompositingMode.ALPHA);
        translucent.setCompositingMode(blend);
        renderer.bindTarget(graphics);
        try {
            renderer.clear(null);
            renderer.setCamera(new Camera(), null);
            renderer.render(quad(0x80ff0000, translucent), null);
        } finally {
            renderer.releaseTarget();
        }
        int[] pixels = new int[100];
        target.getRGB(pixels, 0, 10, 0, 0, 10, 10);
        boolean seamAndClip = true;
        for (int y = 0; y < 10; y++) {
            for (int x = 0; x < 10; x++) {
                boolean inside = x >= 2 && x < 6 && y >= 3 && y < 7;
                int pixel = pixels[y * 10 + x];
                seamAndClip &= inside ? (pixel & 0xfff00000) == 0x40800000 : pixel == 0;
            }
        }
        boolean depth = depthAndHints(renderer, graphics, target, pixels);
        boolean lightingAndFog = lightingAndFog(renderer, graphics, target, pixels) &&
            directionalLighting(renderer);
        boolean alphaAndHud = alphaAndHud(renderer, pixels);
        return "m3g compressed=" + compressed + " animation=" + animated +
            " clipAndSeams=" + seamAndClip + " depth=" + depth + " lightingAndFog=" + lightingAndFog +
            " implicitIndices=" + implicitIndices(renderer, pixels) +
            " transformVectors=" + transformVectors() +
            " alphaAndHud=" + alphaAndHud + " skinning=" + skinning(renderer, pixels) +
            " sprite=" + sprite(renderer, pixels) + " morphing=" + morphing(renderer, pixels) +
            " poseAndTexture=" + poseAndTexture() + " textureFilters=" + textureFilters() +
            " picking=" + picking();
    }

    private static boolean picking() {
        Group source = new Group();
        if (!source.isPickingEnabled()) return false;
        source.setPickingEnable(false);
        Group duplicate = (Group) source.duplicate();
        source.setPickingEnable(true);
        return source.isPickingEnabled() && !duplicate.isPickingEnabled() &&
            duplicate.isRenderingEnabled();
    }

    private static boolean sprite(Graphics3D renderer, int[] pixels) {
        int red = 0xffff0000, green = 0xff00ff00, blue = 0x400000ff;
        int cyan = 0xff00ffff, yellow = 0xffffff00;
        int[] source = { red, cyan, 0x80ffffff, yellow, 0, green };
        Image2D image = new Image2D(Image2D.RGBA, Image.createRGBImage(source, 3, 2, true));
        Appearance look = new Appearance();
        CompositingMode blend = new CompositingMode();
        blend.setAlphaThreshold(1);
        blend.setAlphaWriteEnable(false);
        look.setCompositingMode(blend);
        PolygonMode polygon = new PolygonMode();
        polygon.setCulling(PolygonMode.CULL_FRONT);
        look.setPolygonMode(polygon);
        look.setTexture(1, new Texture2D(image));
        Sprite3D scaled = new Sprite3D(true, image, look);
        scaled.setCrop(-1, -1, -4, -3);
        Group scene = new Group();
        // The turned sprite projects to 8 by 6 pixels at distance 2 in a 90-degree viewport.
        scaled.setOrientation(90, 0, 1, 0);
        scaled.setScale(3.2f, 2.4f, 1);
        scene.addChild(scaled);
        Camera camera = new Camera();
        camera.setPerspective(90, 1, 1, 9);
        Transform cameraPose = new Transform();
        cameraPose.postTranslate(0, 0, 2);
        Transform far = new Transform();
        far.postTranslate(0, 0, -1);
        far.postScale(3, 3, 1);

        Image target = DirectUtils.createImage(10, 10, 0x40000000);
        renderer.bindTarget(target.getGraphics());
        try {
            renderer.setCamera(camera, cameraPose);
            renderer.render(scene, null);
            renderer.render(quad(blue, new Appearance()), far);
        } finally {
            renderer.releaseTarget();
        }
        target.getRGB(pixels, 0, 10, 0, 0, 10, 10);
        int dimRed = 0x40ff0000, dimGreen = 0x4000ff00;
        int dimCyan = 0x4000ffff, dimYellow = 0x40ffff00;
        int[][] visible = {
            { dimGreen, dimGreen, blue, blue, dimYellow, dimYellow },
            { dimGreen, dimGreen, blue, blue, dimYellow, dimYellow },
            { blue, blue, dimCyan, dimCyan, dimRed, dimRed },
            { blue, blue, dimCyan, dimCyan, dimRed, dimRed },
        };
        for (int row = 0; row < 10; row++) {
            for (int col = 0; col < 10; col++) {
                int expected = row >= 2 && row < 6 && col >= 1 && col < 7 ?
                    visible[row - 2][col - 1] : blue;
                if (pixels[row * 10 + col] != expected) return false;
            }
        }

        Sprite3D unscaled = new Sprite3D(false, image, look);
        blend.setAlphaWriteEnable(true);
        Transform pose = new Transform();
        scaled.getCompositeTransform(pose);
        renderer.bindTarget(target.getGraphics());
        try {
            renderer.clear(null);
            renderer.render(unscaled, pose);
        } finally {
            renderer.releaseTarget();
        }
        target.getRGB(pixels, 0, 10, 0, 0, 10, 10);
        return pixels[43] == 0 && pixels[44] == red && pixels[45] == cyan &&
            pixels[46] == 0 && pixels[54] == yellow && pixels[55] == 0 &&
            pixels[56] == green && pixels[57] == 0 && pixels[64] == 0;
    }

    private static boolean implicitIndices(Graphics3D renderer, int[] pixels) {
        VertexArray positions = new VertexArray(9, 3, 1);
        positions.set(0, 9, new byte[] {
            0, 0, 0,
            -9, 6, 0, -9, -6, 0, -3, 6, 0, -3, -6, 0,
             3, 6, 0,  3, -6, 0,  9, 6, 0,  9, -6, 0
        });
        VertexBuffer vertices = new VertexBuffer();
        vertices.setPositions(positions, .1f, null);
        vertices.setDefaultColor(0xff00ff00);
        TriangleStripArray strips = new TriangleStripArray(1, new int[] { 4, 4 });
        Image target = DirectUtils.createImage(10, 10, 0);
        renderMesh(renderer, target, new Mesh(vertices, strips, new Appearance()), pixels);
        if (pixels[51] != 0xff00ff00 || pixels[58] != 0xff00ff00 || pixels[55] != 0) {
            return false;
        }
        new TriangleStripArray(65532, new int[] { 3 });
        try {
            new TriangleStripArray(65533, new int[] { 3 });
            return false;
        } catch (IndexOutOfBoundsException expected) {}
        try {
            new TriangleStripArray(0, new int[] { 3, Integer.MAX_VALUE });
            return false;
        } catch (IndexOutOfBoundsException expected) {}
        try {
            new TriangleStripArray(0, new int[] { 2 });
            return false;
        } catch (IllegalArgumentException expected) {
            return true;
        }
    }

    private static boolean skinning(Graphics3D renderer, int[] pixels) {
        VertexArray positions = new VertexArray(4, 3, 1);
        positions.set(0, 4, new byte[] { -8, 4, 0, -8, -4, 0, -4, 4, 0, -4, -4, 0 });
        VertexBuffer vertices = new VertexBuffer();
        vertices.setPositions(positions, .1f, null);
        vertices.setDefaultColor(0xff00ff00);
        Group skeleton = new Group();
        Group firstBone = new Group();
        Group secondBone = new Group();
        skeleton.addChild(firstBone);
        skeleton.addChild(secondBone);
        SkinnedMesh mesh = new SkinnedMesh(vertices, strip(), new Appearance(), skeleton);
        mesh.addTransform(firstBone, 1, 0, 4);
        mesh.addTransform(secondBone, 3, 0, 4);

        Image target = DirectUtils.createImage(10, 10, 0);
        renderMesh(renderer, target, mesh, pixels);
        boolean atRest = pixels[51] == 0xff00ff00 && pixels[56] == 0;

        secondBone.setTranslation(1.2f, 0, 0);
        renderMesh(renderer, target, mesh, pixels);
        return skeleton.getParent() == mesh && atRest &&
            pixels[51] == 0 && pixels[56] == 0xff00ff00;
    }

    private static void renderMesh(Graphics3D renderer, Image target, Mesh mesh, int[] pixels) {
        renderMesh(renderer, target, mesh, null, pixels);
    }

    private static void renderMesh(Graphics3D renderer, Image target, Mesh mesh, Transform placement,
                                   int[] pixels) {
        renderer.bindTarget(target.getGraphics());
        try {
            renderer.clear(null);
            renderer.setCamera(new Camera(), null);
            renderer.render(mesh, placement);
        } finally {
            renderer.releaseTarget();
        }
        target.getRGB(pixels, 0, 10, 0, 0, 10, 10);
    }

    private static boolean morphing(Graphics3D renderer, int[] pixels) {
        VertexArray basePositions = new VertexArray(4, 3, 1);
        basePositions.set(0, 4, new byte[] { -8, 4, 0, -8, -4, 0, -4, 4, 0, -4, -4, 0 });
        VertexArray targetPositions = new VertexArray(4, 3, 1);
        targetPositions.set(0, 4, new byte[] { 10, 4, 0, 10, -4, 0, 14, 4, 0, 14, -4, 0 });
        VertexBuffer base = new VertexBuffer();
        base.setPositions(basePositions, .1f, null);
        base.setDefaultColor(0xff00ff00);
        VertexBuffer destination = new VertexBuffer();
        destination.setPositions(targetPositions, 1, null);
        MorphingMesh mesh = new MorphingMesh(base, new VertexBuffer[] { destination },
                                            strip(), new Appearance());

        Image target = DirectUtils.createImage(10, 10, 0);
        renderMesh(renderer, target, mesh, pixels);
        boolean atRest = pixels[51] == 0xff00ff00 && pixels[56] == 0;

        mesh.setWeights(new float[] { .5f });
        renderMesh(renderer, target, mesh, pixels);
        return atRest && pixels[51] == 0 && pixels[56] == 0xff00ff00;
    }

    private static boolean poseAndTexture() {
        Group node = new Group();
        node.setTranslation(1, 2, 3);
        node.translate(3, 4, 5);
        float[] values = new float[4];
        node.getTranslation(values);
        boolean translation = values[0] == 4 && values[1] == 6 && values[2] == 8;
        node.setScale(1, 2, 3);
        node.scale(6, 5, 4);
        node.getScale(values);
        boolean scale = values[0] == 6 && values[1] == 10 && values[2] == 12;

        Group rotated = new Group();
        rotated.setOrientation(143, 1, 1, 0);
        rotated.postRotate(62, 0, 1, 0);
        Transform expected = new Transform();
        rotated.getCompositeTransform(expected);
        rotated.getOrientation(values);
        Transform restored = new Transform();
        restored.postRotate(values[0], values[1], values[2], values[3]);
        float[] a = new float[16];
        float[] b = new float[16];
        expected.get(a);
        restored.get(b);
        for (int i = 0; i < a.length; i++) {
            if (!(Math.abs(a[i] - b[i]) <= .0001f)) return false;
        }

        Texture2D texture = new Texture2D(new Image2D(Image2D.RGBA,
            DirectUtils.createImage(1, 1, 0xffff0000)));
        Image2D replacement = new Image2D(Image2D.RGBA,
            DirectUtils.createImage(1, 1, 0xff0000ff));
        texture.setImage(replacement);
        return translation && scale && texture.getImage() == replacement;
    }

    private static boolean textureFilters() {
        Texture2D texture = new Texture2D(new Image2D(Image2D.RGBA,
            DirectUtils.createImage(1, 1, 0xffff0000)));
        texture.setFiltering(Texture2D.FILTER_NEAREST, Texture2D.FILTER_NEAREST);
        if (texture.getLevelFilter() != Texture2D.FILTER_NEAREST ||
                texture.getImageFilter() != Texture2D.FILTER_NEAREST) {
            return false;
        }
        texture.setFiltering(Texture2D.FILTER_LINEAR, Texture2D.FILTER_NEAREST);
        Texture2D copy = (Texture2D) texture.duplicate();
        try {
            texture.setFiltering(Texture2D.FILTER_NEAREST, Texture2D.FILTER_BASE_LEVEL);
            return false;
        } catch (IllegalArgumentException expected) {
            return texture.getLevelFilter() == Texture2D.FILTER_LINEAR &&
                texture.getImageFilter() == Texture2D.FILTER_NEAREST &&
                copy.getLevelFilter() == Texture2D.FILTER_LINEAR &&
                copy.getImageFilter() == Texture2D.FILTER_NEAREST;
        }
    }

    private static boolean transformVectors() {
        Transform transform = new Transform();
        transform.set(new float[] { 2, 1, 0, 5, 0, 3, -1, -2, 0, 0, 4, 7, 1, 0, 0, 1 });
        float[] vectors = { 1, 2, 3, 1, 1, -2, 3, 0 };
        float[] expected = { 9, 1, 19, 2, 0, -9, 12, 1 };
        transform.transform(vectors);
        for (int i = 0; i < vectors.length; i++) {
            if (vectors[i] != expected[i]) return false;
        }
        try {
            transform.transform(new float[5]);
            return false;
        } catch (IllegalArgumentException invalidLength) {
            return true;
        }
    }

    private static boolean depthAndHints(Graphics3D renderer, Graphics graphics, Image target,
                                         int[] pixels) {
        try {
            renderer.bindTarget(graphics, true, 1);
            renderer.releaseTarget();
            return false;
        } catch (IllegalArgumentException expected) {}
        boolean first = depthPixel(renderer, graphics, target, pixels, true, 0, 0xff00ff00);
        boolean disabled = depthPixel(renderer, graphics, target, pixels, false, 30, 0xff0000ff);
        boolean enabled = depthPixel(renderer, graphics, target, pixels, true, 0, 0xff00ff00);
        return first && disabled && enabled;
    }

    private static boolean depthPixel(Graphics3D renderer, Graphics graphics, Image target,
                                      int[] pixels, boolean depth, int hints, int expected) {
        Appearance look = new Appearance();
        TriangleStripArray indices = new TriangleStripArray(0, new int[] { 4 });
        Transform near = new Transform();
        near.postTranslate(0, 0, -.5f);
        renderer.bindTarget(graphics, depth, hints);
        try {
            renderer.clear(null);
            renderer.render(quad(0xff00ff00, look).getVertexBuffer(), indices, look, near);
            renderer.render(quad(0xff0000ff, look).getVertexBuffer(), indices, look, null);
        } finally {
            renderer.releaseTarget();
        }
        target.getRGB(pixels, 0, 10, 0, 0, 10, 10);
        return pixels[45] == expected && pixels[42] == expected && pixels[46] == 0;
    }

    private static boolean alphaAndHud(Graphics3D renderer, int[] pixels) {
        Image target = DirectUtils.createImage(10, 10, 0x4000ffff);
        Graphics graphics = target.getGraphics();
        graphics.translate(1, 1);
        graphics.setClip(1, 2, 4, 4);
        Appearance look = new Appearance();
        if (look.getPolygonMode() != null) return false;
        PolygonMode mode = new PolygonMode();
        look.setPolygonMode(mode);
        if (look.getPolygonMode() != mode) return false;
        CompositingMode blend = new CompositingMode();
        look.setCompositingMode(blend);
        Mesh mesh = quad(0x80ff0000, look);
        blend.setAlphaWriteEnable(false);
        renderOverHud(renderer, graphics, mesh);
        target.getRGB(pixels, 0, 10, 0, 0, 10, 10);
        boolean alphaPreserved = pixels[45] == 0x40ff0000;
        blend.setAlphaWriteEnable(true);
        renderOverHud(renderer, graphics, mesh);
        graphics.setColor(0xffff00);
        graphics.fillRect(0, 1, 2, 2);
        target.getRGB(pixels, 0, 10, 0, 0, 10, 10);
        return alphaPreserved && pixels[45] == 0x80ff0000 &&
            (pixels[32] & 0xffffff) == 0xffff00 &&
            pixels[31] == 0x4000ffff && pixels[22] == 0x4000ffff;
    }

    private static void renderOverHud(Graphics3D renderer, Graphics graphics, Mesh mesh) {
        renderer.bindTarget(graphics);
        try {
            renderer.render(mesh, null);
        } finally {
            renderer.releaseTarget();
        }
    }

    private static boolean lightingAndFog(Graphics3D renderer, Graphics graphics, Image target,
                                          int[] pixels) {
        Appearance look = new Appearance();
        Material material = new Material();
        material.setColor(Material.DIFFUSE, 0xff000000);
        material.setColor(Material.SPECULAR, 0xff0000);
        material.setShininess(4);
        look.setMaterial(material);
        Fog fog = new Fog();
        fog.setColor(0x0000ff);
        fog.setLinear(0, 1);
        look.setFog(fog);
        PolygonMode polygon = new PolygonMode();
        polygon.setCulling(PolygonMode.CULL_NONE);
        polygon.setTwoSidedLightingEnable(true);
        look.setPolygonMode(polygon);

        VertexBuffer buffer = quad(0xff000000, look).getVertexBuffer();
        VertexArray normals = new VertexArray(4, 3, 1);
        normals.set(0, 4, new byte[] { 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1 });
        buffer.setNormals(normals);
        Transform direction = new Transform();
        direction.postRotate(60, 0, 1, 0);
        renderer.addLight(new Light(), direction);
        Transform placement = new Transform();
        placement.postTranslate(0, 0, -.5f);

        int front = litPixel(renderer, graphics, target, pixels, buffer, look, placement);
        polygon.setWinding(PolygonMode.WINDING_CW);
        int back = litPixel(renderer, graphics, target, pixels, buffer, look, placement);
        renderer.resetLights();

        Transform flat = new Transform(placement);
        flat.postScale(1, 1, 0);
        look.setFog(null);
        material.setColor(Material.EMISSIVE, 0x00ff00);
        int flatModel = litPixel(renderer, graphics, target, pixels, buffer, look, flat);

        boolean singularCameraRejected = false;
        try {
            renderer.setCamera(new Camera(), flat);
        } catch (ArithmeticException expected) {
            singularCameraRejected = true;
        }

        // The highlight's intensity is cos(30 degrees)^4. Half-distance fog halves its red
        // component and adds half of the blue fog color. The reversed normal gets no highlight.
        return Math.abs((front >>> 16 & 255) - 72) <= 8 &&
            Math.abs((front & 255) - 127) <= 8 &&
            (back & 0xffff0000) == 0xff000000 && (back & 255) == (front & 255) &&
            flatModel == 0xff00ff00 && singularCameraRejected;
    }

    private static int litPixel(Graphics3D renderer, Graphics graphics, Image target, int[] pixels,
                                VertexBuffer buffer, Appearance look, Transform placement) {
        renderer.bindTarget(graphics);
        try {
            renderer.clear(null);
            renderer.render(buffer, strip(), look, placement);
        } finally {
            renderer.releaseTarget();
        }
        target.getRGB(pixels, 0, 10, 0, 0, 10, 10);
        return pixels[45];
    }

    private static boolean directionalLighting(Graphics3D renderer) {
        Appearance look = new Appearance();
        Material material = new Material();
        material.setColor(Material.AMBIENT, 0x28140a);
        material.setColor(Material.DIFFUSE, 0xc0b4785a);
        material.setColor(Material.EMISSIVE, 0x080604);
        look.setMaterial(material);
        PolygonMode polygon = new PolygonMode();
        polygon.setShading(PolygonMode.SHADE_FLAT);
        look.setPolygonMode(polygon);
        look.setTexture(0, new Texture2D(new Image2D(Image2D.RGBA,
            DirectUtils.createImage(1, 1, 0x80ff00ff))));

        Mesh mesh = quad(0xffffffff, look);
        Transform placement = new Transform();
        placement.postScale(.5f, 1, 2);
        KeyframeSequence opacity = new KeyframeSequence(1, 1, KeyframeSequence.STEP);
        opacity.setKeyframe(0, 0, new float[] { .5f });
        opacity.setDuration(1);
        AnimationTrack fade = new AnimationTrack(opacity, AnimationTrack.ALPHA);
        fade.setController(new AnimationController());
        mesh.addAnimationTrack(fade);
        mesh.animate(0);
        VertexArray normals = new VertexArray(4, 3, 2);
        normals.set(0, 4, new short[] { 2, 0, 4, 2, 0, 4, 2, 0, 4, 2, 0, 4 });
        mesh.getVertexBuffer().setNormals(normals);

        Light ambient = new Light();
        ambient.setMode(Light.AMBIENT);
        ambient.setIntensity(1.046875f);
        Light directional = new Light();
        // The two addition orders round the red channel to 79 and 80, on either side of RGB565's boundary.
        directional.setIntensity(Float.intBitsToFloat(0x3ebc6d0b));
        Light extra = new Light();
        extra.setMode(Light.AMBIENT);
        extra.setIntensity(0);
        extra.setRenderingEnable(false);
        Image target = DirectUtils.createImage(10, 10, 0);
        int[] pixels = new int[100];
        int[] javaPixels = new int[100];
        try {
            for (int order = 0; order < 2; order++) {
                renderer.resetLights();
                renderer.addLight(order == 0 ? ambient : directional, null);
                renderer.addLight(order == 0 ? directional : ambient, null);
                renderer.addLight(extra, null);
                renderMesh(renderer, target, mesh, placement, pixels);
                // Activating the third light forces Java lighting and adds zero to each color channel.
                extra.setRenderingEnable(true);
                renderMesh(renderer, target, mesh, placement, javaPixels);
                extra.setRenderingEnable(false);
                int expected = order == 0 ? 0x30480000 : 0x30500000;
                if ((javaPixels[45] & 0xfff8ff00) != expected) {
                    System.err.println("M3G directional lighting order " + order +
                        ", Java center pixel 0x" + Integer.toHexString(javaPixels[45]) +
                        ", expected masked pixel 0x" + Integer.toHexString(expected));
                    return false;
                }
                for (int i = 0; i < pixels.length; i++) {
                    if (pixels[i] != javaPixels[i]) {
                        System.err.println("M3G directional lighting order " + order +
                            ", pixel " + i + " native 0x" + Integer.toHexString(pixels[i]) +
                            ", Java 0x" + Integer.toHexString(javaPixels[i]));
                        return false;
                    }
                }
            }
            normals.set(0, 4, new short[] { 2, 0, -4, 2, 0, -4, 2, 0, -4, 2, 0, -4 });
            renderMesh(renderer, target, mesh, placement, pixels);
            if ((pixels[45] & 0xfff8ff00) != 0x30300000) {
                System.err.println("M3G directional lighting back-facing center pixel 0x" +
                    Integer.toHexString(pixels[45]) + ", expected masked pixel 0x30300000");
                return false;
            }
            return true;
        } finally {
            renderer.resetLights();
        }
    }

    private static float x(Group group) {
        Transform transform = new Transform();
        float[] values = new float[16];
        group.getCompositeTransform(transform);
        transform.get(values);
        return values[3];
    }

    private static Mesh quad(int argb, Appearance appearance) {
        VertexArray vertices = new VertexArray(4, 3, 1);
        vertices.set(0, 4, new byte[] { -1, 1, 0, -1, -1, 0, 1, 1, 0, 1, -1, 0 });
        VertexBuffer buffer = new VertexBuffer();
        buffer.setPositions(vertices, 1, null);
        buffer.setDefaultColor(argb);
        return new Mesh(buffer, strip(), appearance);
    }

    private static TriangleStripArray strip() {
        return new TriangleStripArray(new int[] { 0, 1, 2, 3 }, new int[] { 4 });
    }
}
