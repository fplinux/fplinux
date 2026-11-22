// SPDX-License-Identifier: GPL-2.0-only
package javax.microedition.sensor;

public interface Condition {
    String OP_EQUALS = "eq";
    String OP_GREATER_THAN = "gt";
    String OP_GREATER_THAN_OR_EQUALS = "ge";
    String OP_LESS_THAN = "lt";
    String OP_LESS_THAN_OR_EQUALS = "le";

    boolean isMet(double value);

    boolean isMet(Object value);
}
