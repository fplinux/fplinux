// SPDX-License-Identifier: GPL-2.0-only
package javax.microedition.sensor;

public interface SensorListener {
    void sensorAvailable(SensorInfo info);

    void sensorUnavailable(SensorInfo info);
}
