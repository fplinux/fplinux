// SPDX-License-Identifier: GPL-2.0-only
package javax.wireless.messaging;

import java.util.Date;

/** Address and timestamp of a wireless message. */
public interface Message {
    /** Address of the recipient of a message to send, or of the sender of a received one. */
    String getAddress();

    void setAddress(String addr);

    /** Time the message was sent, or null if the time is unavailable. */
    Date getTimestamp();
}
