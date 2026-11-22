// SPDX-License-Identifier: GPL-2.0-only
package javax.microedition.sensor;

public interface DataListener {
    void dataReceived(SensorConnection sensor, Data[] data, boolean isDataLost);
}
