// SPDX-License-Identifier: GPL-2.0-only
package javax.microedition.m3g;

import java.util.Vector;

final class Rasterizer {
    // Shared with fplinux-pm-m3g.c: xyzw, uv, alpha, front RGB, back RGB, fog.
    private static final int COMPONENTS = 14;
    private static final CompositingMode DEFAULT_COMPOSITING = new CompositingMode();
    private static final PolygonMode DEFAULT_POLYGON = new PolygonMode();
    private static final int[] SPRITE_STRIPS = { 4 };
    private static final int[] SPRITE_INDICES = { 0, 1, 2, 3 };

    int[] pixels = new int[0];
    private float[] depths = new float[0];
    private float[] vertices = new float[0];
    private final float[] spriteOrigin = new float[4];
    private final float[] spriteReference = new float[4];
    private final float[] spriteCenter = new float[4];
    private final int[] view = new int[6];
    private final int[] state = new int[16];
    private final int[] backgroundView = new int[8];
    private int left, top, right, bottom;
    private boolean depthEnabled;

    void bind(int width, int height, boolean depthBuffer) {
        view[0] = width;
        view[1] = height;
        int count = width * height;
        if (count > pixels.length) {
            int[] newPixels = new int[count];
            float[] newDepths = new float[count];
            pixels = newPixels;
            depths = newDepths;
        }
        depthEnabled = depthBuffer;
    }

    void viewport(int x, int y, int width, int height) {
        view[2] = x;
        view[3] = y;
        view[4] = width;
        view[5] = height;
        left = Math.max(0, x);
        top = Math.max(0, y);
        right = (int) Math.min(view[0], (long) x + width);
        bottom = (int) Math.min(view[1], (long) y + height);
    }

    void resetDepth() {
        clear0(pixels, depths, view, 0, false, true, null, null, false);
    }

    void clear(Background background, boolean alpha) {
        if (left >= right || top >= bottom) return;
        Image2D image = background == null ? null : background.image;
        int color = background == null ? 0 : background.color;
        boolean clearDepth = depthEnabled && (background == null || background.depthClear);
        boolean clearColor = background == null || background.colorClear;
        int opaque = alpha ? 0 : 0xff000000;
        int[] imagePixels = null;
        if (image != null && clearColor && background.width > 0 && background.height > 0) {
            imagePixels = image.pixels();
            backgroundView[0] = image.getWidth();
            backgroundView[1] = image.getHeight();
            backgroundView[2] = background.x;
            backgroundView[3] = background.y;
            backgroundView[4] = background.width;
            backgroundView[5] = background.height;
            backgroundView[6] = background.modeX;
            backgroundView[7] = background.modeY;
        }
        clear0(pixels, depths, view, color | opaque, clearColor, clearDepth,
               imagePixels, backgroundView, !alpha);
    }

    void draw(VertexBuffer buffer, TriangleStripArray triangles, Appearance look,
              Transform projection, Transform model, Transform cameraView, float alpha,
              Vector lights, int scope, float[] eye, float[] towardEye, Mesh mesh) {
        int count = buffer.getVertexCount();
        if (count == 0 || triangles.maxIndex >= count) {
            throw new IllegalStateException("mesh vertex indices");
        }
        if (left >= right || top >= bottom) return;
        if (vertices.length < count * COMPONENTS) vertices = new float[count * COMPONENTS];
        SkinnedMesh skin = mesh instanceof SkinnedMesh ? (SkinnedMesh) mesh : null;
        MorphingMesh morph = mesh instanceof MorphingMesh ? (MorphingMesh) mesh : null;
        boolean deformed = skin != null || morph != null;
        Transform texture = null;
        Texture2D texture0 = look.textures[0];
        if (look.textures[1] != null) throw new IllegalStateException("multiple textures");
        if (texture0 != null) {
            texture = new Transform();
            texture0.getCompositeTransform(texture);
        }
        VertexArray texCoords = texture0 == null ? null : buffer.texCoords[0];
        PolygonMode polygon = look.polygon == null ? DEFAULT_POLYGON : look.polygon;
        boolean local = look.material != null && polygon.localCameraLighting;
        boolean backLighting = look.material != null && polygon.twoSidedLighting;
        boolean constantColor = look.material == null && look.fog == null && buffer.colors == null;
        float[] direction = local ? new float[4] : towardEye;
        float[] fogPoint = look.fog == null ? null : new float[4];
        float[] point = deformed || local || fogPoint != null ? new float[3] : null;
        Transform fogTransform = null;
        if (fogPoint != null) {
            fogTransform = new Transform(cameraView);
            fogTransform.postMultiply(model);
        }
        float[] inverse = null;
        Material material = look.material;
        if (buffer.normals != null && material != null) {
            Transform normalTransform = new Transform(model);
            try {
                normalTransform.invert();
                inverse = normalTransform.matrix;
            } catch (ArithmeticException singularModel) {
                // Continue drawing with ambient and emissive lighting when no normal matrix exists.
            }
        }
        prepareVertices0(vertices, count, buffer.positions.data, buffer.positions.components,
                         buffer.position, projection.matrix,
                         texCoords == null ? null : texCoords.data,
                         texCoords == null ? 0 : texCoords.components, buffer.texTransform[0],
                         texture == null ? null : texture.matrix, constantColor,
                         buffer.defaultColor, alpha);
        if (skin != null) skin.prepareSkinning();
        if (morph != null) morph.prepareMorphing();
        boolean shaded = inverse != null && !deformed && fogPoint == null && !local &&
            !backLighting && shadeDirectional(buffer, count, inverse, material, lights, scope, alpha);
        int shadeCount = shaded || (constantColor && !deformed) ? 0 : count;
        float[] normal = shadeCount == 0 ? null : new float[3];
        for (int i = 0, offset = 0; i < shadeCount; i++, offset += COMPONENTS) {
            if (point != null) buffer.point(i, point);
            float a = 0, b = 0, c = 0;
            if (inverse != null) {
                a = buffer.normals.get(i, 0);
                b = buffer.normals.get(i, 1);
                c = buffer.normals.get(i, 2);
            }
            if (deformed) {
                normal[0] = a;
                normal[1] = b;
                normal[2] = c;
                float[] meshNormal = inverse == null ? null : normal;
                if (skin != null) skin.skin(i, point, meshNormal);
                else morph.morph(i, point, meshNormal);
                a = normal[0];
                b = normal[1];
                c = normal[2];
                projection.apply(point[0], point[1], point[2], 1, vertices, offset);
            }
            if (constantColor) continue;
            int color = buffer.color(i);
            if (look.material != null) {
                if (local) {
                    model.apply(point[0], point[1], point[2], 1, direction);
                    for (int axis = 0; axis < 3; axis++) {
                        direction[axis] = eye[axis] / eye[3] - direction[axis] / direction[3];
                    }
                    Graphics3D.normalize(direction);
                }
                if (inverse != null) {
                    for (int axis = 0; axis < 3; axis++) {
                        normal[axis] = a * inverse[axis] + b * inverse[4 + axis] + c * inverse[8 + axis];
                    }
                    Graphics3D.normalize(normal);
                }
                if (backLighting) {
                    rgb(offset + 10, light(color, look.material, normal, direction, lights, scope, -1));
                }
                color = light(color, look.material, normal, direction, lights, scope, 1);
            }
            vertices[offset + 6] = channel(color, 24) * alpha;
            rgb(offset + 7, color);
            if (fogTransform != null) {
                fogTransform.apply(point[0], point[1], point[2], 1, fogPoint);
                vertices[offset + 13] = look.fog.visibility(Math.abs(fogPoint[2] / fogPoint[3]));
            }
        }
        setupState(look, polygon, backLighting);
        if (texture0 != null) {
            setupTexture(texture0.image, texture0.wrapS, texture0.wrapT,
                         texture0.blending, texture0.blendColor, texture0.imageFilter);
        }
        draw0(pixels, depths, view, vertices, count, triangles.strips, triangles.indices,
              texture0 == null ? null : texture0.image.pixels(), state);
    }

    void drawSprite(Sprite3D sprite, Appearance look, Transform modelView,
                    Transform projection, float alpha) {
        if (left >= right || top >= bottom) return;
        Image2D image = sprite.getImage();
        int cropX = sprite.getCropX(), cropY = sprite.getCropY();
        int cropWidth = Math.abs(sprite.getCropWidth()), cropHeight = Math.abs(sprite.getCropHeight());
        if (cropWidth == 0 || cropHeight == 0) return;
        int sourceLeft = Math.max(0, cropX);
        int sourceTop = Math.max(0, cropY);
        int sourceRight = (int) Math.min(image.getWidth(), (long) cropX + cropWidth);
        int sourceBottom = (int) Math.min(image.getHeight(), (long) cropY + cropHeight);
        if (sourceLeft >= sourceRight || sourceTop >= sourceBottom) return;

        modelView.apply(0, 0, 0, 1, spriteOrigin);
        projection.apply(spriteOrigin[0], spriteOrigin[1], spriteOrigin[2], spriteOrigin[3],
                         spriteCenter);
        if (!(spriteCenter[3] > 0)) return;
        double centerX = spriteCenter[0] / (double) spriteCenter[3];
        double centerY = spriteCenter[1] / (double) spriteCenter[3];
        float depth = spriteCenter[2] / spriteCenter[3];
        double width, height, x, y;
        if (sprite.isScaled()) {
            // Project camera-aligned axes with the lengths of the two transformed sprite axes.
            modelView.apply(1, 0, 0, 1, spriteReference);
            float dx = (float) distance(spriteOrigin, spriteReference);
            modelView.apply(0, 1, 0, 1, spriteReference);
            float dy = (float) distance(spriteOrigin, spriteReference);
            float ox = spriteOrigin[0], oy = spriteOrigin[1];
            float oz = spriteOrigin[2], ow = spriteOrigin[3];
            projection.apply(ox + dx, oy, oz, ow, spriteReference);
            width = distance(spriteCenter, spriteReference);
            projection.apply(ox, oy + dy, oz, ow, spriteReference);
            height = distance(spriteCenter, spriteReference);
            if (!(width > 0 && height > 0)) return;
            x = centerX - width * .5;
            y = centerY + height * .5;
        } else {
            // Snap unscaled corners to pixel boundaries to keep their exact dimensions.
            double zoomX = 2.0 / view[4], zoomY = 2.0 / view[5];
            width = cropWidth * zoomX;
            height = cropHeight * zoomY;
            x = Math.floor((centerX + 1) / zoomX - cropWidth * .5 + .5) * zoomX - 1;
            y = 1 - Math.floor((1 - centerY) / zoomY - cropHeight * .5 + .5) * zoomY;
        }

        // Trim off-image texels from the quad so they cannot write color or depth.
        boolean flipX = sprite.getCropWidth() < 0, flipY = sprite.getCropHeight() < 0;
        double xScale = width / cropWidth, yScale = height / cropHeight;
        double x0 = x + xScale * (flipX ? cropWidth - (sourceRight - cropX) : sourceLeft - cropX);
        double y0 = y - yScale * (flipY ? cropHeight - (sourceBottom - cropY) : sourceTop - cropY);
        double x1 = x0 + xScale * (sourceRight - sourceLeft);
        double y1 = y0 - yScale * (sourceBottom - sourceTop);
        float u0 = (flipX ? sourceRight : sourceLeft) / (float) image.getWidth();
        float u1 = (flipX ? sourceLeft : sourceRight) / (float) image.getWidth();
        float v0 = (flipY ? sourceBottom : sourceTop) / (float) image.getHeight();
        float v1 = (flipY ? sourceTop : sourceBottom) / (float) image.getHeight();
        float fog = look.fog == null ? 1 :
            look.fog.visibility(Math.abs(spriteOrigin[2] / spriteOrigin[3]));

        if (vertices.length < 4 * COMPONENTS) vertices = new float[4 * COMPONENTS];
        spriteVertex(0, (float) x0, (float) y0, depth, u0, v0, alpha, fog);
        spriteVertex(1, (float) x0, (float) y1, depth, u0, v1, alpha, fog);
        spriteVertex(2, (float) x1, (float) y0, depth, u1, v0, alpha, fog);
        spriteVertex(3, (float) x1, (float) y1, depth, u1, v1, alpha, fog);
        setupState(look, DEFAULT_POLYGON, false);
        setupTexture(image, Texture2D.WRAP_CLAMP, Texture2D.WRAP_CLAMP,
                     Texture2D.FUNC_MODULATE, 0, Texture2D.FILTER_NEAREST);
        draw0(pixels, depths, view, vertices, 4, SPRITE_STRIPS, SPRITE_INDICES,
              image.pixels(), state);
    }

    private static double distance(float[] a, float[] b) {
        double aw = a[3], bw = b[3];
        double x = a[0] / aw - b[0] / bw;
        double y = a[1] / aw - b[1] / bw;
        double z = a[2] / aw - b[2] / bw;
        return Math.sqrt(x * x + y * y + z * z);
    }

    private void spriteVertex(int index, float x, float y, float depth, float u, float v,
                              float alpha, float fog) {
        int offset = index * COMPONENTS;
        vertices[offset] = x;
        vertices[offset + 1] = y;
        vertices[offset + 2] = depth;
        vertices[offset + 3] = 1;
        vertices[offset + 4] = u;
        vertices[offset + 5] = v;
        vertices[offset + 6] = alpha * 255;
        rgb(offset + 7, 0xffffff);
        vertices[offset + 13] = fog;
    }

    private void rgb(int offset, int color) {
        vertices[offset] = channel(color, 16);
        vertices[offset + 1] = channel(color, 8);
        vertices[offset + 2] = channel(color, 0);
    }

    private void setupState(Appearance look, PolygonMode polygon, boolean backLighting) {
        CompositingMode blend = look.compositing == null ? DEFAULT_COMPOSITING : look.compositing;
        // Layout shared with fplinux-pm-m3g.c. Flags select the inner loop's operations.
        state[0] = polygon.culling;
        state[1] = polygon.winding;
        state[2] = (polygon.shading == PolygonMode.SHADE_SMOOTH ? 1 : 0) |
            (polygon.perspective ? 2 : 0) | (depthEnabled && blend.depthTest ? 4 : 0) |
            (depthEnabled && blend.depthWrite ? 8 : 0) | (blend.colorWrite ? 16 : 0) |
            (blend.alphaWrite ? 32 : 0) | (backLighting ? 64 : 0) |
            (look.fog != null ? 128 : 0);
        state[3] = blend.blend;
        state[4] = (int) (blend.alphaThreshold * 255 + .5f);
        state[5] = Float.floatToIntBits(blend.depthOffsetUnits);
        state[12] = Float.floatToIntBits(blend.depthOffsetFactor);
        if (look.fog != null) state[15] = look.fog.getColor();
    }

    private void setupTexture(Image2D image, int wrapS, int wrapT,
                              int function, int color, int filter) {
        state[6] = image.getWidth();
        state[7] = image.getHeight();
        state[8] = wrapS;
        state[9] = wrapT;
        state[10] = function;
        state[11] = color;
        state[13] = image.format;
        state[14] = filter;
    }

    private boolean shadeDirectional(VertexBuffer buffer, int count, float[] inverse,
                                     Material material, Vector lights, int scope, float alpha) {
        VertexArray normals = buffer.normals;
        VertexArray colors = buffer.colors;
        if (material.vertexColorTracking || material.specular != 0 || normals.components != 3 ||
                (colors != null && (colors.components < 3 || colors.vertices < count))) {
            return false;
        }

        Light ambient = null;
        Graphics3D.SceneLight directional = null;
        boolean directionalFirst = false;
        for (int i = 0; i < lights.size(); i++) {
            Graphics3D.SceneLight source = (Graphics3D.SceneLight) lights.elementAt(i);
            Light light = source.light;
            if (!light.rendering || (light.scope & scope) == 0) continue;
            if (light.mode == Light.AMBIENT && ambient == null) {
                ambient = light;
            } else if (light.mode == Light.DIRECTIONAL && directional == null) {
                directional = source;
                directionalFirst = ambient == null;
            } else {
                return false;
            }
        }
        return ambient != null && directional != null &&
            shadeDirectional0(vertices, count, normals.data, inverse,
                              material.emissive, material.ambient, material.diffuse,
                              ambient.color, ambient.intensity,
                              directional.light.color, directional.light.intensity,
                              directional.direction, directionalFirst, alpha);
    }

    private static int light(int color, Material material, float[] normal, float[] eye,
                             Vector lights, int scope, int side) {
        int diffuse = material.vertexColorTracking ? color : material.diffuse;
        int ambient = material.vertexColorTracking ? color : material.ambient;
        float r = channel(material.emissive, 16), g = channel(material.emissive, 8);
        float b = channel(material.emissive, 0);
        for (int i = 0; i < lights.size(); i++) {
            Graphics3D.SceneLight source = (Graphics3D.SceneLight) lights.elementAt(i);
            Light light = source.light;
            if (!light.rendering || (light.scope & scope) == 0) continue;
            if (light.mode != Light.AMBIENT && light.mode != Light.DIRECTIONAL) continue;
            int reflected = light.mode == Light.AMBIENT ? ambient : diffuse;
            float gain = light.intensity / 255;
            float highlight = 0;
            if (light.mode == Light.DIRECTIONAL) {
                float facing = side * dot(normal, source.direction);
                if (facing <= 0) continue;
                if (material.specular != 0) {
                    float hx = eye[0] + source.direction[0];
                    float hy = eye[1] + source.direction[1];
                    float hz = eye[2] + source.direction[2];
                    float length = (float) Math.sqrt(hx * hx + hy * hy + hz * hz);
                    if (length > 0) {
                        float alignment = side * (normal[0] * hx + normal[1] * hy + normal[2] * hz) / length;
                        if (alignment > 0) highlight = gain * pow0(Math.min(1, alignment), material.shininess);
                    }
                }
                gain *= facing;
            }
            r += channel(light.color, 16) *
                (gain * channel(reflected, 16) + highlight * channel(material.specular, 16));
            g += channel(light.color, 8) *
                (gain * channel(reflected, 8) + highlight * channel(material.specular, 8));
            b += channel(light.color, 0) *
                (gain * channel(reflected, 0) + highlight * channel(material.specular, 0));
        }
        return diffuse & 0xff000000 | clamp(r) << 16 | clamp(g) << 8 | clamp(b);
    }

    private static float dot(float[] a, float[] b) {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    }

    private static int channel(int color, int shift) {
        return color >>> shift & 255;
    }

    private static int clamp(float value) {
        return Math.min(255, Math.max(0, (int) (value + .5f)));
    }

    private static native void clear0(int[] pixels, float[] depths, int[] view,
                                      int color, boolean colorClear, boolean depthClear,
                                      int[] image, int[] crop, boolean opaque);

    private static native void prepareVertices0(float[] vertices, int count,
                                               short[] positions, int positionComponents,
                                               float[] positionScaleBias, float[] projection,
                                               short[] texCoords, int texComponents,
                                               float[] texScaleBias, float[] textureMatrix,
                                               boolean constantColor, int color, float alpha);

    private static native boolean shadeDirectional0(float[] vertices, int count, short[] normals,
                                                    float[] inverse, int emissive, int ambient,
                                                    int diffuse, int ambientColor, float ambientIntensity,
                                                    int directionalColor, float directionalIntensity,
                                                    float[] direction, boolean directionalFirst, float alpha);

    private static native void draw0(int[] pixels, float[] depths, int[] view,
                                    float[] vertices, int vertexCount, int[] strips, int[] indices,
                                    int[] texture, int[] state);

    private static native float pow0(float value, float exponent);
}
