// SPDX-License-Identifier: GPL-2.0-only
package javax.microedition.sensor;

public interface ChannelInfo {
    int TYPE_DOUBLE = 1;
    int TYPE_INT = 2;
    int TYPE_OBJECT = 4;

    String getName();

    int getDataType();

    int getScale();

    Unit getUnit();

    float getAccuracy();

    MeasurementRange[] getMeasurementRanges();
}
