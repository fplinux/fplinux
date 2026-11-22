// SPDX-License-Identifier: GPL-2.0-only
package javax.bluetooth;

/** The 24-bit class-of-device record of a Bluetooth device. */
public class DeviceClass {
    private static final int SERVICE_MASK = 0xFFE000;
    private static final int MAJOR_MASK = 0x1F00;
    private static final int MINOR_MASK = 0xFC;

    private final int record;

    /** The record must not use the 8 upper bits of the int. */
    public DeviceClass(int record) {
        if ((record & 0xFF000000) != 0) {
            throw new IllegalArgumentException("class of device bits 24 to 31");
        }
        this.record = record;
    }

    /** The service class bits, still in place in the record. */
    public int getServiceClasses() {
        return record & SERVICE_MASK;
    }

    /** The major device class bits, still in place in the record. */
    public int getMajorDeviceClass() {
        return record & MAJOR_MASK;
    }

    /** The minor device class bits, still in place in the record. */
    public int getMinorDeviceClass() {
        return record & MINOR_MASK;
    }
}
