// SPDX-License-Identifier: GPL-2.0-only
package javax.microedition.sensor;

import java.util.Hashtable;

/** A unit of measurement, named by its symbol. */
public class Unit {
    private static final Hashtable units = new Hashtable();
    private final String symbol;

    private Unit(String symbol) {
        this.symbol = symbol;
    }

    public static synchronized Unit getUnit(String symbol) {
        if (symbol == null) {
            throw new NullPointerException("null unit symbol");
        }
        if (symbol.length() == 0) {
            throw new IllegalArgumentException("Empty unit symbol");
        }
        Unit unit = (Unit)units.get(symbol);
        if (unit == null) {
            unit = new Unit(symbol);
            units.put(symbol, unit);
        }
        return unit;
    }

    public String toString() {
        return symbol;
    }
}
