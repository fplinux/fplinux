// SPDX-License-Identifier: GPL-2.0-only
package javax.microedition.m3g;

import java.util.Hashtable;
import java.util.Vector;

public class VertexBuffer extends Object3D {
    VertexArray positions, normals, colors;
    final VertexArray[] texCoords = new VertexArray[2];
    final float[] position = { 1, 0, 0, 0 };
    final float[][] texTransform = {
        { 1, 0, 0, 0 },
        { 1, 0, 0, 0 },
    };
    int defaultColor = 0xffffffff;

    public int getVertexCount() {
        return positions == null ? 0 : positions.vertices;
    }

    public void setDefaultColor(int argb) {
        defaultColor = argb;
    }

    public int getDefaultColor() {
        return defaultColor;
    }

    public void setColors(VertexArray values) {
        if (values != null && ((values.components != 3 && values.components != 4) ||
                values.componentSize != 1)) {
            throw new IllegalArgumentException("color format");
        }
        checkVertexCount(-3, values);
        colors = values;
    }

    public VertexArray getColors() {
        return colors;
    }

    public VertexArray getNormals() {
        return normals;
    }

    // Fills scaleBias with the scale followed by the bias of each coordinate.
    public VertexArray getPositions(float[] scaleBias) {
        if (positions != null) copyScaleBias(position, scaleBias, 3);
        return positions;
    }

    public VertexArray getTexCoords(int index, float[] scaleBias) {
        VertexArray coordinates = texCoords[index];
        if (coordinates != null) copyScaleBias(texTransform[index], scaleBias, coordinates.components);
        return coordinates;
    }

    private static void copyScaleBias(float[] source, float[] target, int dimensions) {
        if (target == null) return;
        if (target.length < 1 + dimensions) throw new IllegalArgumentException("scale and bias");
        System.arraycopy(source, 0, target, 0, 1 + dimensions);
    }

    public void setPositions(VertexArray coordinates, float scale, float[] bias) {
        if (coordinates != null && coordinates.components != 3) {
            throw new IllegalArgumentException("position components");
        }
        checkVertexCount(-1, coordinates);
        setScaleBias(position, scale, bias, 3);
        positions = coordinates;
    }

    public void setTexCoords(int index, VertexArray coordinates, float scale, float[] bias) {
        if (coordinates != null && coordinates.components > 3) {
            throw new IllegalArgumentException("texture components");
        }
        checkVertexCount(index, coordinates);
        setScaleBias(texTransform[index], scale, bias, coordinates == null ? 0 : coordinates.components);
        texCoords[index] = coordinates;
    }

    public void setNormals(VertexArray values) {
        if (values != null && values.getComponentCount() != 3) {
            throw new IllegalArgumentException("normal components");
        }
        checkVertexCount(-2, values);
        normals = values;
    }

    private void checkVertexCount(int slot, VertexArray value) {
        if (value == null) return;
        if ((slot != -1 && differs(positions, value)) || (slot != -3 && differs(colors, value)) ||
                (slot != -2 && differs(normals, value))) {
            throw new IllegalArgumentException("vertex count");
        }
        for (int i = 0; i < texCoords.length; i++) {
            if (i != slot && differs(texCoords[i], value)) {
                throw new IllegalArgumentException("vertex count");
            }
        }
    }

    private static boolean differs(VertexArray other, VertexArray value) {
        return other != null && other.vertices != value.vertices;
    }

    private static void setScaleBias(float[] target, float scale, float[] bias, int dimensions) {
        if (bias != null && bias.length < dimensions) {
            throw new IllegalArgumentException("coordinate bias");
        }
        target[0] = scale;
        for (int i = 0; i < 3; i++) target[i + 1] = bias == null || i >= dimensions ? 0 : bias[i];
    }

    void point(int index, float[] xyz) {
        for (int i = 0; i < 3; i++) {
            xyz[i] = positions.get(index, i) * position[0] + position[i + 1];
        }
    }

    int color(int index) {
        if (colors == null) {
            return defaultColor;
        }
        int alpha = colors.components == 4 ? colors.get(index, 3) & 255 : 255;
        return alpha << 24 | (colors.get(index, 0) & 255) << 16 |
            (colors.get(index, 1) & 255) << 8 | (colors.get(index, 2) & 255);
    }

    void references(Vector objects) {
        super.references(objects);
        addReference(objects, positions);
        addReference(objects, normals);
        addReference(objects, colors);
        for (int i = 0; i < texCoords.length; i++) addReference(objects, texCoords[i]);
    }

    Object3D newCopy() {
        return new VertexBuffer();
    }

    void copyFields(Object3D copy, Hashtable copies) {
        super.copyFields(copy, copies);
        VertexBuffer target = (VertexBuffer) copy;
        target.positions = positions;
        target.normals = normals;
        target.colors = colors;
        target.defaultColor = defaultColor;
        System.arraycopy(position, 0, target.position, 0, position.length);
        System.arraycopy(texCoords, 0, target.texCoords, 0, texCoords.length);
        for (int i = 0; i < texTransform.length; i++) {
            System.arraycopy(texTransform[i], 0, target.texTransform[i], 0, 4);
        }
    }
}
