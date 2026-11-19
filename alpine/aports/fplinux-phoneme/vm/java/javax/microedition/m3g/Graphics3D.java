// SPDX-License-Identifier: GPL-2.0-only
package javax.microedition.m3g;

import java.util.Vector;
import javax.microedition.lcdui.FPLinuxNokiaImage;
import javax.microedition.lcdui.Graphics;

public final class Graphics3D {
    public static final int ANTIALIAS = 2;
    public static final int DITHER = 4;
    public static final int TRUE_COLOR = 8;
    public static final int OVERWRITE = 16;

    private static final Graphics3D INSTANCE = new Graphics3D();

    private final Vector lights = new Vector();
    private final Transform view = new Transform();
    private final float[] eye = new float[4];
    private final float[] towardEye = new float[4];
    private final Rasterizer raster = new Rasterizer();
    private Graphics target;
    private int originX, originY, clipX, clipY, width, height;
    private boolean hasAlpha;
    private Camera camera;

    private Graphics3D() {}

    public static Graphics3D getInstance() {
        return INSTANCE;
    }

    public void bindTarget(Object value) {
        bindTarget(value, true, 0);
    }

    public void bindTarget(Object value, boolean depthBuffer, int hints) {
        if (target != null) throw new IllegalStateException("target already bound");
        if (value == null) throw new NullPointerException();
        if (!(value instanceof Graphics)) throw new IllegalArgumentException("Graphics target");
        // Rendering hints are advisory. The color buffer still preserves the target.
        if ((hints & ~(ANTIALIAS | DITHER | TRUE_COLOR | OVERWRITE)) != 0) {
            throw new IllegalArgumentException("rendering hints");
        }
        Graphics graphics = (Graphics) value;
        originX = graphics.getTranslateX();
        originY = graphics.getTranslateY();
        clipX = graphics.getClipX() + originX;
        clipY = graphics.getClipY() + originY;
        width = graphics.getClipWidth();
        height = graphics.getClipHeight();
        hasAlpha = FPLinuxNokiaImage.hasAlpha(graphics);
        raster.bind(width, height, depthBuffer);
        raster.viewport(0, 0, width, height);
        if (width != 0 && height != 0) {
            FPLinuxNokiaImage.read(graphics, raster.pixels, 0, width,
                clipX - originX, clipY - originY, width, height);
            if (depthBuffer) raster.resetDepth();
        }
        target = graphics;
    }

    public void releaseTarget() {
        if (target == null) return;
        try {
            if (width != 0 && height != 0) {
                FPLinuxNokiaImage.writeComposited(target, raster.pixels, 0,
                    width, clipX, clipY, width, height);
            }
        } finally {
            target = null;
        }
    }

    public void setViewport(int x, int y, int width, int height) {
        if (width <= 0 || height <= 0) throw new IllegalArgumentException("viewport");
        raster.viewport(position((long) originX + x - clipX),
                        position((long) originY + y - clipY), width, height);
    }

    private static int position(long coordinate) {
        return (int) Math.max(Integer.MIN_VALUE, Math.min(Integer.MAX_VALUE, coordinate));
    }

    public void setCamera(Camera value, Transform transform) {
        Transform pose = transform == null ? new Transform() : transform;
        Transform inverse = new Transform(pose);
        inverse.invert();
        pose.apply(0, 0, 0, 1, eye);
        pose.apply(0, 0, 1, 0, towardEye);
        normalize(towardEye);
        view.set(inverse);
        camera = value;
    }

    public int addLight(Light value, Transform transform) {
        if (value == null) throw new NullPointerException();
        lights.addElement(new SceneLight(value, transform == null ? new Transform() : transform));
        return lights.size() - 1;
    }

    public void resetLights() {
        lights.removeAllElements();
    }

    public void clear(Background background) {
        requireTarget();
        raster.clear(background, hasAlpha);
    }

    public void render(World world) {
        requireTarget();
        Camera active = world.getActiveCamera();
        if (active == null) throw new IllegalStateException("world has no camera");
        // Some authored scenes keep the selected camera as a separate root.
        setCamera(active, active.parent == null ? place(active, new Transform()) :
                  relativeTo(active, world));
        resetLights();
        collectLights(world, new Transform());
        clear(world.getBackground());
        drawScene(world, new Transform());
    }

    public void render(Node node, Transform transform) {
        requireCamera();
        if (node == null) throw new NullPointerException();
        drawScene(node, transform == null ? new Transform() : transform);
    }

    public void render(VertexBuffer vertices, IndexBuffer indices, Appearance appearance,
                       Transform transform) {
        render(vertices, indices, appearance, transform, -1);
    }

    public void render(VertexBuffer vertices, IndexBuffer indices, Appearance appearance,
                       Transform transform, int scope) {
        requireCamera();
        if (vertices == null || indices == null || appearance == null) throw new NullPointerException();
        if ((camera.scope & scope) == 0) return;
        draw(vertices, indices, appearance, transform == null ? new Transform() : transform,
             viewProjection(), 1, scope, null);
    }

    private void requireTarget() {
        if (target == null) throw new IllegalStateException("target unavailable");
    }

    private void requireCamera() {
        requireTarget();
        if (camera == null) throw new IllegalStateException("camera unavailable");
    }

    private Transform viewProjection() {
        Transform result = new Transform(camera.projection);
        result.postMultiply(view);
        return result;
    }

    private void draw(VertexBuffer vertices, IndexBuffer indices, Appearance look,
                      Transform model, Transform projection, float alpha, int scope,
                      Mesh mesh) {
        Transform screen = new Transform(projection);
        screen.postMultiply(model);
        raster.draw(vertices, (TriangleStripArray) indices, look, screen, model, view,
                    alpha, lights, scope, eye, towardEye, mesh);
    }

    private static Transform place(Node node, Transform parent) {
        Transform local = new Transform();
        node.getCompositeTransform(local);
        Transform result = new Transform(parent);
        result.postMultiply(local);
        return result;
    }

    private static Transform relativeTo(Node node, Node root) {
        if (node == root) return new Transform();
        if (node == null) throw new IllegalStateException("camera outside world");
        return place(node, relativeTo(node.parent, root));
    }

    private void collectLights(Node node, Transform transform) {
        if (!node.rendering) return;
        if (node instanceof Light) addLight((Light) node, transform);
        if (node instanceof SkinnedMesh) {
            Group skeleton = ((SkinnedMesh) node).getSkeleton();
            collectLights(skeleton, place(skeleton, transform));
        } else if (node instanceof Group) {
            Group group = (Group) node;
            for (int i = 0; i < group.getChildCount(); i++) {
                Node child = group.getChild(i);
                collectLights(child, place(child, transform));
            }
        }
    }

    private void drawScene(Node node, Transform transform) {
        Vector surfaces = new Vector();
        collectSurfaces(node, transform, 1f, surfaces);
        Transform projection = viewProjection();
        for (int i = 0; i < surfaces.size(); i++) {
            Surface surface = (Surface) surfaces.elementAt(i);
            if (surface.node instanceof Sprite3D) {
                Transform modelView = new Transform(view);
                modelView.postMultiply(surface.model);
                raster.drawSprite((Sprite3D) surface.node, surface.look, modelView,
                                  camera.projection, surface.alpha);
            } else {
                Mesh mesh = (Mesh) surface.node;
                draw(mesh.vertices, mesh.indices[surface.index], surface.look,
                     surface.model, projection, surface.alpha, mesh.scope, mesh);
            }
        }
    }

    private void collectSurfaces(Node node, Transform model, float alpha, Vector out) {
        if (!node.rendering || (alpha *= node.alpha) == 0) return;
        if (node instanceof SkinnedMesh) {
            Group skeleton = ((SkinnedMesh) node).getSkeleton();
            collectSurfaces(skeleton, place(skeleton, model), alpha, out);
        }
        if (node instanceof Mesh) {
            if ((node.scope & camera.scope) == 0) return;
            Mesh mesh = (Mesh) node;
            for (int i = 0; i < mesh.appearances.length; i++) {
                addSurface(node, i, mesh.appearances[i], model, alpha, out);
            }
        } else if (node instanceof Sprite3D) {
            if ((node.scope & camera.scope) == 0) return;
            addSurface(node, 0, ((Sprite3D) node).getAppearance(), model, alpha, out);
        } else if (node instanceof Group) {
            Group group = (Group) node;
            for (int i = 0; i < group.getChildCount(); i++) {
                Node child = group.getChild(i);
                collectSurfaces(child, place(child, model), alpha, out);
            }
        }
    }

    private static void addSurface(Node node, int index, Appearance look, Transform model,
                                   float alpha, Vector out) {
        if (look == null) return;
        int position = out.size();
        while (position > 0 && precedes(look, ((Surface) out.elementAt(position - 1)).look)) {
            position--;
        }
        out.insertElementAt(new Surface(node, index, look, model, alpha), position);
    }

    private static boolean precedes(Appearance a, Appearance b) {
        if (a.layer != b.layer) return a.layer < b.layer;
        return !blended(a) && blended(b);
    }

    private static boolean blended(Appearance look) {
        return look.compositing != null && look.compositing.blend != CompositingMode.REPLACE;
    }

    static void normalize(float[] vector) {
        float length = (float) Math.sqrt(vector[0] * vector[0] +
            vector[1] * vector[1] + vector[2] * vector[2]);
        if (length != 0) {
            for (int axis = 0; axis < 3; axis++) vector[axis] /= length;
        }
    }

    static final class SceneLight {
        final Light light;
        final float[] direction = new float[4];

        SceneLight(Light light, Transform transform) {
            this.light = light;
            transform.apply(0, 0, 1, 0, direction);
            normalize(direction);
        }
    }

    private static final class Surface {
        final Node node;
        final int index;
        final Appearance look;
        final Transform model;
        final float alpha;

        Surface(Node node, int index, Appearance look, Transform model, float alpha) {
            this.node = node;
            this.index = index;
            this.look = look;
            this.model = model;
            this.alpha = alpha;
        }
    }
}
