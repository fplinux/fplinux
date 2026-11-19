// SPDX-License-Identifier: GPL-2.0-only
package javax.microedition.m3g;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.Hashtable;
import java.util.Vector;
import javax.microedition.lcdui.Image;

public final class Loader {
    private static final byte[] SIGNATURE = {
        (byte) 0xab, 'J', 'S', 'R', '1', '8', '4', (byte) 0xbb, 13, 10, 26, 10,
    };
    private static final byte[] PNG_SIGNATURE = {
        (byte) 0x89, 'P', 'N', 'G', 13, 10, 26, 10,
    };
    private final String directory;
    private final Hashtable loading;
    private final Vector chunks = new Vector();
    private Object3D[] objects;
    private boolean[] referenced, decoding;

    private Loader(String path, Hashtable loading) {
        directory = path.substring(0, path.lastIndexOf('/') + 1);
        this.loading = loading;
        chunks.addElement(null);
    }

    public static Object3D[] load(String name) throws IOException {
        if (name == null) {
            throw new NullPointerException();
        }
        return resource(name, new Hashtable());
    }

    private static Object3D[] resource(String name, Hashtable loading) throws IOException {
        String path = resourcePath(name);
        if (loading.put(path, path) != null) throw new IOException("cyclic M3G resource");
        try {
            InputStream stream = Loader.class.getResourceAsStream(path);
            if (stream == null) throw new IOException("missing M3G resource " + path);
            byte[] data;
            try {
                ByteArrayOutputStream contents = new ByteArrayOutputStream();
                byte[] page = new byte[2048];
                int length;
                while ((length = stream.read(page)) >= 0) contents.write(page, 0, length);
                data = contents.toByteArray();
            } finally {
                stream.close();
            }
            return new Loader(path, loading).decode(data, 0);
        } finally {
            loading.remove(path);
        }
    }

    public static Object3D[] load(byte[] data, int offset) throws IOException {
        return new Loader("/", new Hashtable()).decode(data, offset);
    }

    private static String resourcePath(String name) throws IOException {
        if (name.indexOf(':') >= 0) throw new IOException("M3G resource URL");
        Vector parts = new Vector();
        for (int start = 0; start <= name.length();) {
            int slash = name.indexOf('/', start);
            int end = slash < 0 ? name.length() : slash;
            String part = name.substring(start, end);
            if ("..".equals(part)) {
                if (parts.size() == 0) throw new IOException("M3G resource path");
                parts.removeElementAt(parts.size() - 1);
            } else if (part.length() != 0 && !".".equals(part)) {
                parts.addElement(part);
            }
            start = end + 1;
        }
        StringBuffer path = new StringBuffer();
        for (int i = 0; i < parts.size(); i++) path.append('/').append(parts.elementAt(i));
        if (path.length() == 0) throw new IOException("empty M3G resource path");
        return path.toString();
    }

    private Object3D[] decode(byte[] data, int offset) throws IOException {
        Reader file = new Reader(data, offset, data.length);
        if (matches(data, offset, PNG_SIGNATURE)) {
            try {
                Image image = Image.createImage(data, offset, data.length - offset);
                return new Object3D[] { new Image2D(Image2D.RGBA, image) };
            } catch (IllegalArgumentException invalid) {
                throw new IOException("invalid M3G PNG");
            }
        }
        for (int i = 0; i < SIGNATURE.length; i++) {
            if (file.byteValue() != (SIGNATURE[i] & 255)) throw new IOException("M3G signature");
        }
        if (file.remaining() == 0 || data[file.offset] != 0) throw new IOException("M3G header");
        Reader headerSection = readSection(file);
        if (headerSection.byteValue() != 0) throw new IOException("M3G header object");
        Reader header = headerSection.slice(headerSection.length());
        if (headerSection.remaining() != 0 || header.byteValue() != 1 ||
                header.byteValue() != 0) throw new IOException("M3G header version");
        header.bool();
        int size = header.length();
        header.skip(4);
        header.string();
        if (header.remaining() != 0 || size < file.offset - offset || size > data.length - offset) {
            throw new IOException("M3G file length");
        }
        chunks.addElement(null);
        file = new Reader(data, file.offset, offset + size);
        while (file.remaining() != 0) {
            Reader section = readSection(file);
            while (section.remaining() != 0) {
                int type = section.byteValue();
                if (type == 0) throw new IOException("duplicate M3G header");
                chunks.addElement(new Chunk(type, section.slice(section.length())));
            }
        }
        objects = new Object3D[chunks.size()];
        referenced = new boolean[objects.length];
        decoding = new boolean[objects.length];
        for (int i = 2; i < objects.length; i++) object(i);
        Vector roots = new Vector();
        for (int i = 2; i < objects.length; i++) {
            if (objects[i] != null && !referenced[i]) {
                roots.addElement(objects[i]);
            }
        }
        Object3D[] result = new Object3D[roots.size()];
        roots.copyInto(result);
        return result;
    }

    private static boolean matches(byte[] data, int offset, byte[] signature) {
        if (data.length - offset < signature.length) return false;
        for (int i = 0; i < signature.length; i++) {
            if (data[offset + i] != signature[i]) return false;
        }
        return true;
    }

    private static Reader readSection(Reader file) throws IOException {
        int start = file.offset;
        int compression = file.byteValue();
        int total = file.length();
        int length = file.length();
        if (total < 13 || compression > 1 || compression == 0 && length != total - 13) {
            throw new IOException("M3G section header");
        }
        Reader payload = file.slice(total - 13);
        if (file.intValue() != checksum0(file.data, start, total - 4)) {
            throw new IOException("M3G section checksum");
        }
        if (compression == 0) return payload;
        byte[] data = new byte[length];
        if (length != 0 && !inflate0(file.data, payload.offset, payload.remaining(), data)) {
            throw new IOException("M3G compressed section");
        }
        return new Reader(data, 0, length);
    }

    private static native int checksum0(byte[] data, int offset, int length);

    private static native boolean inflate0(byte[] source, int offset, int length, byte[] dest);

    private Object3D reference(Reader reader) throws IOException {
        int index = reader.length();
        if (index == 1 || index >= objects.length) {
            throw new IOException("M3G object index");
        }
        referenced[index] = true;
        return index == 0 ? null : object(index);
    }

    private Object3D object(int index) throws IOException {
        if (objects[index] != null) {
            return objects[index];
        }
        if (decoding[index]) {
            throw new IOException("cyclic M3G reference");
        }
        decoding[index] = true;
        Chunk chunk = (Chunk) chunks.elementAt(index);
        Reader reader = chunk.reader;
        Object3D result;
        if (chunk.type == 255) {
            result = external(reader.string());
        } else {
            int id = reader.intValue();
            int[] tracks = reader.indices();
            int parameters = reader.length();
            for (int i = 0; i < parameters; i++) {
                reader.skip(4);
                reader.skip(reader.length());
            }
            result = construct(chunk.type, reader);
            result.setUserID(id);
            objects[index] = result;
            for (int i = 0; i < tracks.length; i++) {
                int track = tracks[i];
                if (track <= 1 || track >= objects.length) {
                    throw new IOException("M3G animation track index");
                }
                referenced[track] = true;
                result.addAnimationTrack((AnimationTrack) object(track));
            }
        }
        objects[index] = result;
        decoding[index] = false;
        return result;
    }

    private Object3D external(String uri) throws IOException {
        String path = uri.startsWith("/") ? uri : directory + uri;
        Object3D[] scene = resource(path, loading);
        if (scene.length == 0) throw new IOException("empty M3G reference " + uri);
        return scene[0];
    }

    private Object3D construct(int type, Reader reader) throws IOException {
        switch (type) {
            case 1: return controller(reader);
            case 2: return track(reader);
            case 3: return appearance(reader);
            case 4: return background(reader);
            case 5: return camera(reader);
            case 6: return compositing(reader);
            case 7: return fog(reader);
            case 8: return polygon(reader);
            case 9: return group(reader, new Group());
            case 10: return image2d(reader);
            case 11: return strips(reader);
            case 12: return light(reader);
            case 13: return material(reader);
            case 14:
            case 15:
            case 16: return mesh(reader, type);
            case 17: return texture(reader);
            case 19: return keyframes(reader);
            case 20: return vertexArray(reader);
            case 21: return vertexBuffer(reader);
            case 22: return world(reader);
            default: throw new IOException("unsupported M3G object type " + type);
        }
    }

    private static void transformable(Transformable object, Reader reader)
            throws IOException {
        if (reader.bool()) {
            object.setTranslation(reader.scalar(), reader.scalar(), reader.scalar());
            object.setScale(reader.scalar(), reader.scalar(), reader.scalar());
            object.setOrientation(reader.scalar(), reader.scalar(), reader.scalar(),
                                  reader.scalar());
        }
        if (reader.bool()) {
            object.setTransform(reader.transform());
        }
    }

    private static void node(Node object, Reader reader) throws IOException {
        transformable(object, reader);
        object.rendering = reader.bool();
        object.picking = reader.bool();
        object.alpha = reader.byteValue() / 255f;
        object.scope = reader.intValue();
        if (reader.bool()) {
            reader.skip(10);
        }
    }

    private AnimationController controller(Reader reader) throws IOException {
        AnimationController controller = new AnimationController();
        float speed = reader.scalar();
        controller.setWeight(reader.scalar());
        controller.setActiveInterval(reader.intValue(), reader.intValue());
        float sequenceTime = reader.scalar();
        int worldTime = reader.intValue();
        controller.setSpeed(speed, worldTime);
        controller.setPosition(sequenceTime, worldTime);
        return controller;
    }

    private AnimationTrack track(Reader reader) throws IOException {
        KeyframeSequence sequence = (KeyframeSequence) reference(reader);
        AnimationController controller = (AnimationController) reference(reader);
        AnimationTrack track = new AnimationTrack(sequence, reader.length());
        track.setController(controller);
        return track;
    }

    private Appearance appearance(Reader reader) throws IOException {
        Appearance appearance = new Appearance();
        appearance.layer = (byte) reader.byteValue();
        appearance.compositing = (CompositingMode) reference(reader);
        appearance.fog = (Fog) reference(reader);
        appearance.polygon = (PolygonMode) reference(reader);
        appearance.material = (Material) reference(reader);
        int count = reader.length();
        if (count > appearance.textures.length) {
            throw new IOException("M3G texture units");
        }
        for (int i = 0; i < count; i++) {
            appearance.setTexture(i, (Texture2D) reference(reader));
        }
        return appearance;
    }

    private Background background(Reader reader) throws IOException {
        Background background = new Background();
        background.setColor(reader.rgba());
        background.setImage((Image2D) reference(reader));
        background.setImageMode(reader.byteValue(), reader.byteValue());
        background.setCrop(reader.intValue(), reader.intValue(),
                           reader.intValue(), reader.intValue());
        background.depthClear = reader.bool();
        background.colorClear = reader.bool();
        return background;
    }

    private Camera camera(Reader reader) throws IOException {
        Camera camera = new Camera();
        node(camera, reader);
        int mode = reader.byteValue();
        if (mode == Camera.GENERIC) {
            camera.setGeneric(reader.transform());
        } else if (mode == Camera.PERSPECTIVE) {
            camera.setPerspective(reader.scalar(), reader.scalar(),
                                  reader.scalar(), reader.scalar());
        } else if (mode == Camera.PARALLEL) {
            camera.setParallel(reader.scalar(), reader.scalar(),
                               reader.scalar(), reader.scalar());
        } else {
            throw new IOException("M3G projection mode");
        }
        return camera;
    }

    private CompositingMode compositing(Reader reader) throws IOException {
        CompositingMode mode = new CompositingMode();
        mode.depthTest = reader.bool();
        mode.depthWrite = reader.bool();
        mode.colorWrite = reader.bool();
        mode.alphaWrite = reader.bool();
        mode.blend = reader.byteValue();
        mode.alphaThreshold = reader.byteValue() / 255f;
        mode.depthOffsetFactor = reader.scalar();
        mode.depthOffsetUnits = reader.scalar();
        return mode;
    }

    private PolygonMode polygon(Reader reader) throws IOException {
        PolygonMode polygon = new PolygonMode();
        polygon.culling = reader.byteValue();
        polygon.shading = reader.byteValue();
        polygon.winding = reader.byteValue();
        polygon.twoSidedLighting = reader.bool();
        polygon.localCameraLighting = reader.bool();
        polygon.perspective = reader.bool();
        return polygon;
    }

    private Group group(Reader reader, Group group) throws IOException {
        node(group, reader);
        for (int count = reader.length(); count > 0; count--) {
            group.addChild((Node) reference(reader));
        }
        return group;
    }

    private World world(Reader reader) throws IOException {
        World world = (World) group(reader, new World());
        world.setActiveCamera((Camera) reference(reader));
        world.setBackground((Background) reference(reader));
        return world;
    }

    private Light light(Reader reader) throws IOException {
        Light light = new Light();
        node(light, reader);
        reader.skip(12);
        light.color = reader.rgb();
        light.setMode(reader.byteValue());
        light.setIntensity(reader.scalar());
        reader.skip(8);
        return light;
    }

    private Material material(Reader reader) throws IOException {
        Material material = new Material();
        material.ambient = reader.rgb();
        material.diffuse = reader.rgba();
        material.emissive = reader.rgb();
        material.specular = reader.rgb();
        material.setShininess(reader.scalar());
        material.vertexColorTracking = reader.bool();
        return material;
    }

    private static Fog fog(Reader reader) throws IOException {
        Fog fog = new Fog();
        fog.setColor(reader.rgb());
        try {
            fog.setMode(reader.byteValue());
            if (fog.getMode() == Fog.EXPONENTIAL) {
                fog.setDensity(reader.scalar());
            } else {
                fog.setLinear(reader.scalar(), reader.scalar());
            }
        } catch (IllegalArgumentException invalid) {
            throw new IOException("M3G fog parameters");
        }
        return fog;
    }

    private Texture2D texture(Reader reader) throws IOException {
        Transformable temporary = new Group();
        transformable(temporary, reader);
        Texture2D texture = new Texture2D((Image2D) reference(reader));
        Transform transform = new Transform();
        temporary.getCompositeTransform(transform);
        texture.setTransform(transform);
        texture.blendColor = reader.rgb();
        texture.setBlending(reader.byteValue());
        texture.setWrapping(reader.byteValue(), reader.byteValue());
        texture.setFiltering(reader.byteValue(), reader.byteValue());
        return texture;
    }

    private Image2D image2d(Reader reader) throws IOException {
        int format = reader.byteValue();
        boolean mutable = reader.bool();
        int width = reader.length(), height = reader.length();
        if (width == 0 || height == 0 || (long) width * height > Integer.MAX_VALUE ||
                mutable) {
            throw new IOException("M3G Image2D dimensions or mutability");
        }
        Reader palette = reader.slice(reader.length());
        Reader pixels = reader.slice(reader.length());
        int[] argb = new int[width * height];
        for (int i = 0; i < argb.length; i++) {
            Reader source = pixels;
            if (palette.remaining() != 0) {
                int stride = format == Image2D.RGB ? 3 :
                    format == Image2D.RGBA ? 4 : format == Image2D.LUMINANCE_ALPHA ? 2 : 1;
                int index = pixels.byteValue() * stride;
                source = new Reader(palette.data, palette.offset + index, palette.end);
            }
            if (format == Image2D.RGB) {
                argb[i] = 0xff000000 | source.rgb();
            } else if (format == Image2D.RGBA) {
                argb[i] = source.rgba();
            } else {
                int light = source.byteValue();
                int alpha = format == Image2D.LUMINANCE_ALPHA ? source.byteValue() :
                    format == Image2D.ALPHA ? light : 255;
                argb[i] = alpha << 24 | (format == Image2D.ALPHA ?
                    0xffffff : light * 0x010101);
            }
        }
        return new Image2D(format, Image.createRGBImage(argb, width, height, true));
    }

    private VertexArray vertexArray(Reader reader) throws IOException {
        int size = reader.byteValue(), components = reader.byteValue();
        int encoding = reader.byteValue(), count = reader.shortValue();
        VertexArray array = new VertexArray(count, components, size);
        if (encoding > 1) {
            throw new IOException("M3G vertex delta encoding");
        }
        int[] previous = new int[components];
        for (int i = 0; i < count; i++) {
            for (int j = 0; j < components; j++) {
                int value = size == 1 ? (byte) reader.byteValue() :
                    (short) reader.shortValue();
                if (encoding == 1) {
                    value += previous[j];
                }
                previous[j] = size == 1 ? (byte) value : (short) value;
                array.data[i * components + j] = (short) previous[j];
            }
        }
        return array;
    }

    private static void biasScale(float[] transform, Reader reader)
            throws IOException {
        for (int i = 1; i < transform.length; i++) {
            transform[i] = reader.scalar();
        }
        transform[0] = reader.scalar();
    }

    private VertexBuffer vertexBuffer(Reader reader) throws IOException {
        VertexBuffer buffer = new VertexBuffer();
        buffer.defaultColor = reader.rgba();
        buffer.positions = (VertexArray) reference(reader);
        biasScale(buffer.position, reader);
        buffer.normals = (VertexArray) reference(reader);
        buffer.colors = (VertexArray) reference(reader);
        int count = reader.length();
        if (count > buffer.texCoords.length) {
            throw new IOException("M3G vertex texture units");
        }
        for (int i = 0; i < count; i++) {
            buffer.texCoords[i] = (VertexArray) reference(reader);
            biasScale(buffer.texTransform[i], reader);
        }
        return buffer;
    }

    private TriangleStripArray strips(Reader reader) throws IOException {
        int flags = reader.byteValue();
        int format = flags & 127;
        int[] indices = null;
        int base = 0;
        if ((flags & 128) != 0) {
            int count = reader.length();
            if (count > reader.remaining()) {
                throw new IOException("M3G index count");
            }
            indices = new int[count];
            for (int i = 0; i < count; i++) {
                indices[i] = reader.index(format);
            }
        } else {
            base = reader.index(format);
        }
        int[] lengths = reader.indices();
        if (indices == null) {
            int count = 0;
            for (int i = 0; i < lengths.length; i++) {
                if (lengths[i] > 65536 - base - count) {
                    throw new IOException("M3G implicit index count");
                }
                count += lengths[i];
            }
            indices = new int[count];
            for (int i = 0; i < count; i++) {
                indices[i] = base + i;
            }
        }
        return new TriangleStripArray(indices, lengths);
    }

    private Mesh mesh(Reader reader, int type) throws IOException {
        Group transform = new Group();
        node(transform, reader);
        VertexBuffer buffer = (VertexBuffer) reference(reader);
        int count = reader.length();
        if (count > reader.remaining() / 8) {
            throw new IOException("M3G submesh count");
        }
        IndexBuffer[] surfaces = new IndexBuffer[count];
        Appearance[] looks = new Appearance[count];
        for (int i = 0; i < count; i++) {
            surfaces[i] = (IndexBuffer) reference(reader);
            looks[i] = (Appearance) reference(reader);
        }
        Mesh mesh;
        if (type == 15) {
            mesh = morphingMesh(reader, buffer, surfaces, looks);
        } else if (type == 16) {
            mesh = skinnedMesh(reader, buffer, surfaces, looks);
        } else {
            mesh = new Mesh(buffer, surfaces, looks);
        }
        transform.copyNodeFields(mesh, new java.util.Hashtable());
        return mesh;
    }

    private MorphingMesh morphingMesh(Reader reader, VertexBuffer buffer,
                                      IndexBuffer[] surfaces, Appearance[] looks)
            throws IOException {
        int count = reader.length();
        if (count == 0 || count > reader.remaining() / 8 || count * 8 != reader.remaining()) {
            throw new IOException("M3G morph target count");
        }
        VertexBuffer[] targets = new VertexBuffer[count];
        float[] weights = new float[count];
        for (int i = 0; i < count; i++) {
            Object3D target = reference(reader);
            if (!(target instanceof VertexBuffer)) throw new IOException("M3G morph target reference");
            targets[i] = (VertexBuffer) target;
            weights[i] = reader.scalar();
        }
        MorphingMesh mesh = new MorphingMesh(buffer, targets, surfaces, looks);
        mesh.setWeights(weights);
        return mesh;
    }

    private SkinnedMesh skinnedMesh(Reader reader, VertexBuffer buffer,
                                    IndexBuffer[] surfaces, Appearance[] looks)
            throws IOException {
        Object3D skeleton = reference(reader);
        if (!(skeleton instanceof Group)) {
            throw new IOException("M3G skeleton reference");
        }
        int count = reader.length();
        if (count > reader.remaining() / 16 || count * 16 != reader.remaining()) {
            throw new IOException("M3G bone reference count");
        }
        try {
            SkinnedMesh mesh = new SkinnedMesh(buffer, surfaces, looks, (Group) skeleton);
            for (int i = 0; i < count; i++) {
                Object3D bone = reference(reader);
                int firstVertex = reader.length();
                int vertexCount = reader.length();
                int weight = reader.intValue();
                if (!(bone instanceof Node)) {
                    throw new IOException("M3G bone reference");
                }
                mesh.addTransform((Node) bone, weight, firstVertex, vertexCount);
            }
            return mesh;
        } catch (IllegalArgumentException invalid) {
            throw new IOException("M3G skinning parameters");
        } catch (IndexOutOfBoundsException invalid) {
            throw new IOException("M3G bone vertex range");
        } catch (ArithmeticException invalid) {
            throw new IOException("M3G bone bind transform");
        }
    }

    private KeyframeSequence keyframes(Reader reader) throws IOException {
        int interpolation = reader.byteValue();
        int repeat = reader.byteValue();
        int encoding = reader.byteValue();
        int duration = reader.length();
        int first = reader.length(), last = reader.length();
        int components = reader.length(), frames = reader.length();
        if (encoding > 2 || frames > reader.remaining()) {
            throw new IOException("M3G keyframe format");
        }
        KeyframeSequence sequence = new KeyframeSequence(frames, components, interpolation);
        sequence.setDuration(duration);
        sequence.setRepeatMode(repeat);
        sequence.setValidRange(first, last);
        float[] bias = new float[components], scale = new float[components];
        if (encoding != 0) {
            for (int i = 0; i < components; i++) {
                bias[i] = reader.scalar();
            }
            for (int i = 0; i < components; i++) {
                scale[i] = reader.scalar() / (encoding == 1 ? 255 : 65535);
            }
        }
        float[] values = new float[components];
        for (int frame = 0; frame < frames; frame++) {
            int time = reader.length();
            for (int i = 0; i < components; i++) {
                values[i] = encoding == 0 ? reader.scalar() :
                    bias[i] + scale[i] * (encoding == 1 ?
                        reader.byteValue() : reader.shortValue());
            }
            sequence.setKeyframe(frame, time, values);
        }
        return sequence;
    }

    private static final class Chunk {
        final int type;
        final Reader reader;

        Chunk(int type, Reader reader) {
            this.type = type;
            this.reader = reader;
        }
    }

    private static final class Reader {
        final byte[] data;
        final int end;
        int offset;

        Reader(byte[] data, int offset, int end) throws IOException {
            if (offset < 0 || offset > end || end > data.length) {
                throw new IOException("M3G object range");
            }
            this.data = data;
            this.offset = offset;
            this.end = end;
        }

        int remaining() {
            return end - offset;
        }

        void skip(int count) throws IOException {
            if (count < 0 || count > remaining()) {
                throw new IOException("truncated M3G data");
            }
            offset += count;
        }

        Reader slice(int length) throws IOException {
            int start = offset;
            skip(length);
            return new Reader(data, start, offset);
        }

        int byteValue() throws IOException {
            if (offset == end) {
                throw new IOException("truncated M3G data");
            }
            return data[offset++] & 255;
        }

        boolean bool() throws IOException {
            return byteValue() != 0;
        }

        int shortValue() throws IOException {
            return byteValue() | byteValue() << 8;
        }

        int intValue() throws IOException {
            return shortValue() | shortValue() << 16;
        }

        int length() throws IOException {
            int value = intValue();
            if (value < 0) {
                throw new IOException("M3G length");
            }
            return value;
        }

        int index(int format) throws IOException {
            if (format == 0) return length();
            if (format == 1) return byteValue();
            if (format == 2) return shortValue();
            throw new IOException("M3G index format");
        }

        int[] indices() throws IOException {
            int count = length();
            if (count > remaining() / 4) {
                throw new IOException("M3G array length");
            }
            int[] result = new int[count];
            for (int i = 0; i < count; i++) {
                result[i] = length();
            }
            return result;
        }

        float scalar() throws IOException {
            return Float.intBitsToFloat(intValue());
        }

        int rgb() throws IOException {
            return byteValue() << 16 | byteValue() << 8 | byteValue();
        }

        int rgba() throws IOException {
            return rgb() | byteValue() << 24;
        }

        Transform transform() throws IOException {
            float[] array = new float[16];
            for (int i = 0; i < array.length; i++) {
                array[i] = scalar();
            }
            Transform value = new Transform();
            value.set(array);
            return value;
        }

        String string() throws IOException {
            int start = offset;
            while (byteValue() != 0) {
            }
            return new String(data, start, offset - start - 1, "UTF-8");
        }
    }
}
