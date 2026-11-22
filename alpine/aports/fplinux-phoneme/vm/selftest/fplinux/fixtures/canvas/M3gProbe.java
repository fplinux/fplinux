// SPDX-License-Identifier: GPL-2.0-only
package fplinux.fixtures.canvas;

import com.nokia.mid.ui.DirectUtils;
import java.io.IOException;
import java.util.Hashtable;
import java.util.Random;
import javax.microedition.lcdui.Graphics;
import javax.microedition.lcdui.Image;
import javax.microedition.m3g.AnimationController;
import javax.microedition.m3g.AnimationTrack;
import javax.microedition.m3g.Appearance;
import javax.microedition.m3g.Background;
import javax.microedition.m3g.Camera;
import javax.microedition.m3g.CompositingMode;
import javax.microedition.m3g.Fog;
import javax.microedition.m3g.Graphics3D;
import javax.microedition.m3g.Group;
import javax.microedition.m3g.Image2D;
import javax.microedition.m3g.IndexBuffer;
import javax.microedition.m3g.KeyframeSequence;
import javax.microedition.m3g.Light;
import javax.microedition.m3g.Loader;
import javax.microedition.m3g.Material;
import javax.microedition.m3g.Mesh;
import javax.microedition.m3g.MorphingMesh;
import javax.microedition.m3g.Node;
import javax.microedition.m3g.Object3D;
import javax.microedition.m3g.PolygonMode;
import javax.microedition.m3g.RayIntersection;
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
        boolean depthRange = depthRange(renderer, graphics, target, pixels);
        boolean lightingAndFog = lightingAndFog(renderer, graphics, target, pixels) &&
            directionalLighting(renderer);
        boolean alphaAndHud = alphaAndHud(renderer, pixels);
        return "m3g compressed=" + compressed + " animation=" + animated +
            " clipAndSeams=" + seamAndClip + " depth=" + depth + " depthRange=" + depthRange +
            " depthClears=" + depthClears(renderer) + " targetReads=" + targetReads(renderer) +
            " lightingAndFog=" + lightingAndFog +
            " implicitIndices=" + implicitIndices(renderer, pixels) +
            " submeshRanges=" + submeshRanges(renderer) +
            " transformVectors=" + transformVectors() + " transformMath=" + transformMath() +
            " alphaAndHud=" + alphaAndHud + " skinning=" + skinning(renderer, pixels) +
            " sprite=" + sprite(renderer, pixels) + " morphing=" + morphing(renderer, pixels) +
            " poseAndTexture=" + poseAndTexture() + " textureFilters=" + textureFilters() +
            " picking=" + picking() + " lightColor=" + lightColor(renderer, pixels) +
            " imageData=" + imageData(renderer, pixels) +
            " mutableImage=" + mutableImage(renderer, pixels) +
            " vertexColors=" + vertexColors(renderer, pixels) +
            " pickRay=" + pickRay() + " pickCamera=" + pickCamera() +
            " pickDeformed=" + pickDeformed() + " pickDeformedNormals=" + pickDeformedNormals() +
            " pickFacing=" + pickFacing() +
            " stateAccessors=" + stateAccessors(renderer) +
            " nodeAccessors=" + nodeAccessors() + " vertexAccessors=" + vertexAccessors() +
            " wideRows=" + wideRows(renderer);
    }

    // The color the target reports for an ARGB color drawn into it: the target
    // keeps RGB565, so each channel is cut to 5 or 6 bits and widened again.
    private static int rgb565(int argb) {
        int red = argb >> 19 & 31, green = argb >> 10 & 63, blue = argb >> 3 & 31;
        return argb & 0xff000000 | (red << 3 | red >> 2) << 16 | (green << 2 | green >> 4) << 8 |
            (blue << 3 | blue >> 2);
    }

    // A white diffuse quad facing the light takes the light's color.
    private static boolean lightColor(Graphics3D renderer, int[] pixels) {
        Appearance look = new Appearance();
        Material material = new Material();
        material.setColor(Material.DIFFUSE, 0xffffffff);
        look.setMaterial(material);
        Mesh mesh = quad(0xffffffff, look);
        VertexArray normals = new VertexArray(4, 3, 1);
        normals.set(0, 4, new byte[] { 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1 });
        mesh.getVertexBuffer().setNormals(normals);
        Light light = new Light();
        light.setColor(0x4080c0);
        Light dark = new Light();
        dark.setMode(Light.AMBIENT);
        dark.setIntensity(0);
        Image target = DirectUtils.createImage(10, 10, 0);
        try {
            renderer.resetLights();
            renderer.addLight(light, null);
            renderMesh(renderer, target, mesh, pixels);
            int directional = pixels[45];
            // An ambient light as well takes the renderer's other lighting path.
            renderer.addLight(dark, null);
            renderMesh(renderer, target, mesh, pixels);
            int withAmbient = pixels[45];
            light.setColor(0xffffff);
            renderMesh(renderer, target, mesh, pixels);
            return directional == rgb565(0xff4080c0) && withAmbient == rgb565(0xff4080c0) &&
                pixels[45] == 0xffffffff;
        } finally {
            renderer.resetLights();
        }
    }

    // Quad whose corners show the corners of the texture, first row at the top.
    private static Mesh texturedQuad(Image2D image) {
        Appearance look = new Appearance();
        Texture2D texture = new Texture2D(image);
        texture.setBlending(Texture2D.FUNC_REPLACE);
        look.setTexture(0, texture);
        Mesh mesh = quad(0xffffffff, look);
        VertexArray coordinates = new VertexArray(4, 2, 1);
        coordinates.set(0, 4, new byte[] { 0, 0, 0, 1, 1, 0, 1, 1 });
        mesh.getVertexBuffer().setTexCoords(0, coordinates, 1, null);
        return mesh;
    }

    private static boolean imageData(Graphics3D renderer, int[] pixels) {
        Image target = DirectUtils.createImage(10, 10, 0);
        byte[] rgb = { -1, 0, 0, 0, -1, 0, 0, 0, -1, -1, -1, -1 };
        renderMesh(renderer, target, texturedQuad(new Image2D(Image2D.RGB, 2, 2, rgb)), pixels);
        boolean direct = pixels[11] == 0xffff0000 && pixels[18] == 0xff00ff00 &&
            pixels[81] == 0xff0000ff && pixels[88] == 0xffffffff;

        byte[] indices = { 1, 0, 0, 1 };
        byte[] palette = { 10, 20, 30, -56, 100, 50 };
        Image2D indexed = new Image2D(Image2D.RGB, 2, 2, indices, palette);
        renderMesh(renderer, target, texturedQuad(indexed), pixels);
        boolean paletted = pixels[11] == rgb565(0xffc86432) && pixels[18] == rgb565(0xff0a141e) &&
            pixels[81] == rgb565(0xff0a141e) && pixels[88] == rgb565(0xffc86432);

        Image2D level = new Image2D(Image2D.LUMINANCE, 1, 1, new byte[] { -128 });
        renderMesh(renderer, target, texturedQuad(level), pixels);
        boolean luminance = pixels[45] == rgb565(0xff808080);
        Image2D levelAlpha = new Image2D(Image2D.LUMINANCE_ALPHA, 1, 1,
                                         new byte[] { 64, -128 });
        renderMesh(renderer, target, texturedQuad(levelAlpha), pixels);
        boolean luminanceAlpha = pixels[45] == rgb565(0x80404040);

        // Every corner has (s, t, r) = (0, 0, 1) and the texture maps s to
        // s + .75r: the quad shows the right texel, the left one without r.
        Mesh third = texturedQuad(new Image2D(Image2D.RGB, 2, 1,
                                              new byte[] { -1, 0, 0, 0, 0, -1 }));
        VertexArray stp = new VertexArray(4, 3, 1);
        stp.set(0, 4, new byte[] { 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1 });
        third.getVertexBuffer().setTexCoords(0, stp, 1, null);
        Transform withR = new Transform();
        withR.set(new float[] { 1, 0, .75f, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 });
        third.getAppearance(0).getTexture(0).setTransform(withR);
        renderMesh(renderer, target, third, pixels);
        boolean thirdCoordinate = pixels[45] == 0xff0000ff;

        boolean rejected = rejectsImage(Image2D.RGB, 2, 2, new byte[11], null) &&
            rejectsImage(Image2D.RGBA, 2, 2, new byte[15], null) &&
            rejectsImage(Image2D.RGB, 0, 2, new byte[12], null) &&
            rejectsImage(Image2D.ALPHA - 1, 2, 2, new byte[12], null) &&
            rejectsImage(Image2D.RGB, 2, 2, indices, new byte[] { 1, 2, 3, 4 }) &&
            rejectsImage(Image2D.RGB, 2, 2, new byte[] { 2, 0, 0, 0 }, palette) &&
            rejectsImage(Image2D.RGB, 2, 2, indices, new byte[0]) &&
            !rejectsImage(Image2D.RGBA, 2, 2, new byte[16], null);
        return direct && paletted && luminance && luminanceAlpha && thirdCoordinate && rejected &&
            level.getFormat() == Image2D.LUMINANCE && !level.isMutable() &&
            indexed.getWidth() == 2 && indexed.getHeight() == 2;
    }

    private static boolean rejectsImage(int format, int width, int height, byte[] data,
                                        byte[] palette) {
        try {
            if (palette == null) new Image2D(format, width, height, data);
            else new Image2D(format, width, height, data, palette);
            return false;
        } catch (IllegalArgumentException expected) {
            return true;
        }
    }

    private static boolean mutableImage(Graphics3D renderer, int[] pixels) {
        Image2D image = new Image2D(Image2D.RGB, 2, 2);
        boolean state = image.isMutable() && image.getFormat() == Image2D.RGB &&
            image.getWidth() == 2 && image.getHeight() == 2;
        // A new mutable image is opaque white; set replaces the top row only.
        image.set(0, 0, 2, 1, new byte[] { -1, 0, 0, 0, 0, -1 });
        Image target = DirectUtils.createImage(10, 10, 0);
        renderMesh(renderer, target, texturedQuad(image), pixels);
        boolean written = pixels[11] == 0xffff0000 && pixels[18] == 0xff0000ff &&
            pixels[81] == 0xffffffff && pixels[88] == 0xffffffff;
        image.set(1, 1, 1, 1, new byte[] { 0, -1, 0 });
        renderMesh(renderer, target, texturedQuad(image), pixels);
        written &= pixels[81] == 0xffffffff && pixels[88] == 0xff00ff00;

        Image2D immutable = new Image2D(Image2D.RGB, 1, 1, new byte[3]);
        boolean refused = false;
        try {
            immutable.set(0, 0, 1, 1, new byte[3]);
        } catch (IllegalStateException expected) {
            refused = true;
        }
        boolean bounds = true;
        try {
            image.set(1, 0, 2, 1, new byte[6]);
            bounds = false;
        } catch (IllegalArgumentException expected) {}
        try {
            image.set(0, 0, 2, 2, new byte[11]);
            bounds = false;
        } catch (IllegalArgumentException expected) {}
        return state && written && refused && bounds;
    }

    private static boolean vertexColors(Graphics3D renderer, int[] pixels) {
        Mesh mesh = quad(0xffffffff, new Appearance());
        VertexArray colors = new VertexArray(4, 3, 1);
        colors.set(0, 4, new byte[] { 32, 96, -96, 32, 96, -96, 32, 96, -96, 32, 96, -96 });
        mesh.getVertexBuffer().setColors(colors);
        Image target = DirectUtils.createImage(10, 10, 0);
        renderMesh(renderer, target, mesh, pixels);
        boolean uniform = pixels[45] == rgb565(0xff2060a0) && pixels[11] == rgb565(0xff2060a0);

        // Red at the left corners, blue at the right ones.
        colors.set(0, 4, new byte[] { -1, 0, 0, -1, 0, 0, 0, 0, -1, 0, 0, -1 });
        renderMesh(renderer, target, mesh, pixels);
        int left = pixels[51], right = pixels[58];
        boolean gradient = (left >>> 24) == 255 && (left >> 16 & 255) > 0xd0 && (left & 255) < 0x30 &&
            (right >> 16 & 255) < 0x30 && (right & 255) > 0xd0;
        return uniform && gradient && mesh.getVertexBuffer().getColors() == colors;
    }

    // Two copies of a mesh with a gap between its two submeshes: x from -4 to -1
    // and from 1 to 4, y from -3 to 3, at depth z. Texture coordinates run from
    // 0 to 1 over each submesh, t from the top.
    private static Mesh pickMesh(float z) {
        VertexArray positions = new VertexArray(8, 3, 1);
        positions.set(0, 8, new byte[] {
            -4, 3, 0, -4, -3, 0, -1, 3, 0, -1, -3, 0,
            1, 3, 0, 1, -3, 0, 4, 3, 0, 4, -3, 0,
        });
        VertexArray coordinates = new VertexArray(8, 2, 1);
        coordinates.set(0, 8, new byte[] { 0, 0, 0, 1, 1, 0, 1, 1, 0, 0, 0, 1, 1, 0, 1, 1 });
        VertexBuffer buffer = new VertexBuffer();
        buffer.setPositions(positions, 1, null);
        buffer.setTexCoords(0, coordinates, 1, null);
        Mesh mesh = new Mesh(buffer, new IndexBuffer[] {
            new TriangleStripArray(0, new int[] { 4 }),
            new TriangleStripArray(4, new int[] { 4 }),
        }, new Appearance[] { new Appearance(), new Appearance() });
        mesh.setTranslation(0, 0, z);
        return mesh;
    }

    private static boolean near(float actual, float expected) {
        return Math.abs(actual - expected) <= .001f;
    }

    // The meshes sit at world z -3 and -1 below a group scaled by 2.
    private static boolean pickRay() {
        Group root = new Group();
        Group scaled = new Group();
        scaled.setScale(2, 2, 2);
        Mesh back = pickMesh(-1.5f), front = pickMesh(-.5f);
        scaled.addChild(back);
        scaled.addChild(front);
        root.addChild(scaled);
        root.setTranslation(100, 0, 0);

        RayIntersection hit = new RayIntersection();
        boolean nearest = root.pick(-1, 5, 3, 5, 0, 0, -1, hit) && hit.getIntersected() == front &&
            near(hit.getDistance(), 6) && hit.getSubmeshIndex() == 1 &&
            near(hit.getNormalX(), 0) && near(hit.getNormalY(), 0) && near(hit.getNormalZ(), 1) &&
            near(hit.getTextureS(0), .5f) && near(hit.getTextureT(0), .25f) &&
            near(hit.getTextureS(1), 0);
        float[] ray = new float[6];
        hit.getRay(ray);
        float[] sent = { 5, 3, 5, 0, 0, -1 };
        for (int i = 0; i < 6; i++) nearest &= near(ray[i], sent[i]);

        boolean missLeavesHit = !root.pick(-1, 0, 0, 5, 0, 0, -1, hit) &&
            !root.pick(-1, 5, 3, 5, 0, 0, 1, hit) && hit.getIntersected() == front &&
            near(hit.getDistance(), 6);
        boolean query = root.pick(-1, 5, 3, 5, 0, 0, -1, null) &&
            !root.pick(-1, 0, 0, 5, 0, 0, -1, null);

        // A texture scaled by 2 and moved by .5 in s maps (.5, .25) to (1.5, .5).
        Texture2D texture = new Texture2D(new Image2D(Image2D.RGB, 2, 2, new byte[12]));
        texture.setTranslation(.5f, 0, 0);
        texture.setScale(2, 2, 1);
        front.getAppearance(1).setTexture(0, texture);
        boolean textureTransform = root.pick(-1, 5, 3, 5, 0, 0, -1, hit) &&
            near(hit.getTextureS(0), 1.5f) && near(hit.getTextureT(0), .5f);
        front.getAppearance(1).setTexture(0, null);
        boolean projective = pickProjective();

        front.setPickingEnable(false);
        boolean behind = root.pick(-1, 5, 3, 5, 0, 0, -1, hit) && hit.getIntersected() == back &&
            near(hit.getDistance(), 8);
        boolean leftSubmesh = root.pick(-1, -5, 0, 5, 0, 0, -1, hit) && hit.getSubmeshIndex() == 0 &&
            near(hit.getTextureS(0), .5f) && near(hit.getTextureT(0), .5f);

        back.setScope(2);
        boolean scope = !root.pick(1, 5, 3, 5, 0, 0, -1, hit) && root.pick(3, 5, 3, 5, 0, 0, -1, hit);
        back.setScope(-1);
        scaled.setPickingEnable(false);
        boolean groupOff = !root.pick(-1, 5, 3, 5, 0, 0, -1, hit);

        boolean arguments = false;
        try {
            root.pick(-1, 5, 3, 5, 0, 0, 0, null);
        } catch (IllegalArgumentException expected) {
            arguments = true;
        }
        return nearest && missLeavesHit && query && textureTransform && projective && behind &&
            leftSubmesh && scope && groupOff && arguments;
    }

    // The renderer divides by Q at each corner and interpolates the results,
    // so a pick reports those. The texture maps (s, t, r) to (s + r, t) / (s + 1).
    // The ray hits the first triangle of the strip with weights 1/2, 1/4, 1/4
    // for corners (0, 0, 2), (0, 4, 0) and (4, 0, 0), which give (2, 0), (0, 4)
    // and (.8, 0): S = 1 + .2, T = 1.
    private static boolean pickProjective() {
        Appearance look = new Appearance();
        Texture2D texture = new Texture2D(new Image2D(Image2D.RGB, 2, 2, new byte[12]));
        Transform projection = new Transform();
        projection.set(new float[] { 1, 0, 1, 0, 0, 1, 0, 0, 0, 0, 1, 0, 1, 0, 0, 1 });
        texture.setTransform(projection);
        look.setTexture(0, texture);
        Mesh mesh = quad(0xffffffff, look);
        VertexArray coordinates = new VertexArray(4, 3, 1);
        coordinates.set(0, 4, new byte[] { 0, 0, 2, 0, 4, 0, 4, 0, 0, 0, 0, 0 });
        mesh.getVertexBuffer().setTexCoords(0, coordinates, 1, null);
        Group root = new Group();
        root.addChild(mesh);
        RayIntersection hit = new RayIntersection();
        return root.pick(-1, -.5f, .5f, 5, 0, 0, -1, hit) &&
            near(hit.getTextureS(0), 1.2f) && near(hit.getTextureT(0), 1);
    }

    // Picking skips the faces the polygon mode culls as seen along the ray. The
    // submesh at x from 1 to 4 faces +z.
    private static boolean pickFacing() {
        Group root = new Group();
        Mesh mesh = pickMesh(0);
        root.addChild(mesh);
        PolygonMode polygon = new PolygonMode();
        mesh.getAppearance(1).setPolygonMode(polygon);
        boolean back = facing(root, 2, true, false);
        polygon.setCulling(PolygonMode.CULL_FRONT);
        boolean front = facing(root, 2, false, true);
        polygon.setCulling(PolygonMode.CULL_NONE);
        boolean none = facing(root, 2, true, true);
        polygon.setCulling(PolygonMode.CULL_BACK);
        polygon.setWinding(PolygonMode.WINDING_CW);
        boolean clockwise = facing(root, 2, false, true);
        polygon.setWinding(PolygonMode.WINDING_CCW);
        // Mirrored, the submesh spans x from -4 to -1 and faces -z.
        mesh.setScale(-1, 1, 1);
        boolean mirrored = facing(root, -2, false, true);
        return back && front && none && clockwise && mirrored;
    }

    // Whether rays at x toward -z and toward +z hit as expected.
    private static boolean facing(Group root, float x, boolean fromFront, boolean fromBack) {
        return root.pick(-1, x, 0, 5, 0, 0, -1, null) == fromFront &&
            root.pick(-1, x, 0, -5, 0, 0, 1, null) == fromBack;
    }

    // A quad at x from -1 to 1 is picked where its morph target or bone has moved it.
    private static boolean pickDeformed() {
        VertexArray base = new VertexArray(4, 3, 1);
        base.set(0, 4, new byte[] { -1, 1, 0, -1, -1, 0, 1, 1, 0, 1, -1, 0 });
        VertexBuffer buffer = new VertexBuffer();
        buffer.setPositions(base, 1, null);
        VertexArray moved = new VertexArray(4, 3, 1);
        moved.set(0, 4, new byte[] { 9, 1, 0, 9, -1, 0, 11, 1, 0, 11, -1, 0 });
        VertexBuffer destination = new VertexBuffer();
        destination.setPositions(moved, 1, null);

        MorphingMesh morph = new MorphingMesh(buffer, new VertexBuffer[] { destination },
                                              strip(), new Appearance());
        Group morphRoot = new Group();
        morphRoot.addChild(morph);
        RayIntersection hit = new RayIntersection();
        boolean atRest = !morphRoot.pick(-1, 5, 0, 3, 0, 0, -1, hit);
        morph.setWeights(new float[] { .5f });
        boolean morphed = morphRoot.pick(-1, 5, 0, 3, 0, 0, -1, hit) &&
            hit.getIntersected() == morph && near(hit.getDistance(), 3);

        Group skeleton = new Group();
        Group bone = new Group();
        skeleton.addChild(bone);
        SkinnedMesh skin = new SkinnedMesh(buffer, strip(), new Appearance(), skeleton);
        skin.addTransform(bone, 1, 0, 4);
        Group skinRoot = new Group();
        skinRoot.addChild(skin);
        boolean skinAtRest = !skinRoot.pick(-1, 5, 0, 3, 0, 0, -1, hit);
        bone.setTranslation(5, 0, 0);
        boolean skinned = skinRoot.pick(-1, 5, 0, 3, 0, 0, -1, hit) &&
            hit.getIntersected() == skin && near(hit.getDistance(), 3);
        return atRest && morphed && skinAtRest && skinned;
    }

    private static boolean pickDeformedNormals() {
        Appearance appearance = new Appearance();
        VertexBuffer buffer = quad(0xffffffff, appearance).getVertexBuffer();
        VertexArray baseNormals = new VertexArray(4, 3, 1);
        baseNormals.set(0, 4, new byte[] { 0, 3, 4, 0, 3, 4, 0, 3, 4, 0, 3, 4 });
        buffer.setNormals(baseNormals);
        VertexArray targetNormals = new VertexArray(4, 3, 1);
        targetNormals.set(0, 4, new byte[] { 4, 3, 0, 4, 3, 0, 4, 3, 0, 4, 3, 0 });
        VertexBuffer target = new VertexBuffer();
        target.setNormals(targetNormals);
        MorphingMesh morph = new MorphingMesh(buffer, new VertexBuffer[] { target },
                                              strip(), appearance);
        morph.setWeights(new float[] { .5f });
        Group morphRoot = new Group();
        morphRoot.addChild(morph);
        RayIntersection hit = new RayIntersection();
        // The midpoint of the two normals is (2, 3, 2), with length sqrt(17).
        boolean morphed = morphRoot.pick(-1, -.5f, .5f, 3, 0, 0, -1, hit) &&
            hit.getIntersected() == morph && near(hit.getNormalX(), .485071f) &&
            near(hit.getNormalY(), .727607f) && near(hit.getNormalZ(), .485071f);

        Group bone = new Group();
        Group skeleton = new Group();
        skeleton.addChild(bone);
        SkinnedMesh skin = new SkinnedMesh(buffer, strip(), appearance, skeleton);
        skin.addTransform(bone, 1, 0, 4);
        bone.setOrientation(90, 0, 1, 0);
        Group skinRoot = new Group();
        skinRoot.addChild(skin);
        // A 90-degree turn about +y rotates (0, 3, 4) to (4, 3, 0), with length 5.
        boolean skinned = skinRoot.pick(-1, 3, .5f, .5f, -1, 0, 0, hit) &&
            hit.getIntersected() == skin && near(hit.getNormalX(), .8f) &&
            near(hit.getNormalY(), .6f) && near(hit.getNormalZ(), 0);
        return morphed && skinned;
    }

    // A camera 5 units above the origin sees the meshes 6 and 8 units away.
    private static boolean pickCamera() {
        Group root = new Group();
        Group scaled = new Group();
        scaled.setScale(2, 2, 2);
        Mesh back = pickMesh(-1.5f), front = pickMesh(-.5f);
        scaled.addChild(back);
        scaled.addChild(front);
        root.addChild(scaled);
        Camera camera = new Camera();
        camera.setPerspective(90, 1, 1, 10);
        camera.setTranslation(0, 0, 5);
        root.addChild(camera);

        // At distance 6 the 90-degree view spans x and y from -6 to 6.
        RayIntersection hit = new RayIntersection();
        boolean found = root.pick(-1, 11f / 12, .25f, camera, hit) && hit.getIntersected() == front &&
            near(hit.getDistance(), 5f / 9) && hit.getSubmeshIndex() == 1 &&
            near(hit.getTextureS(0), .5f) && near(hit.getTextureT(0), .25f);
        float[] ray = new float[6];
        hit.getRay(ray);
        // The ray starts on the near plane, 1 unit in front of the camera, and
        // reaches the far plane, 10 units in front of it, at distance 1.
        found &= near(ray[0], 5f / 6) && near(ray[1], .5f) && near(ray[2], 4) &&
            near(ray[3], 7.5f) && near(ray[4], 4.5f) && near(ray[5], -9);
        found &= root.pick(-1, 11f / 12, .25f, camera, null);

        // The ray goes on past a far plane 5 units from the camera.
        camera.setPerspective(90, 1, 1, 5);
        boolean pastFar = root.pick(-1, 11f / 12, .25f, camera, hit) &&
            hit.getIntersected() == front && near(hit.getDistance(), 1.25f);
        boolean outside = false;
        try {
            root.pick(-1, .5f, .5f, new Camera(), hit);
        } catch (IllegalStateException expected) {
            outside = true;
        }
        return found && pastFar && outside;
    }

    private static boolean stateAccessors(Graphics3D renderer) {
        CompositingMode mode = new CompositingMode();
        boolean compositing = mode.isDepthTestEnabled() && mode.isDepthWriteEnabled() &&
            mode.isColorWriteEnabled() && mode.isAlphaWriteEnabled() &&
            mode.getBlending() == CompositingMode.REPLACE && mode.getAlphaThreshold() == 0 &&
            mode.getDepthOffsetFactor() == 0 && mode.getDepthOffsetUnits() == 0;
        mode.setDepthTestEnable(false);
        mode.setDepthWriteEnable(false);
        mode.setColorWriteEnable(false);
        mode.setAlphaWriteEnable(false);
        mode.setBlending(CompositingMode.MODULATE_X2);
        mode.setAlphaThreshold(.25f);
        mode.setDepthOffset(1.5f, -2);
        compositing &= !mode.isDepthTestEnabled() && !mode.isDepthWriteEnabled() &&
            !mode.isColorWriteEnabled() && !mode.isAlphaWriteEnabled() &&
            mode.getBlending() == CompositingMode.MODULATE_X2 && mode.getAlphaThreshold() == .25f &&
            mode.getDepthOffsetFactor() == 1.5f && mode.getDepthOffsetUnits() == -2;

        PolygonMode polygon = new PolygonMode();
        boolean polygons = polygon.getCulling() == PolygonMode.CULL_BACK &&
            polygon.getShading() == PolygonMode.SHADE_SMOOTH &&
            polygon.getWinding() == PolygonMode.WINDING_CCW &&
            polygon.isPerspectiveCorrectionEnabled() && !polygon.isLocalCameraLightingEnabled() &&
            !polygon.isTwoSidedLightingEnabled();
        polygon.setCulling(PolygonMode.CULL_FRONT);
        polygon.setShading(PolygonMode.SHADE_FLAT);
        polygon.setWinding(PolygonMode.WINDING_CW);
        polygon.setPerspectiveCorrectionEnable(false);
        polygon.setLocalCameraLightingEnable(true);
        polygon.setTwoSidedLightingEnable(true);
        polygons &= polygon.getCulling() == PolygonMode.CULL_FRONT &&
            polygon.getShading() == PolygonMode.SHADE_FLAT &&
            polygon.getWinding() == PolygonMode.WINDING_CW &&
            !polygon.isPerspectiveCorrectionEnabled() && polygon.isLocalCameraLightingEnabled() &&
            polygon.isTwoSidedLightingEnabled();

        Background background = new Background();
        boolean backgrounds = background.getColor() == 0 && background.isColorClearEnabled() &&
            background.isDepthClearEnabled() && background.getImageModeX() == Background.BORDER &&
            background.getImageModeY() == Background.BORDER;
        background.setColor(0x80102030);
        background.setCrop(1, 2, 3, 4);
        background.setImageMode(Background.REPEAT, Background.BORDER);
        background.setColorClearEnable(false);
        background.setDepthClearEnable(false);
        backgrounds &= background.getColor() == 0x80102030 && background.getCropX() == 1 &&
            background.getCropY() == 2 && background.getCropWidth() == 3 &&
            background.getCropHeight() == 4 && background.getImageModeX() == Background.REPEAT &&
            background.getImageModeY() == Background.BORDER && !background.isColorClearEnabled() &&
            !background.isDepthClearEnabled();

        Texture2D texture = new Texture2D(new Image2D(Image2D.RGB, 1, 1, new byte[3]));
        boolean textures = texture.getBlending() == Texture2D.FUNC_MODULATE &&
            texture.getWrappingS() == Texture2D.WRAP_REPEAT &&
            texture.getWrappingT() == Texture2D.WRAP_REPEAT && texture.getBlendColor() == 0;
        texture.setBlending(Texture2D.FUNC_BLEND);
        texture.setWrapping(Texture2D.WRAP_CLAMP, Texture2D.WRAP_REPEAT);
        texture.setBlendColor(0x12345678);
        textures &= texture.getBlending() == Texture2D.FUNC_BLEND &&
            texture.getWrappingS() == Texture2D.WRAP_CLAMP &&
            texture.getWrappingT() == Texture2D.WRAP_REPEAT && texture.getBlendColor() == 0x345678;

        Appearance look = new Appearance();
        boolean layers = look.getLayer() == 0;
        look.setLayer(-63);
        layers &= look.getLayer() == -63;
        look.setLayer(64);
        layers &= look.getLayer() == 64;
        try {
            look.setLayer(65);
            layers = false;
        } catch (IllegalArgumentException expected) {}
        try {
            look.setLayer(-64);
            layers = false;
        } catch (IllegalArgumentException expected) {}
        layers &= look.getLayer() == 64;

        Material material = new Material();
        boolean materials = !material.isVertexColorTrackingEnabled();
        material.setVertexColorTrackingEnable(true);
        materials &= material.isVertexColorTrackingEnabled();

        Light light = new Light();
        boolean lights = light.getMode() == Light.DIRECTIONAL && light.getColor() == 0xffffff &&
            light.getIntensity() == 1 && light.getConstantAttenuation() == 1 &&
            light.getLinearAttenuation() == 0 && light.getQuadraticAttenuation() == 0 &&
            light.getSpotAngle() == 45 && light.getSpotExponent() == 0;
        light.setMode(Light.SPOT);
        light.setColor(0xff102030);
        light.setIntensity(2.5f);
        light.setAttenuation(1, .5f, .25f);
        light.setSpotAngle(30);
        light.setSpotExponent(5);
        Light copy = (Light) light.duplicate();
        lights &= light.getMode() == Light.SPOT && light.getColor() == 0x102030 &&
            light.getIntensity() == 2.5f && light.getConstantAttenuation() == 1 &&
            light.getLinearAttenuation() == .5f && light.getQuadraticAttenuation() == .25f &&
            light.getSpotAngle() == 30 && light.getSpotExponent() == 5 &&
            copy.getColor() == 0x102030 && copy.getQuadraticAttenuation() == .25f &&
            copy.getSpotAngle() == 30;
        for (int bad = 0; bad < 5; bad++) {
            try {
                if (bad == 0) light.setSpotAngle(91);
                else if (bad == 1) light.setSpotAngle(-1);
                else if (bad == 2) light.setSpotExponent(129);
                else if (bad == 3) light.setAttenuation(0, 0, 0);
                else light.setAttenuation(1, -1, 1);
                lights = false;
            } catch (IllegalArgumentException expected) {}
        }
        lights &= light.getSpotAngle() == 30 && light.getLinearAttenuation() == .5f;

        KeyframeSequence frames = new KeyframeSequence(3, 2, KeyframeSequence.LINEAR);
        frames.setKeyframe(1, 50, new float[] { 1, 2 });
        frames.setRepeatMode(KeyframeSequence.LOOP);
        frames.setValidRange(1, 2);
        float[] value = new float[2];
        boolean sequences = frames.getKeyframeCount() == 3 && frames.getComponentCount() == 2 &&
            frames.getInterpolationType() == KeyframeSequence.LINEAR &&
            frames.getKeyframe(1, value) == 50 && value[0] == 1 && value[1] == 2 &&
            frames.getKeyframe(1, null) == 50 &&
            frames.getRepeatMode() == KeyframeSequence.LOOP &&
            frames.getValidRangeFirst() == 1 && frames.getValidRangeLast() == 2;

        AnimationController controller = new AnimationController();
        controller.setPosition(2, 100);
        boolean controllers = controller.getRefWorldTime() == 100;
        controller.setSpeed(2, 150);
        controllers &= controller.getRefWorldTime() == 150;

        Camera camera = new Camera();
        float[] projection = { 9, 9, 9, 9 };
        boolean cameras = camera.getProjection(projection) == Camera.GENERIC &&
            projection[0] == 9 && projection[3] == 9;
        camera.setPerspective(60, 1.5f, .5f, 100);
        cameras &= camera.getProjection(projection) == Camera.PERSPECTIVE &&
            projection[0] == 60 && projection[1] == 1.5f && projection[2] == .5f &&
            projection[3] == 100;
        camera.setParallel(4, 2, 1, 8);
        cameras &= camera.getProjection(projection) == Camera.PARALLEL &&
            projection[0] == 4 && projection[1] == 2 && projection[2] == 1 && projection[3] == 8;
        try {
            camera.getProjection(new float[3]);
            cameras = false;
        } catch (IllegalArgumentException expected) {}

        return compositing && polygons && backgrounds && textures && layers && materials &&
            lights && sequences && controllers && cameras && rendererAccessors(renderer);
    }

    private static boolean rendererAccessors(Graphics3D renderer) {
        Image target = DirectUtils.createImage(10, 10, 0);
        Graphics graphics = target.getGraphics();
        boolean unbound = renderer.getTarget() == null && !renderer.isDepthBufferEnabled() &&
            renderer.getHints() == 0;
        renderer.bindTarget(graphics, false, Graphics3D.TRUE_COLOR);
        try {
            boolean bound = renderer.getTarget() == graphics && !renderer.isDepthBufferEnabled() &&
                renderer.getHints() == Graphics3D.TRUE_COLOR &&
                renderer.getViewportX() == 0 && renderer.getViewportY() == 0 &&
                renderer.getViewportWidth() == 10 && renderer.getViewportHeight() == 10 &&
                renderer.getDepthRangeNear() == 0 && renderer.getDepthRangeFar() == 1;
            renderer.setViewport(1, 2, 3, 4);
            renderer.setDepthRange(.25f, .75f);
            bound &= renderer.getViewportX() == 1 && renderer.getViewportY() == 2 &&
                renderer.getViewportWidth() == 3 && renderer.getViewportHeight() == 4 &&
                renderer.getDepthRangeNear() == .25f && renderer.getDepthRangeFar() == .75f;
            try {
                renderer.setDepthRange(1.5f, 1);
                bound = false;
            } catch (IllegalArgumentException expected) {}
            bound &= renderer.getDepthRangeFar() == .75f;
            Hashtable properties = Graphics3D.getProperties();
            Object lights = properties.get("maxLights");
            Object units = properties.get("numTextureUnits");
            Object dithering = properties.get("supportDithering");
            return unbound && bound && lights instanceof Integer &&
                ((Integer) lights).intValue() >= 1 && units instanceof Integer &&
                ((Integer) units).intValue() >= 1 && dithering instanceof Boolean;
        } finally {
            renderer.releaseTarget();
            renderer.setDepthRange(0, 1);
        }
    }

    private static boolean nodeAccessors() {
        Group node = new Group();
        node.setOrientation(90, 0, 0, 1);
        node.preRotate(90, 1, 0, 0);
        Transform composite = new Transform();
        node.getCompositeTransform(composite);
        // Turning about z first and about x second takes the x axis to the z axis.
        float[] axis = { 1, 0, 0, 0 };
        composite.transform(axis);
        boolean preRotated = near(axis[0], 0) && near(axis[1], 0) && near(axis[2], 1);

        Transform matrix = new Transform();
        matrix.set(new float[] { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 });
        matrix.transpose();
        float[] values = new float[16];
        matrix.get(values);
        float[] transposed = { 1, 5, 9, 13, 2, 6, 10, 14, 3, 7, 11, 15, 4, 8, 12, 16 };
        boolean transpose = true;
        for (int i = 0; i < 16; i++) transpose &= values[i] == transposed[i];

        Group root = new Group();
        Group a = new Group(), b = new Group();
        root.addChild(a);
        root.addChild(b);
        a.setTranslation(1, 2, 3);
        b.setTranslation(10, 0, 0);
        b.setScale(2, 2, 2);
        Transform relative = new Transform();
        float[] origin = { 0, 0, 0, 1 };
        boolean related = a.getTransformTo(b, relative);
        relative.transform(origin);
        related &= near(origin[0], -4.5f) && near(origin[1], 1) && near(origin[2], 1.5f);
        Transform untouched = new Transform();
        boolean unrelated = !a.getTransformTo(new Group(), untouched);
        untouched.get(values);
        unrelated &= values[0] == 1 && values[3] == 0;
        b.setScale(0, 1, 1);
        unrelated &= !a.getTransformTo(b, untouched);

        Camera camera = new Camera();
        camera.setScope(6);
        camera.setAlphaFactor(.5f);
        boolean factors = camera.getScope() == 6 && camera.getAlphaFactor() == .5f &&
            ((Camera) camera.duplicate()).getScope() == 6;
        try {
            camera.setAlphaFactor(1.5f);
            factors = false;
        } catch (IllegalArgumentException expected) {}
        Mesh mesh = quad(0, new Appearance());
        mesh.setScope(8);
        factors &= mesh.getScope() == 8;
        Sprite3D sprite = new Sprite3D(true, new Image2D(Image2D.RGB, 1, 1, new byte[3]), null);
        Group zReference = new Group();
        sprite.setAlignment(zReference, Node.Z_AXIS, null, Node.ORIGIN);
        boolean alignment = sprite.getAlignmentReference(Node.Z_AXIS) == zReference &&
            sprite.getAlignmentTarget(Node.Z_AXIS) == Node.Z_AXIS &&
            sprite.getAlignmentReference(Node.Y_AXIS) == null &&
            sprite.getAlignmentTarget(Node.Y_AXIS) == Node.ORIGIN;
        try {
            sprite.getAlignmentTarget(Node.X_AXIS);
            alignment = false;
        } catch (IllegalArgumentException expected) {}
        try {
            sprite.setAlignment(sprite, Node.Z_AXIS, null, Node.NONE);
            alignment = false;
        } catch (IllegalArgumentException expected) {}

        Object tag = new Object();
        mesh.setUserObject(tag);
        Mesh duplicate = (Mesh) mesh.duplicate();
        Object3D[] references = new Object3D[3];
        boolean objects = mesh.getUserObject() == tag && duplicate.getUserObject() == tag &&
            mesh.getReferences(null) == 3 && mesh.getReferences(references) == 3 &&
            references[0] == mesh.getVertexBuffer() && references[1] == mesh.getIndexBuffer(0) &&
            references[2] == mesh.getAppearance(0);
        try {
            mesh.getReferences(new Object3D[2]);
            objects = false;
        } catch (IllegalArgumentException expected) {}
        return preRotated && transpose && related && unrelated && factors && alignment && objects;
    }

    private static boolean vertexAccessors() {
        VertexArray positions = new VertexArray(2, 3, 1);
        positions.set(0, 2, new byte[] { 1, -2, 3, -128, 127, 0 });
        VertexArray wide = new VertexArray(2, 2, 2);
        wide.set(0, 2, new short[] { 300, -300, 32767, -32768 });
        byte[] bytes = new byte[6];
        short[] shorts = new short[4];
        positions.get(0, 2, bytes);
        wide.get(0, 2, shorts);
        boolean arrays = bytes[0] == 1 && bytes[1] == -2 && bytes[3] == -128 && bytes[4] == 127 &&
            shorts[0] == 300 && shorts[1] == -300 && shorts[2] == 32767 && shorts[3] == -32768;
        byte[] second = new byte[3];
        positions.get(1, 1, second);
        arrays &= second[0] == -128 && second[1] == 127 && second[2] == 0;
        try {
            positions.get(0, 2, shorts);
            arrays = false;
        } catch (IllegalStateException expected) {}
        try {
            wide.get(0, 2, new short[3]);
            arrays = false;
        } catch (IllegalArgumentException expected) {}
        try {
            positions.get(1, 2, bytes);
            arrays = false;
        } catch (IndexOutOfBoundsException expected) {}

        VertexBuffer buffer = new VertexBuffer();
        VertexArray coordinates = new VertexArray(2, 2, 2);
        buffer.setPositions(positions, .5f, new float[] { 1, 2, 3 });
        buffer.setTexCoords(1, coordinates, 2, new float[] { 4, 5 });
        buffer.setDefaultColor(0x80112233);
        float[] scaleBias = new float[4];
        boolean buffers = buffer.getPositions(scaleBias) == positions && scaleBias[0] == .5f &&
            scaleBias[1] == 1 && scaleBias[2] == 2 && scaleBias[3] == 3 &&
            buffer.getTexCoords(1, scaleBias) == coordinates && scaleBias[0] == 2 &&
            scaleBias[1] == 4 && scaleBias[2] == 5 && buffer.getTexCoords(0, null) == null &&
            buffer.getDefaultColor() == 0x80112233 && buffer.getColors() == null &&
            buffer.getNormals() == null;
        try {
            buffer.getPositions(new float[3]);
            buffers = false;
        } catch (IllegalArgumentException expected) {}
        try {
            buffer.setColors(wide);
            buffers = false;
        } catch (IllegalArgumentException expected) {}
        try {
            buffer.setColors(new VertexArray(2, 2, 1));
            buffers = false;
        } catch (IllegalArgumentException expected) {}
        try {
            buffer.setColors(new VertexArray(3, 3, 1));
            buffers = false;
        } catch (IllegalArgumentException expected) {}
        VertexArray colors = new VertexArray(2, 4, 1);
        buffer.setColors(colors);
        buffer.setColors(new VertexArray(2, 3, 1));
        buffer.setColors(null);
        buffers &= buffer.getColors() == null;

        MorphingMesh morph = new MorphingMesh(buffer, new VertexBuffer[] { buffer },
            new TriangleStripArray(0, new int[] { 3 }), new Appearance());
        SkinnedMesh skin = new SkinnedMesh(buffer, new TriangleStripArray(0, new int[] { 3 }),
            new Appearance(), new Group());
        // The inherited accessor must resolve through the subclasses.
        boolean meshes = morph.getIndexBuffer(0) == ((Mesh) morph).getIndexBuffer(0) &&
            skin.getIndexBuffer(0) instanceof TriangleStripArray;
        return arrays && buffers && meshes;
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

    // The depth range places the depths of what is drawn: the blue quad, drawn
    // into the front half, stays in front of the nearer green quad drawn into
    // the back half.
    private static boolean depthRange(Graphics3D renderer, Graphics graphics, Image target,
                                      int[] pixels) {
        Appearance look = new Appearance();
        TriangleStripArray indices = new TriangleStripArray(0, new int[] { 4 });
        Transform near = new Transform();
        near.postTranslate(0, 0, -.5f);
        renderer.bindTarget(graphics, true, 0);
        try {
            renderer.clear(null);
            renderer.setDepthRange(0, .5f);
            renderer.render(quad(0xff0000ff, look).getVertexBuffer(), indices, look, null);
            renderer.setDepthRange(.5f, 1);
            renderer.render(quad(0xff00ff00, look).getVertexBuffer(), indices, look, near);
        } finally {
            renderer.setDepthRange(0, 1);
            renderer.releaseTarget();
        }
        target.getRGB(pixels, 0, 10, 0, 0, 10, 10);
        boolean halves = pixels[45] == 0xff0000ff && pixels[42] == 0xff0000ff;

        // An empty range at the far end puts every depth at the cleared value,
        // which passes the test: the quad covers the whole 12 by 12 viewport.
        // Summing the corner weights of these pixels rounds above 1.
        Image full = DirectUtils.createImage(12, 12, 0);
        renderer.bindTarget(full.getGraphics());
        try {
            renderer.clear(null);
            renderer.setDepthRange(1, 1);
            renderer.render(quad(0xff0000ff, look).getVertexBuffer(), indices, look, null);
        } finally {
            renderer.setDepthRange(0, 1);
            renderer.releaseTarget();
        }
        int[] covered = new int[144];
        full.getRGB(covered, 0, 12, 0, 0, 12, 12);
        boolean empty = true;
        for (int i = 0; i < covered.length; i++) {
            empty &= covered[i] == 0xff0000ff;
        }
        return halves && empty;
    }

    // The target keeps its pixels where a frame does not draw: without any
    // drawing, after a depth-only clear and around a cleared viewport. Each
    // frame starts from a color the renderer has not held before.
    private static boolean targetReads(Graphics3D renderer) {
        Image target = DirectUtils.createImage(10, 10, 0xffff00ff);
        Graphics graphics = target.getGraphics();
        int[] pixels = new int[100];
        renderer.bindTarget(graphics, true, 0);
        renderer.releaseTarget();
        target.getRGB(pixels, 0, 10, 0, 0, 10, 10);
        boolean untouched = pixels[52] == 0xffff00ff && pixels[57] == 0xffff00ff;

        graphics.setColor(0x00ffff);
        graphics.fillRect(0, 0, 10, 10);
        Background depthOnly = new Background();
        depthOnly.setColorClearEnable(false);
        renderer.bindTarget(graphics, true, 0);
        try {
            renderer.clear(depthOnly);
        } finally {
            renderer.releaseTarget();
        }
        target.getRGB(pixels, 0, 10, 0, 0, 10, 10);
        boolean depth = pixels[52] == 0xff00ffff && pixels[57] == 0xff00ffff;

        graphics.setColor(0xffff00);
        graphics.fillRect(0, 0, 10, 10);
        Background green = new Background();
        green.setColor(0xff00ff00);
        renderer.bindTarget(graphics, true, 0);
        try {
            renderer.setViewport(0, 0, 5, 10);
            renderer.clear(green);
        } finally {
            renderer.releaseTarget();
        }
        target.getRGB(pixels, 0, 10, 0, 0, 10, 10);
        boolean viewport = pixels[52] == 0xff00ff00 && pixels[57] == 0xffffff00;
        return untouched && depth && viewport;
    }

    // Depth clears after drawing take effect, also over a part the previous
    // clear did not cover; a clear right after another one may be skipped.
    private static boolean depthClears(Graphics3D renderer) {
        Image target = DirectUtils.createImage(10, 10, 0xff000000);
        Graphics graphics = target.getGraphics();
        Appearance look = new Appearance();
        TriangleStripArray indices = new TriangleStripArray(0, new int[] { 4 });
        VertexBuffer green = quad(0xff00ff00, look).getVertexBuffer();
        VertexBuffer blue = quad(0xff0000ff, look).getVertexBuffer();
        Transform near = new Transform();
        near.postTranslate(0, 0, -.5f);
        Background depthOnly = new Background();
        depthOnly.setColorClearEnable(false);
        int[] pixels = new int[100];

        renderer.bindTarget(graphics, true, 0);
        try {
            renderer.setCamera(new Camera(), null);
            renderer.clear(null);
            renderer.render(green, indices, look, near);
            renderer.clear(depthOnly);
            renderer.clear(depthOnly);
            renderer.render(blue, indices, look, null);
        } finally {
            renderer.releaseTarget();
        }
        target.getRGB(pixels, 0, 10, 0, 0, 10, 10);
        boolean repeated = pixels[55] == 0xff0000ff;

        renderer.bindTarget(graphics, true, 0);
        try {
            renderer.setCamera(new Camera(), null);
            renderer.render(green, indices, look, near);
            renderer.setViewport(0, 0, 5, 10);
            renderer.clear(depthOnly);
            renderer.setViewport(0, 0, 10, 10);
            renderer.clear(depthOnly);
            renderer.render(blue, indices, look, null);
        } finally {
            renderer.releaseTarget();
        }
        target.getRGB(pixels, 0, 10, 0, 0, 10, 10);
        return repeated && pixels[52] == 0xff0000ff && pixels[57] == 0xff0000ff;
    }

    // Large triangles, whose rows are long: shaded, textured, depth tested and
    // blended, partly clipped. The hash is that of the pixels before the
    // renderer left out the tests at the ends of long rows.
    private static final int HASH = 814828724;

    private static boolean wideRows(Graphics3D renderer) {
        Image target = DirectUtils.createImage(96, 80, 0xff102030);
        Appearance plain = new Appearance();
        Mesh shaded = quad(0xffffffff, plain);
        VertexArray colors = new VertexArray(4, 3, 1);
        colors.set(0, 4, new byte[] { -1, 0, 0, 0, -1, 0, 0, 0, -1, -1, -1, 0 });
        shaded.getVertexBuffer().setColors(colors);

        byte[] texels = new byte[8 * 8 * 4];
        for (int y = 0; y < 8; y++) {
            for (int x = 0; x < 8; x++) {
                int i = (y * 8 + x) * 4;
                texels[i] = (byte) (x * 32);
                texels[i + 1] = (byte) (y * 32);
                texels[i + 2] = (byte) ((x ^ y) * 30);
                texels[i + 3] = (byte) (((x + y) & 3) == 0 ? 0 : (x == 3 ? 100 : 255));
            }
        }
        Appearance modulated = new Appearance();
        Texture2D texture = new Texture2D(new Image2D(Image2D.RGBA, 8, 8, texels));
        texture.setBlending(Texture2D.FUNC_MODULATE);
        modulated.setTexture(0, texture);
        Mesh textured = quad(0xff80c0ff, modulated);
        VertexArray coordinates = new VertexArray(4, 2, 1);
        coordinates.set(0, 4, new byte[] { 0, 0, 0, 1, 1, 0, 1, 1 });
        textured.getVertexBuffer().setTexCoords(0, coordinates, 3, null);

        Appearance replacing = new Appearance();
        Texture2D copy = new Texture2D(new Image2D(Image2D.RGB, 8, 8, texels2(texels)));
        copy.setBlending(Texture2D.FUNC_REPLACE);
        replacing.setTexture(0, copy);
        CompositingMode over = new CompositingMode();
        over.setBlending(CompositingMode.ALPHA);
        over.setDepthWriteEnable(false);
        replacing.setCompositingMode(over);
        Mesh opaque = quad(0x80ffffff, replacing);
        opaque.getVertexBuffer().setTexCoords(0, coordinates, 2, null);

        Transform first = new Transform();
        first.postRotate(20, 0, 0, 1);
        first.postScale(.9f, .8f, 1);
        Transform second = new Transform();
        second.postTranslate(.2f, -.1f, -.2f);
        second.postRotate(-35, 0, 0, 1);
        second.postScale(1.3f, .5f, 1);
        Transform third = new Transform();
        third.postTranslate(-.1f, .2f, -.4f);
        third.postRotate(70, 0, 0, 1);
        third.postScale(1.1f, .6f, 1);
        renderer.bindTarget(target.getGraphics());
        try {
            renderer.clear(null);
            renderer.setCamera(new Camera(), null);
            renderer.render(shaded, first);
            renderer.render(textured, second);
            renderer.render(opaque, third);
        } finally {
            renderer.releaseTarget();
        }
        int[] pixels = new int[96 * 80];
        target.getRGB(pixels, 0, 96, 0, 0, 96, 80);
        int hash = 0;
        for (int i = 0; i < pixels.length; i++) {
            hash = hash * 31 + pixels[i];
        }
        return HASH == hash;
    }

    // The RGB bytes of RGBA texels.
    private static byte[] texels2(byte[] rgba) {
        byte[] rgb = new byte[rgba.length / 4 * 3];
        for (int i = 0; i < rgba.length / 4; i++) {
            rgb[i * 3] = rgba[i * 4 + 2];
            rgb[i * 3 + 1] = rgba[i * 4];
            rgb[i * 3 + 2] = rgba[i * 4 + 1];
        }
        return rgb;
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

    // Transform multiplication and inversion give the same bits as the plain
    // Java float arithmetic below, done in the same order.
    private static boolean transformMath() {
        Random random = new Random(20261121);
        float[] got = new float[16];
        Transform left = new Transform(), right = new Transform();
        for (int round = 0; round < 500; round++) {
            float[] a = randomMatrix(random, round), b = randomMatrix(random, round + 1);
            left.set(a);
            right.set(b);
            left.postMultiply(right);
            left.get(got);
            if (!sameBits(got, product(a, b))) return false;
            left.set(a);
            left.postMultiply(left);
            left.get(got);
            if (!sameBits(got, product(a, a))) return false;
            float[] expected = inverse(a);
            left.set(a);
            try {
                left.invert();
                left.get(got);
                if (expected == null || !sameBits(got, expected)) return false;
            } catch (ArithmeticException singular) {
                left.get(got);
                if (expected != null || !sameBits(got, a)) return false;
            }
        }
        new Transform().get(got);
        for (int i = 0; i < 16; i++) {
            if (got[i] != (i % 5 == 0 ? 1 : 0)) return false;
        }
        return true;
    }

    // Every seventh matrix repeats a row and is singular.
    private static float[] randomMatrix(Random random, int round) {
        float[] m = new float[16];
        for (int i = 0; i < 16; i++) {
            m[i] = (random.nextInt(20001) - 10000) / 97f;
        }
        if (round % 7 == 0) {
            System.arraycopy(m, 0, m, 8, 4);
        }
        return m;
    }

    private static float[] product(float[] a, float[] b) {
        float[] result = new float[16];
        for (int i = 0; i < 16; i += 4) {
            for (int j = 0; j < 4; j++) {
                result[i + j] = a[i] * b[j] + a[i + 1] * b[4 + j] + a[i + 2] * b[8 + j] +
                    a[i + 3] * b[12 + j];
            }
        }
        return result;
    }

    // Gauss-Jordan elimination with partial pivoting; null when singular.
    private static float[] inverse(float[] matrix) {
        float[][] rows = new float[4][8];
        for (int i = 0; i < 4; i++) {
            System.arraycopy(matrix, i * 4, rows[i], 0, 4);
            rows[i][i + 4] = 1;
        }
        for (int column = 0; column < 4; column++) {
            int pivot = column;
            for (int candidate = column + 1; candidate < 4; candidate++) {
                if (Math.abs(rows[candidate][column]) > Math.abs(rows[pivot][column])) {
                    pivot = candidate;
                }
            }
            float[] basis = rows[pivot];
            if (basis[column] == 0) return null;
            rows[pivot] = rows[column];
            rows[column] = basis;
            float divisor = basis[column];
            for (int k = column; k < 8; k++) basis[k] /= divisor;
            for (int row = 0; row < 4; row++) {
                if (row == column) continue;
                float coefficient = rows[row][column];
                for (int k = column; k < 8; k++) rows[row][k] -= basis[k] * coefficient;
            }
        }
        float[] result = new float[16];
        for (int i = 0; i < 4; i++) {
            System.arraycopy(rows[i], 4, result, 4 * i, 4);
        }
        return result;
    }

    private static boolean sameBits(float[] a, float[] b) {
        for (int i = 0; i < 16; i++) {
            if (Float.floatToIntBits(a[i]) != Float.floatToIntBits(b[i])) return false;
        }
        return true;
    }

    // Submeshes that use separate parts of one lit vertex buffer render like
    // meshes with their own buffers, with native and with Java lighting (fog).
    private static boolean submeshRanges(Graphics3D renderer) {
        byte[] corners = {
            -9, 9, 0, -9, -9, 0, -1, 9, 0, -1, -9, 0,
            1, 9, 0, 1, -9, 0, 9, 9, 0, 9, -9, 0,
        };
        // Different angles to the light in the two halves.
        short[] directions = {
            0, 0, 4, 1, 0, 4, 0, 1, 4, 1, 1, 4,
            4, 0, 2, 0, 4, 1, 4, 4, 1, 3, 0, 1,
        };
        Appearance[] looks = { new Appearance(), new Appearance() };
        int[] diffuse = { 0xffc04020, 0xff2050c0 };
        for (int i = 0; i < 2; i++) {
            Material material = new Material();
            material.setColor(Material.DIFFUSE, diffuse[i]);
            material.setColor(Material.AMBIENT, 0x203040);
            looks[i].setMaterial(material);
        }
        Light ambient = new Light();
        ambient.setMode(Light.AMBIENT);
        ambient.setIntensity(.5f);
        Light directional = new Light();
        directional.setIntensity(.75f);
        Image target = DirectUtils.createImage(10, 10, 0);
        int[] shared = new int[100];
        int[] separate = new int[100];
        try {
            renderer.resetLights();
            renderer.addLight(ambient, null);
            renderer.addLight(directional, null);
            for (int pass = 0; pass < 2; pass++) {
                if (pass == 1) {
                    looks[1].setFog(new Fog());
                }
                VertexBuffer buffer = litBuffer(corners, directions, 0, 8);
                Mesh mesh = new Mesh(buffer, new IndexBuffer[] {
                    new TriangleStripArray(0, new int[] { 4 }),
                    new TriangleStripArray(4, new int[] { 4 }),
                }, looks);
                renderNode(renderer, target, mesh, shared);
                Group group = new Group();
                for (int i = 0; i < 2; i++) {
                    group.addChild(new Mesh(litBuffer(corners, directions, 4 * i, 4),
                        new TriangleStripArray(0, new int[] { 4 }), looks[i]));
                }
                renderNode(renderer, target, group, separate);
                if (shared[42] == 0 || shared[47] == 0 || shared[42] == shared[47]) {
                    return false;
                }
                for (int i = 0; i < shared.length; i++) {
                    if (shared[i] != separate[i]) {
                        System.err.println("M3G submesh range pass " + pass + ", pixel " + i +
                            " shared 0x" + Integer.toHexString(shared[i]) +
                            ", separate 0x" + Integer.toHexString(separate[i]));
                        return false;
                    }
                }
            }
            return true;
        } finally {
            renderer.resetLights();
        }
    }

    private static VertexBuffer litBuffer(byte[] corners, short[] directions, int first,
                                          int count) {
        VertexArray positions = new VertexArray(count, 3, 1);
        byte[] corner = new byte[count * 3];
        System.arraycopy(corners, first * 3, corner, 0, corner.length);
        positions.set(0, count, corner);
        VertexArray normals = new VertexArray(count, 3, 2);
        short[] direction = new short[count * 3];
        System.arraycopy(directions, first * 3, direction, 0, direction.length);
        normals.set(0, count, direction);
        VertexBuffer buffer = new VertexBuffer();
        buffer.setPositions(positions, .1f, null);
        buffer.setNormals(normals);
        buffer.setDefaultColor(0xffffffff);
        return buffer;
    }

    private static void renderNode(Graphics3D renderer, Image target, Node node, int[] pixels) {
        renderer.bindTarget(target.getGraphics());
        try {
            renderer.clear(null);
            renderer.setCamera(new Camera(), null);
            renderer.render(node, null);
        } finally {
            renderer.releaseTarget();
        }
        target.getRGB(pixels, 0, 10, 0, 0, 10, 10);
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
