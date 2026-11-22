// SPDX-License-Identifier: GPL-2.0-only
package javax.bluetooth;

/** Receives the results of a device inquiry and of a service search. */
public interface DiscoveryListener {
    int INQUIRY_COMPLETED = 0x00;
    int SERVICE_SEARCH_COMPLETED = 0x01;
    int SERVICE_SEARCH_TERMINATED = 0x02;
    int SERVICE_SEARCH_ERROR = 0x03;
    int SERVICE_SEARCH_NO_RECORDS = 0x04;
    int INQUIRY_TERMINATED = 0x05;
    int SERVICE_SEARCH_DEVICE_NOT_REACHABLE = 0x06;
    int INQUIRY_ERROR = 0x07;

    void deviceDiscovered(RemoteDevice btDevice, DeviceClass cod);

    void servicesDiscovered(int transID, ServiceRecord[] servRecord);

    void serviceSearchCompleted(int transID, int respCode);

    void inquiryCompleted(int discType);
}
