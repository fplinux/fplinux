// SPDX-License-Identifier: GPL-2.0-only
package javax.bluetooth;

import java.io.IOException;

/**
 * Service discovery record of a service on a local or a remote device.
 */
public interface ServiceRecord {
    int NOAUTHENTICATE_NOENCRYPT = 0x00;
    int AUTHENTICATE_NOENCRYPT = 0x01;
    int AUTHENTICATE_ENCRYPT = 0x02;

    DataElement getAttributeValue(int attrID);

    RemoteDevice getHostDevice();

    int[] getAttributeIDs();

    boolean populateRecord(int[] attrIDs) throws IOException;

    String getConnectionURL(int requiredSecurity, boolean mustBeMaster);

    void setDeviceServiceClasses(int classes);

    boolean setAttributeValue(int attrID, DataElement attrValue);
}
