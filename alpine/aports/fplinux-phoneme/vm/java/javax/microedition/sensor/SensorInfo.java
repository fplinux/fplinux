// SPDX-License-Identifier: GPL-2.0-only
package javax.microedition.sensor;

public interface SensorInfo {
    int CONN_EMBEDDED = 1;
    int CONN_REMOTE = 2;
    int CONN_SHORT_RANGE_WIRELESS = 4;
    int CONN_WIRED = 8;

    String CONTEXT_TYPE_AMBIENT = "ambient";
    String CONTEXT_TYPE_DEVICE = "device";
    String CONTEXT_TYPE_USER = "user";
    String CONTEXT_TYPE_VEHICLE = "vehicle";

    String PROP_LATITUDE = "latitude";
    String PROP_LOCATION = "location";
    String PROP_LONGITUDE = "longitude";
    String PROP_MAX_RATE = "maxSamplingRate";
    String PROP_VENDOR = "vendor";
    String PROP_VERSION = "version";

    String getQuantity();

    String getContextType();

    int getConnectionType();

    String getModel();

    String getDescription();

    String getUrl();

    int getMaxBufferSize();

    boolean isAvailabilityPushSupported();

    boolean isConditionPushSupported();

    boolean isAvailable();

    ChannelInfo[] getChannelInfos();

    Object getProperty(String name);

    String[] getPropertyNames();
}
