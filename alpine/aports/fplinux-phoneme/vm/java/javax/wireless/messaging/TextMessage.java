// SPDX-License-Identifier: GPL-2.0-only
package javax.wireless.messaging;

/** A message whose payload is text. */
public interface TextMessage extends Message {
    /** The current text, or null when unset. */
    String getPayloadText();

    /** Sets the text, which may be null. */
    void setPayloadText(String data);
}
