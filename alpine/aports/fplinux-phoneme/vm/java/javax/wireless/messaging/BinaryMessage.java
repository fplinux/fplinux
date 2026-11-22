// SPDX-License-Identifier: GPL-2.0-only
package javax.wireless.messaging;

/** A message whose payload is a byte array. */
public interface BinaryMessage extends Message {
    /** The current payload array, or null when unset. */
    byte[] getPayloadData();

    /** Retains the supplied array reference, which may be null. */
    void setPayloadData(byte[] data);
}
