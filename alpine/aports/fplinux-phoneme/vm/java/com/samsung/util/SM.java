// SPDX-License-Identifier: GPL-2.0-only
package com.samsung.util;

public class SM {
    private String destination;
    private String callback;
    private String data;

    public SM() {
    }

    public SM(String dest, String callback, String text) {
        setDestAddress(dest);
        setCallbackAddress(callback);
        setData(text);
    }

    public void setDestAddress(String address) {
        if (address == null || address.length() == 0) {
            throw new IllegalArgumentException("destination address");
        }
        int first = address.charAt(0) == '+' ? 1 : 0;
        if (first == address.length()) {
            throw new IllegalArgumentException("destination address");
        }
        for (int i = first; i < address.length(); i++) {
            char digit = address.charAt(i);
            if (digit < '0' || digit > '9') {
                throw new IllegalArgumentException("destination address");
            }
        }
        destination = address;
    }

    public void setCallbackAddress(String address) {
        callback = address;
    }

    public void setData(String text) {
        if (text != null && text.length() > 80) {
            throw new IllegalArgumentException("message length");
        }
        data = text;
    }

    public String getDestAddress() {
        return destination;
    }

    public String getCallbackAddress() {
        return callback;
    }

    public String getData() {
        return data;
    }
}
