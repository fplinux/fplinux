/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * fplinux-phoneme LCDUI look and feel: DateField as a form date row, which
 * the user edits with the runtime's date editor. The two sides exchange
 * wall-clock fields: the model's Calendar (in the field's time zone) and
 * the runtime's local time, so a time is shown as the MIDlet set it. The
 * runtime has no empty date: a field without a date shows its Calendar's
 * time (the current time) and stays null until the user changes it. A
 * TIME value travels on a fixed day and a DATE value at noon, so neither
 * meets the epoch or a daylight-saving gap; the other fields are dropped
 * as DateField itself does. A new input mode rebuilds the row.
 */
package javax.microedition.lcdui;

import java.util.Calendar;

class DateFieldLFImpl extends ItemLFImpl implements DateFieldLF {

    DateField dateField;

    DateFieldLFImpl(DateField df) {
        super(df);
        dateField = df;
    }

    private int peerMode() {
        switch (dateField.mode) {
        case DateField.TIME:
            return FormPeer.TIME;
        case DateField.DATE_TIME:
            return FormPeer.DATE_TIME;
        default:
            return FormPeer.DATE;
        }
    }

    private int year() {
        return dateField.mode == DateField.TIME
            ? 2000 : dateField.currentDate.get(Calendar.YEAR);
    }

    private int month() {
        return dateField.mode == DateField.TIME
            ? Calendar.JANUARY : dateField.currentDate.get(Calendar.MONTH);
    }

    private int day() {
        return dateField.mode == DateField.TIME
            ? 1 : dateField.currentDate.get(Calendar.DATE);
    }

    private int hour() {
        return dateField.mode == DateField.DATE
            ? 12 : dateField.currentDate.get(Calendar.HOUR_OF_DAY);
    }

    private int minute() {
        return dateField.mode == DateField.DATE
            ? 0 : dateField.currentDate.get(Calendar.MINUTE);
    }

    int lAppendPeer(int form) {
        return FormPeer.appendDate0(form, item.label, peerMode(), year(),
                                    month(), day(), hour(), minute());
    }

    /** Takes the peer's value into the model's Calendar; true if it changed. */
    private boolean lTakePeerDate(int form, int index) {
        int[] f = new int[5];
        if (!FormPeer.getDate0(form, index, f)) {
            return false;
        }
        Calendar c = dateField.currentDate;
        int year = f[0];
        int month = f[1];
        int day = f[2];
        int hour = f[3];
        int minute = f[4];
        if (dateField.mode == DateField.TIME) {
            year = 1970;
            month = Calendar.JANUARY;
            day = 1;
        } else if (dateField.mode == DateField.DATE) {
            hour = 0;
            minute = 0;
        }
        boolean changed = !dateField.initialized
            || c.get(Calendar.YEAR) != year || c.get(Calendar.MONTH) != month
            || c.get(Calendar.DATE) != day
            || c.get(Calendar.HOUR_OF_DAY) != hour
            || c.get(Calendar.MINUTE) != minute;
        if (changed) {
            c.set(Calendar.YEAR, year);
            c.set(Calendar.MONTH, month);
            c.set(Calendar.DATE, day);
            c.set(Calendar.HOUR_OF_DAY, hour);
            c.set(Calendar.MINUTE, minute);
            c.set(Calendar.SECOND, 0);
            c.set(Calendar.MILLISECOND, 0);
            dateField.initialized = true;
        }
        return changed;
    }

    boolean lSyncFromPeer(int form, int index) {
        // The peer of a null date shows a placeholder time, not a value.
        return dateField.initialized && lTakePeerDate(form, index);
    }

    boolean lPeerChanged(int form, int index) {
        lTakePeerDate(form, index);
        return true;
    }

    public void lSetDate(java.util.Date date) {
        int form = lPeer();
        int index = lIndex();
        if (form != 0 && index >= 0 && date != null) {
            FormPeer.setDate0(form, index, year(), month(), day(), hour(),
                              minute());
        }
    }

    public void lSetInputMode(int mode) {
        lRebuildPeer(false);
    }

    public java.util.Date lGetDate() {
        // DateField then copies its Calendar, which lSync() brought up to date.
        lSync();
        return null;
    }
}
