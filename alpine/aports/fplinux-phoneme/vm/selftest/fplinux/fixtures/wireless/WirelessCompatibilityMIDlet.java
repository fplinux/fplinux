// SPDX-License-Identifier: GPL-2.0-only
package fplinux.fixtures.wireless;

import java.io.IOException;
import java.util.Enumeration;
import javax.bluetooth.BluetoothConnectionException;
import javax.bluetooth.BluetoothStateException;
import javax.bluetooth.DataElement;
import javax.bluetooth.DeviceClass;
import javax.bluetooth.LocalDevice;
import javax.bluetooth.RemoteDevice;
import javax.bluetooth.UUID;
import javax.microedition.io.Connection;
import javax.microedition.io.ConnectionNotFoundException;
import javax.microedition.io.Connector;
import javax.microedition.midlet.MIDlet;
import javax.wireless.messaging.MessageConnection;

public class WirelessCompatibilityMIDlet extends MIDlet {
    private static class Peer extends RemoteDevice {
        Peer(String address) {
            super(address);
        }
    }

    private static void require(boolean condition, String check) {
        if (!condition) {
            throw new IllegalStateException(check);
        }
    }

    private static void checkUuids() {
        UUID serialPort = new UUID(0x1101L);
        UUID expanded = new UUID("0000110100001000800000805f9b34fb", false);
        require(serialPort.equals(expanded), "expanded serial port UUID");
        require(serialPort.hashCode() == expanded.hashCode(), "equal UUID hash");
        require(serialPort.equals(new UUID("00001101", true)), "short UUID leading zeros");
        require("110100001000800000805F9B34FB".equals(serialPort.toString()), "full UUID text");
        require(serialPort.equals(new UUID(serialPort.toString(), false)), "UUID text round trip");
        require("FFFFFFFF00001000800000805F9B34FB"
                .equals(new UUID(0xFFFFFFFFL).toString()), "32-bit unsigned UUID");
        require("DEAD0000BEEF0000123456789ABCDEF0"
                .equals(new UUID("dead0000beef0000123456789abcdef0", false).toString()),
                "128-bit UUID");
        require("0".equals(new UUID("0000", false).toString()), "zero UUID");
        try {
            new UUID("0x1101", true);
            throw new IllegalStateException("UUID accepted a prefix");
        } catch (NumberFormatException expected) {
            System.out.println("PM-WIRELESS UUID values");
        }
    }

    private static void requireNoObjectValue(DataElement element) {
        try {
            element.getValue();
            throw new IllegalStateException("object getter accepted a primitive or NULL");
        } catch (ClassCastException expected) {
        }
    }

    private static void checkWideInteger(int type, int length) {
        byte[] input = new byte[length];
        input[0] = (byte) 0xFE;
        input[length - 1] = 7;
        DataElement element = new DataElement(type, input);
        input[0] = 0;
        byte[] output = (byte[]) element.getValue();
        require(output.length == length && output[0] == (byte) 0xFE && output[length - 1] == 7,
                "wide SDP integer value");
        output[length - 1] = 0;
        require(((byte[]) element.getValue())[length - 1] == 7, "wide SDP integer copy");
    }

    private static void checkDataElements() {
        DataElement unsigned = new DataElement(DataElement.U_INT_4, 4294967295L);
        DataElement signed = new DataElement(DataElement.INT_8, Long.MIN_VALUE);
        DataElement flag = new DataElement(true);
        require(unsigned.getDataType() == 10 && unsigned.getLong() == 4294967295L,
                "unsigned SDP integer");
        require(signed.getDataType() == 19 && signed.getLong() == -9223372036854775808L,
                "signed SDP integer");
        require(flag.getDataType() == 40 && flag.getBoolean(), "SDP boolean");
        requireNoObjectValue(unsigned);
        requireNoObjectValue(flag);
        requireNoObjectValue(new DataElement(DataElement.NULL));

        checkWideInteger(DataElement.U_INT_8, 8);
        checkWideInteger(DataElement.U_INT_16, 16);
        checkWideInteger(DataElement.INT_16, 16);
        try {
            new DataElement(DataElement.U_INT_1, 256L);
            throw new IllegalStateException("out-of-range SDP integer");
        } catch (IllegalArgumentException expected) {
        }
        try {
            new DataElement(DataElement.STRING, null);
            throw new IllegalStateException("null SDP string");
        } catch (IllegalArgumentException expected) {
        }

        DataElement name = new DataElement(DataElement.STRING, "hands-free");
        DataElement alternative = new DataElement(DataElement.DATALT);
        alternative.addElement(name);
        DataElement sequence = new DataElement(DataElement.DATSEQ);
        sequence.addElement(alternative);
        sequence.insertElementAt(flag, 0);
        sequence.addElement(flag);
        require(sequence.getSize() == 3 && sequence.removeElement(flag), "SDP sequence mutation");
        require(!sequence.removeElement(new DataElement(true)), "SDP sequence element identity");
        Enumeration elements = (Enumeration) sequence.getValue();
        require(sequence.getSize() == 2 && elements.nextElement() == alternative
                && elements.nextElement() == flag && !elements.hasMoreElements(), "SDP sequence order");
        Enumeration alternatives = (Enumeration) alternative.getValue();
        require(alternatives.nextElement() == name && !alternatives.hasMoreElements()
                && "hands-free".equals(name.getValue()), "nested SDP alternative");
        System.out.println("PM-WIRELESS SDP values");
    }

    private static void checkBluetoothIdentity() {
        DeviceClass device = new DeviceClass(0x5A020F);
        require(device.getServiceClasses() == 0x5A0000 && device.getMajorDeviceClass() == 0x0200
                && device.getMinorDeviceClass() == 0x0C, "Bluetooth class masks");
        Peer lower = new Peer("0023afaa2233");
        Peer upper = new Peer("0023AFAA2233");
        require(upper.getBluetoothAddress().equals(lower.getBluetoothAddress())
                && lower.equals(upper) && lower.hashCode() == upper.hashCode(), "peer identity");
        BluetoothConnectionException failure = new BluetoothConnectionException(
                BluetoothConnectionException.UNACCEPTABLE_PARAMS, "parameters");
        require(failure.getStatus() == 6 && "parameters".equals(failure.getMessage()),
                "Bluetooth connection exception");
        System.out.println("PM-WIRELESS Bluetooth identity");
    }

    private static void requireNoConnection(String url) {
        try {
            Connection connection = Connector.open(url);
            connection.close();
            throw new IllegalStateException("wireless transport opened: " + url);
        } catch (ConnectionNotFoundException expected) {
        } catch (IOException unexpected) {
            throw new IllegalStateException(unexpected.toString());
        }
    }

    private static void checkBluetoothAvailability() {
        require(!LocalDevice.isPowerOn() && LocalDevice.getProperty("bluetooth.api.version") == null,
                "Bluetooth is unavailable to MIDlets");
        try {
            LocalDevice.getLocalDevice();
            throw new IllegalStateException("local Bluetooth device became available");
        } catch (BluetoothStateException expected) {
        }
        requireNoConnection("btspp://0023AFAA2233:1");
        requireNoConnection("btl2cap://0023AFAA2233:1001");
        requireNoConnection("btgoep://0023AFAA2233:1");
        System.out.println("PM-WIRELESS Bluetooth transports unavailable");
    }

    private static void checkMessagingAvailability() {
        try {
            Class.forName("javax.wireless.messaging.Message");
            Class.forName("javax.wireless.messaging.BinaryMessage");
            Class.forName("javax.wireless.messaging.TextMessage");
            Class.forName("javax.wireless.messaging.MessageListener");
        } catch (ClassNotFoundException missing) {
            throw new IllegalStateException(missing.toString());
        }
        try {
            MessageConnection connection = (MessageConnection) Connector.open("sms://+15551234567");
            connection.close();
            throw new IllegalStateException("SMS transport opened");
        } catch (ConnectionNotFoundException expected) {
        } catch (IOException unexpected) {
            throw new IllegalStateException(unexpected.toString());
        }
        requireNoConnection("sms://:5000");
        requireNoConnection("cbs://:50");
        System.out.println("PM-WIRELESS SMS and CBS unavailable");
    }

    protected void startApp() {
        checkUuids();
        checkDataElements();
        checkBluetoothIdentity();
        checkBluetoothAvailability();
        checkMessagingAvailability();
        notifyDestroyed();
    }

    protected void pauseApp() { }

    protected void destroyApp(boolean unconditional) { }
}
