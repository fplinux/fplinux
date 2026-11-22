// SPDX-License-Identifier: GPL-2.0-only
package javax.microedition.sensor;

/** Sensor discovery; this runtime has no sensors, so nothing is found. */
public final class SensorManager {
    private static final String SCHEME = "sensor:";

    private SensorManager() {
    }

    public static SensorInfo[] findSensors(String quantity, String contextType) {
        checkContextType(contextType);
        return new SensorInfo[0];
    }

    public static SensorInfo[] findSensors(String url) {
        if (url == null) {
            throw new NullPointerException("null sensor URL");
        }
        if (!url.startsWith(SCHEME)) {
            throw new IllegalArgumentException("Invalid sensor URL");
        }

        int start = SCHEME.length();
        int end = endOfPart(url, start);
        checkIdentifier(url.substring(start, end));
        boolean hasContext = false;
        boolean hasModel = false;
        boolean hasLocation = false;
        while (end < url.length()) {
            start = end + 1;
            end = endOfPart(url, start);
            int equals = url.indexOf('=', start);
            if (equals < start || equals >= end) {
                throw new IllegalArgumentException("Invalid sensor parameter");
            }
            String name = url.substring(start, equals);
            String value = url.substring(equals + 1, end);
            checkIdentifier(value);
            if ("contextType".equals(name) && !hasContext) {
                checkContextType(value);
                hasContext = true;
            } else if ("model".equals(name) && !hasModel) {
                hasModel = true;
            } else if ("location".equals(name) && !hasLocation) {
                hasLocation = true;
            } else {
                throw new IllegalArgumentException("Invalid sensor parameter: " + name);
            }
        }
        return new SensorInfo[0];
    }

    private static int endOfPart(String url, int start) {
        int separator = url.indexOf(';', start);
        return separator < 0 ? url.length() : separator;
    }

    private static void checkIdentifier(String value) {
        if (value.length() == 0) {
            throw new IllegalArgumentException("Missing sensor identifier");
        }
        for (int i = 0; i < value.length(); i++) {
            char ch = value.charAt(i);
            if (!Character.isDigit(ch) && !Character.isLowerCase(ch) &&
                !Character.isUpperCase(ch) && "-_.!~*'()".indexOf(ch) < 0) {
                throw new IllegalArgumentException("Invalid sensor identifier");
            }
        }
    }

    private static void checkContextType(String contextType) {
        if (contextType != null &&
            !SensorInfo.CONTEXT_TYPE_AMBIENT.equals(contextType) &&
            !SensorInfo.CONTEXT_TYPE_DEVICE.equals(contextType) &&
            !SensorInfo.CONTEXT_TYPE_USER.equals(contextType) &&
            !SensorInfo.CONTEXT_TYPE_VEHICLE.equals(contextType)) {
            throw new IllegalArgumentException("Invalid sensor context type");
        }
    }

    public static void addSensorListener(SensorListener listener,
                                         SensorInfo info) {
        if (listener == null || info == null) {
            throw new NullPointerException("null sensor listener or info");
        }
        throw new IllegalArgumentException("Unknown sensor");
    }

    /** A quantity can be monitored even when no matching sensors exist. */
    public static void addSensorListener(SensorListener listener,
                                         String quantity) {
        if (listener == null || quantity == null) {
            throw new NullPointerException("null sensor listener or quantity");
        }
    }

    public static void removeSensorListener(SensorListener listener) {
        if (listener == null) {
            throw new NullPointerException("null sensor listener");
        }
    }
}
