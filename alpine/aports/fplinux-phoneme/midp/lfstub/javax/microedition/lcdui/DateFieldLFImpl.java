/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * fplinux-phoneme headless LCDUI stub: DateField look and feel. There is no
 * editor, so the model value is always current (lGetDate returns null).
 */
package javax.microedition.lcdui;

class DateFieldLFImpl extends ItemLFImpl implements DateFieldLF {

    DateFieldLFImpl(DateField df) {
        super(df);
    }

    public void lSetDate(java.util.Date date) {
    }

    public void lSetInputMode(int mode) {
    }

    public java.util.Date lGetDate() {
        return null;
    }
}
