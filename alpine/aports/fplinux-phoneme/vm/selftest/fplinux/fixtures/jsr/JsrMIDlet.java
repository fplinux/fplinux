// SPDX-License-Identifier: GPL-2.0-only
package fplinux.fixtures.jsr;

import java.io.ByteArrayInputStream;
import java.io.IOException;
import javax.microedition.io.CommConnection;
import javax.microedition.io.ConnectionNotFoundException;
import javax.microedition.io.Connector;
import javax.microedition.io.PushRegistry;
import javax.microedition.media.Manager;
import javax.microedition.media.MediaException;
import javax.microedition.media.Player;
import javax.microedition.media.control.MIDIControl;
import javax.microedition.media.control.VideoControl;
import javax.microedition.midlet.MIDlet;
import javax.microedition.pim.PIM;
import javax.microedition.pim.PIMException;
import javax.microedition.sensor.MeasurementRange;
import javax.microedition.sensor.SensorInfo;
import javax.microedition.sensor.SensorListener;
import javax.microedition.sensor.SensorManager;
import javax.microedition.sensor.Unit;

public class JsrMIDlet extends MIDlet {
    private static final String DATAGRAM = "datagram://:1234";
    private static final String DECLARED = "fplinux.fixtures.jsr.JsrMIDlet$Declared";
    private static final String UNDECLARED = "fplinux.fixtures.jsr.JsrMIDlet$Undeclared";
    private static boolean initialized;

    protected void startApp() {
        try {
            push();
            mediaAndSerial();
            pim();
            sensors();
            System.out.println("PM-JSR done");
        } catch (Exception e) {
            e.printStackTrace();
        } finally {
            notifyDestroyed();
        }
    }

    protected void pauseApp() {
    }

    protected void destroyApp(boolean unconditional) {
    }

    private static void require(boolean condition, String failure) {
        if (!condition) {
            throw new RuntimeException(failure);
        }
    }

    private static void push() throws IOException, ClassNotFoundException {
        try {
            PushRegistry.registerConnection(DATAGRAM, DECLARED, "*");
            throw new RuntimeException("Registered an unsupported push connection");
        } catch (ConnectionNotFoundException expected) {
        }
        try {
            PushRegistry.registerAlarm(DECLARED, System.currentTimeMillis() + 10000);
            throw new RuntimeException("Registered an unsupported alarm");
        } catch (ConnectionNotFoundException expected) {
        }
        try {
            PushRegistry.registerAlarm(UNDECLARED, System.currentTimeMillis());
            throw new RuntimeException("Accepted a MIDlet absent from the manifest");
        } catch (ClassNotFoundException expected) {
        }
        try {
            PushRegistry.registerConnection(DATAGRAM, null, "*");
            throw new RuntimeException("Accepted a null MIDlet name");
        } catch (ClassNotFoundException expected) {
        }
        try {
            PushRegistry.registerConnection(DATAGRAM, DECLARED, null);
            throw new RuntimeException("Accepted a null push filter");
        } catch (IllegalArgumentException expected) {
        }
        require(!initialized, "Push registration initialized a MIDlet class");
        require(PushRegistry.listConnections(false).length == 0 &&
                PushRegistry.listConnections(true).length == 0 &&
                !PushRegistry.unregisterConnection(DATAGRAM) &&
                PushRegistry.getMIDlet(DATAGRAM) == null &&
                PushRegistry.getFilter(DATAGRAM) == null,
                "Failed push registration changed registry state");
        System.out.println("PM-JSR push unavailable for declared MIDlets");
    }

    private static void mediaAndSerial() throws IOException, MediaException {
        String ports = System.getProperty("microedition.commports");
        require(ports == null || ports.length() == 0,
                "Unexpected serial ports: " + ports);
        try {
            CommConnection serial = (CommConnection)Connector.open("comm:COM1");
            serial.close();
            throw new RuntimeException("Opened an unsupported serial port");
        } catch (ConnectionNotFoundException expected) {
        }

        Player player = Manager.createPlayer(Manager.TONE_DEVICE_LOCATOR);
        try {
            player.realize();
            VideoControl video = (VideoControl)player.getControl("VideoControl");
            require(video == null &&
                    player.getControl("javax.microedition.media.control.VideoControl") == null,
                    "Tone player exposed a video control");
            // Games cast the answer before testing it for null, and the cast
            // needs the class even then.
            MIDIControl midi = (MIDIControl)player.getControl("MIDIControl");
            require(midi == null && MIDIControl.NOTE_ON == 0x90 &&
                    MIDIControl.CONTROL_CHANGE == 0xB0,
                    "Tone player exposed a MIDI control");
        } finally {
            player.close();
        }
        System.out.println("PM-JSR serial, video and MIDI controls unavailable");
    }

    private static void pim() {
        PIM pim = PIM.getInstance();
        require(pim.listPIMLists(PIM.CONTACT_LIST).length == 0 &&
                pim.supportedSerialFormats(PIM.CONTACT_LIST).length == 0,
                "Unexpected PIM contact lists or formats");
        try {
            pim.openPIMList(PIM.CONTACT_LIST, PIM.READ_ONLY);
            throw new RuntimeException("Opened an unsupported contact list");
        } catch (PIMException e) {
            require(e.getReason() == 3, "Expected LIST_NOT_ACCESSIBLE");
        }
        try {
            pim.fromSerialFormat(new ByteArrayInputStream(new byte[0]), null);
            throw new RuntimeException("Accepted an unsupported serial format");
        } catch (PIMException e) {
            require(e.getReason() == 0, "Expected FEATURE_NOT_SUPPORTED");
        } catch (IOException e) {
            throw new RuntimeException("Unexpected encoding error");
        }
        System.out.println("PM-JSR PIM lists and formats unavailable");
    }

    private static void sensors() throws IOException {
        require(SensorManager.findSensors(null, null).length == 0 &&
                SensorManager.findSensors("sensor:acceleration;contextType=device").length == 0,
                "Unexpected sensors");
        try {
            SensorManager.findSensors("sensor:acceleration;bad");
            throw new RuntimeException("Accepted a malformed sensor URL");
        } catch (IllegalArgumentException expected) {
        }
        try {
            Connector.open("sensor:acceleration").close();
            throw new RuntimeException("Opened an unsupported sensor");
        } catch (ConnectionNotFoundException expected) {
        }
        SensorListener listener = new SensorListener() {
            public void sensorAvailable(SensorInfo info) {
                throw new RuntimeException("Unexpected available sensor");
            }

            public void sensorUnavailable(SensorInfo info) {
                throw new RuntimeException("Unexpected unavailable sensor");
            }
        };
        SensorManager.addSensorListener(listener, "acceleration");
        SensorManager.removeSensorListener(listener);
        Unit unit = Unit.getUnit("m/s^2");
        require(unit == Unit.getUnit(new String("m/s^2")) &&
                "m/s^2".equals(unit.toString()), "Measurement unit identity differs");
        MeasurementRange range = new MeasurementRange(-2.0, 2.0, 0.5);
        require(range.getSmallestValue() == -2.0 &&
                range.getLargestValue() == 2.0 && range.getResolution() == 0.5,
                "Measurement range values differ");
        try {
            new MeasurementRange(2.0, -2.0, 0.5);
            throw new RuntimeException("Accepted a reversed measurement range");
        } catch (IllegalArgumentException expected) {
        }
        System.out.println("PM-JSR sensor discovery empty, range and unit checked");
    }

    public static final class Declared extends JsrMIDlet {
        static {
            initialized = true;
        }
    }

    public static final class Undeclared extends JsrMIDlet {
        static {
            initialized = true;
        }
    }
}
