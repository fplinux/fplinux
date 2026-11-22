// SPDX-License-Identifier: GPL-2.0-only
package javax.microedition.sensor;

public class MeasurementRange {
    private final double smallest;
    private final double largest;
    private final double resolution;

    public MeasurementRange(double smallest, double largest, double resolution) {
        if (smallest > largest || resolution < 0) {
            throw new IllegalArgumentException("Invalid measurement range");
        }
        this.smallest = smallest;
        this.largest = largest;
        this.resolution = resolution;
    }

    public double getSmallestValue() {
        return smallest;
    }

    public double getLargestValue() {
        return largest;
    }

    public double getResolution() {
        return resolution;
    }
}
