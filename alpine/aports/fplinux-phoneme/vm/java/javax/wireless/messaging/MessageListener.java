// SPDX-License-Identifier: GPL-2.0-only
package javax.wireless.messaging;

/** Receives a notification that a message arrived on a connection. */
public interface MessageListener {
    void notifyIncomingMessage(MessageConnection conn);
}
