// SPDX-License-Identifier: GPL-2.0-only
package javax.microedition.sensor;

import java.io.IOException;
import javax.microedition.io.Connection;

public interface SensorConnection extends Connection {
    int STATE_OPENED = 1;
    int STATE_LISTENING = 2;
    int STATE_CLOSED = 4;

    Channel getChannel(ChannelInfo channelInfo);

    SensorInfo getSensorInfo();

    int getState();

    void setDataListener(DataListener listener, int bufferSize);

    void setDataListener(DataListener listener, int bufferSize,
                         long bufferingPeriod, boolean isTimestampIncluded,
                         boolean isUncertaintyIncluded,
                         boolean isValidityIncluded);

    void removeDataListener();

    Data[] getData(int bufferSize) throws IOException;

    Data[] getData(int bufferSize, long bufferingPeriod,
                   boolean isTimestampIncluded, boolean isUncertaintyIncluded,
                   boolean isValidityIncluded) throws IOException;

    int[] getErrorCodes();

    String getErrorText(int errorCode);
}
