// SPDX-License-Identifier: GPL-2.0-only
package javax.wireless.messaging;

import java.io.IOException;
import java.io.InterruptedIOException;
import javax.microedition.io.Connection;

/**
 * Wireless Messaging API connection. Opening an SMS or CBS URL in this
 * runtime throws ConnectionNotFoundException.
 */
public interface MessageConnection extends Connection {
    /** Type of a message created by newMessage() for text. */
    String TEXT_MESSAGE = "text";

    /** Type of a message created by newMessage() for binary data. */
    String BINARY_MESSAGE = "binary";

    Message newMessage(String type);

    Message newMessage(String type, String address);

    void send(Message msg) throws IOException, InterruptedIOException;

    Message receive() throws IOException, InterruptedIOException;

    void setMessageListener(MessageListener l) throws IOException;

    int numberOfSegments(Message msg);
}
