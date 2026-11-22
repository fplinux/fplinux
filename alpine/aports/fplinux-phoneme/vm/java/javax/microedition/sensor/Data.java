// SPDX-License-Identifier: GPL-2.0-only
package javax.microedition.sensor;

public interface Data {
    ChannelInfo getChannelInfo();

    int[] getIntValues();

    double[] getDoubleValues();

    Object[] getObjectValues();

    long getTimestamp(int index);

    float getUncertainty(int index);

    boolean isValid(int index);
}
