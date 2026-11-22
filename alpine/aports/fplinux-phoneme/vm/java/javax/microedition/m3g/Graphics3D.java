// SPDX-License-Identifier: GPL-2.0-only
package javax.microedition.m3g;

import java.util.Hashtable;
import java.util.Vector;
import javax.microedition.lcdui.FPLinuxNokiaImage;
import javax.microedition.lcdui.Graphics;

public final class Graphics3D {
    public static final int ANTIALIAS = 2;
    public static final int DITHER = 4;
    public static final int TRUE_COLOR = 8;
    public static final int OVERWRITE = 16;

    private static final Graphics3D INSTANCE = new Graphics3D();
    private static final int MAX_DIMENSION = 1024;

    private final Vector lights = new Vector();
    private final Transform view = new Transform();
    private final float[] eye = new float[4];
    private final float[] towardEye = new float[4];
    private final Rasterizer raster = new Rasterizer();

    // Rendering reuses these instead of allocating per node and mesh. Every
    // render call refills the transform pool from its start; nothing taken
    // from it is kept after that call.
    private final Transform identity = new Transform();
    private final Transform local = new Transform();
    private final Transform cameraInverse = new Transform();
    private final Transform sceneProjection = new Transform();
    private final Transform screen = new Transform();
    private final Transform modelView = new Transform();
    private final Vector surfaces = new Vector();
    private final Vector surfacePool = new Vector();
    private final Vector transformPool = new Vector();
    private int transformsUsed;
    private Graphics target;
    private int originX, originY, clipX, clipY, width, height;
    private boolean hasAlpha;
    private Camera camera;
    private int viewportX, viewportY, viewportWidth, viewportHeight;
    private int hints;
    private boolean depthBuffer;
    private float depthNear, depthFar = 1;

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
        viewportX = graphics.getClipX();
        viewportY = graphics.getClipY();
        viewportWidth = width;
        viewportHeight = height;
        this.depthBuffer = depthBuffer;
        this.hints = hints;
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
        viewportX = x;
        viewportY = y;
        viewportWidth = width;
        viewportHeight = height;
    }

    public int getViewportX() {
        return viewportX;
    }

    public int getViewportY() {
        return viewportY;
    }

    public int getViewportWidth() {
        return viewportWidth;
    }

    public int getViewportHeight() {
        return viewportHeight;
    }

    public void setDepthRange(float near, float far) {
        if (!(near >= 0 && near <= 1 && far >= 0 && far <= 1)) {
            throw new IllegalArgumentException("depth range");
        }
        depthNear = near;
        depthFar = far;
        raster.depthRange(near, far);
    }

    public float getDepthRangeNear() {
        return depthNear;
    }

    public float getDepthRangeFar() {
        return depthFar;
    }

    public Object getTarget() {
        return target;
    }

    public boolean isDepthBufferEnabled() {
        return target != null && depthBuffer;
    }

    public int getHints() {
        return target == null ? 0 : hints;
    }

    // Limits of the software renderer: it has none beyond memory, so these are
    // the sizes typical content stays within.
    public static Hashtable getProperties() {
        Hashtable properties = new Hashtable();
        properties.put("supportAntialiasing", Boolean.FALSE);
        properties.put("supportTrueColor", Boolean.TRUE);
        properties.put("supportDithering", Boolean.FALSE);
        properties.put("supportMipmapping", Boolean.FALSE);
        properties.put("supportPerspectiveCorrection", Boolean.TRUE);
        properties.put("supportLocalCameraLighting", Boolean.TRUE);
        properties.put("maxLights", new Integer(8));
        properties.put("maxViewportWidth", new Integer(MAX_DIMENSION));
        properties.put("maxViewportHeight", new Integer(MAX_DIMENSION));
        properties.put("maxViewportDimension", new Integer(MAX_DIMENSION));
        properties.put("maxTextureDimension", new Integer(MAX_DIMENSION));
        properties.put("maxSpriteCropDimension", new Integer(MAX_DIMENSION));
        properties.put("maxTransformsPerVertex", new Integer(4));
        properties.put("numTextureUnits", new Integer(1));
        return properties;
    }

    private static int position(long coordinate) {
        return (int) Math.max(Integer.MIN_VALUE, Math.min(Integer.MAX_VALUE, coordinate));
    }

    public void setCamera(Camera value, Transform transform) {
        Transform pose = transform == null ? identity : transform;
        cameraInverse.set(pose);
        cameraInverse.invert();
        pose.apply(0, 0, 0, 1, eye);
        pose.apply(0, 0, 1, 0, towardEye);
        normalize(towardEye);
        view.set(cameraInverse);
        camera = value;
    }

    public int addLight(Light value, Transform transform) {
        if (value == null) throw new NullPointerException();
        lights.addElement(new SceneLight(value, transform == null ? identity : transform));
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
        transformsUsed = 0;
        // Some authored scenes keep the selected camera as a separate root.
        setCamera(active, active.parent == null ? place(active, identity) :
                  relativeTo(active, world));
        resetLights();
        collectLights(world, identity);
        clear(world.getBackground());
        drawScene(world, identity);
    }

    public void render(Node node, Transform transform) {
        requireCamera();
        if (node == null) throw new NullPointerException();
        transformsUsed = 0;
        drawScene(node, transform == null ? identity : transform);
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
        draw(vertices, indices, appearance, transform == null ? identity : transform,
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
        sceneProjection.set(camera.projection);
        sceneProjection.postMultiply(view);
        return sceneProjection;
    }

    private void draw(VertexBuffer vertices, IndexBuffer indices, Appearance look,
                      Transform model, Transform projection, float alpha, int scope,
                      Mesh mesh) {
        screen.set(projection);
        screen.postMultiply(model);
        raster.draw(vertices, (TriangleStripArray) indices, look, screen, model, view,
                    alpha, lights, scope, eye, towardEye, mesh);
    }

    private Transform place(Node node, Transform parent) {
        node.getCompositeTransform(local);
        if (transformsUsed == transformPool.size()) {
            transformPool.addElement(new Transform());
        }
        Transform result = (Transform) transformPool.elementAt(transformsUsed++);
        result.set(parent);
        result.postMultiply(local);
        return result;
    }

    private Transform relativeTo(Node node, Node root) {
        if (node == root) return identity;
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
        try {
            collectSurfaces(node, transform, 1f, surfaces);
            Transform projection = viewProjection();
            for (int i = 0; i < surfaces.size(); i++) {
                Surface surface = (Surface) surfaces.elementAt(i);
                if (surface.node instanceof Sprite3D) {
                    modelView.set(view);
                    modelView.postMultiply(surface.model);
                    raster.drawSprite((Sprite3D) surface.node, surface.look, modelView,
                                      camera.projection, surface.alpha);
                } else {
                    Mesh mesh = (Mesh) surface.node;
                    draw(mesh.vertices, mesh.indices[surface.index], surface.look,
                         surface.model, projection, surface.alpha, mesh.scope, mesh);
                }
            }
        } finally {
            // Do not keep the scene reachable after rendering.
            for (int i = 0; i < surfaces.size(); i++) {
                Surface surface = (Surface) surfaces.elementAt(i);
                surface.node = null;
                surface.look = null;
                surface.model = null;
            }
            surfaces.removeAllElements();
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

    private void addSurface(Node node, int index, Appearance look, Transform model,
                            float alpha, Vector out) {
        if (look == null) return;
        int position = out.size();
        while (position > 0 && precedes(look, ((Surface) out.elementAt(position - 1)).look)) {
            position--;
        }
        // The collected surfaces are the first out.size() pool entries.
        if (out.size() == surfacePool.size()) surfacePool.addElement(new Surface());
        Surface surface = (Surface) surfacePool.elementAt(out.size());
        surface.node = node;
        surface.index = index;
        surface.look = look;
        surface.model = model;
        surface.alpha = alpha;
        out.insertElementAt(surface, position);
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
        Node node;
        int index;
        Appearance look;
        Transform model;
        float alpha;
    }
}
