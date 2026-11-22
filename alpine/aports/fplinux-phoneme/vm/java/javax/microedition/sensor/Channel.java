// SPDX-License-Identifier: GPL-2.0-only
package javax.microedition.sensor;

public interface Channel {
    void addCondition(ConditionListener listener, Condition condition);

    ChannelInfo getChannelInfo();

    Condition[] getConditions(ConditionListener listener);

    String getChannelUrl();

    void removeAllConditions();

    void removeCondition(ConditionListener listener, Condition condition);

    void removeConditionListener(ConditionListener listener);
}
