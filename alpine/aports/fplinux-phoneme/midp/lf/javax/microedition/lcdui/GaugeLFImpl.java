/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * fplinux-phoneme LCDUI look and feel: Gauge as a form gauge (interactive:
 * Left/Right change it) or a progress bar. The runtime has no busy state in
 * forms, so INDEFINITE is approximated with a bar of INDEFINITE_STEPS:
 * empty while idle, full while running, and one step further on each
 * INCREMENTAL_UPDATING. A changed maximum rebuilds the row. In an Alert the
 * gauge is the alert's indicator (AlertLFImpl).
 */
package javax.microedition.lcdui;

class GaugeLFImpl extends ItemLFImpl implements GaugeLF {

    private static final int INDEFINITE_STEPS = 10;

    Gauge gauge;

    /* The bar position shown for INCREMENTAL_UPDATING. */
    private int incrementalStep;

    GaugeLFImpl(Gauge g) {
        super(g);
        gauge = g;
    }

    private int indefinitePosition() {
        switch (gauge.value) {
        case Gauge.CONTINUOUS_RUNNING:
            return INDEFINITE_STEPS;
        case Gauge.INCREMENTAL_UPDATING:
            return incrementalStep;
        default:
            return 0;
        }
    }

    int lAppendPeer(int form) {
        if (gauge.maxValue == Gauge.INDEFINITE) {
            return FormPeer.appendGauge0(form, item.label, 0, INDEFINITE_STEPS,
                                         indefinitePosition(), false);
        }
        return FormPeer.appendGauge0(form, item.label, 0, gauge.maxValue,
                                     gauge.value, gauge.interactive);
    }

    boolean lSyncFromPeer(int form, int index) {
        if (!gauge.interactive) {
            return false;
        }
        int value = FormPeer.getValue0(form, index);
        if (value < 0 || value > gauge.maxValue || value == gauge.value) {
            return false;
        }
        gauge.value = value;
        return true;
    }

    public void lSetValue(int oldValue, int value) {
        if (gauge.owner instanceof Alert) {
            AlertLFImpl.lIndicatorChanged(gauge);
            return;
        }
        int form = lPeer();
        int index = lIndex();
        if (form == 0 || index < 0) {
            return;
        }
        if (gauge.maxValue == Gauge.INDEFINITE) {
            if (value == Gauge.INCREMENTAL_UPDATING) {
                incrementalStep = incrementalStep % INDEFINITE_STEPS + 1;
            }
            FormPeer.setValue0(form, index, indefinitePosition());
        } else {
            FormPeer.setValue0(form, index, value);
        }
    }

    public void lSetMaxValue(int oldMaxValue, int maxValue) {
        if (gauge.owner instanceof Alert) {
            AlertLFImpl.lIndicatorChanged(gauge);
            return;
        }
        lRebuildPeer(false);
    }

    public int lGetValue() {
        lSync();
        return gauge.value;
    }
}
