/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * fplinux-phoneme LCDUI look and feel: the natives of Form and TextBox peers
 * (port/lf-form/fplinux-pm-lf-form.c), which map onto libfplinux-ui forms and
 * text boxes. A screen is the peer handle of DisplayableLFImpl.nativeId; an
 * item is its index, which always equals the item's index in the Form,
 * because an item without a native counterpart takes a hidden placeholder.
 * Without the runtime (headless) every native does nothing and returns 0,
 * -1, false or null, so the Java model stays the only state.
 */
package javax.microedition.lcdui;

final class FormPeer {

    /* PEER_CHANGED subtypes (FACTORY.md section 4). */
    static final int PEER_ITEM_CHANGED = 1;
    /* An item command: index = item, hint = command id (its slot in Item.commands). */
    static final int PEER_ITEM_COMMAND = 16;

    /* enum fplinux_ui_text_constraint */
    static final int TEXT_ANY = 0;
    static final int TEXT_NUMERIC = 1;
    static final int TEXT_DECIMAL = 2;
    static final int TEXT_PHONE = 3;
    static final int TEXT_PASSWORD = 0x10000;
    static final int TEXT_UNEDITABLE = 0x20000;

    /* enum fplinux_ui_choice_kind */
    static final int CHOICE_EXCLUSIVE = 0;
    static final int CHOICE_MULTIPLE = 1;
    static final int CHOICE_POPUP = 2;

    /* enum fplinux_ui_date_mode */
    static final int DATE = 0;
    static final int TIME = 1;
    static final int DATE_TIME = 2;

    private FormPeer() {
    }

    /**
     * The runtime constraint of a MIDP TextField constraint. EMAILADDR and
     * URL take ANY; SENSITIVE, NON_PREDICTIVE and the INITIAL_CAPS modifiers
     * have no runtime counterpart.
     */
    static int textConstraint(int constraints) {
        int type;
        switch (constraints & TextField.CONSTRAINT_MASK) {
        case TextField.NUMERIC:
            type = TEXT_NUMERIC;
            break;
        case TextField.DECIMAL:
            type = TEXT_DECIMAL;
            break;
        case TextField.PHONENUMBER:
            type = TEXT_PHONE;
            break;
        default:
            type = TEXT_ANY;
            break;
        }
        if ((constraints & TextField.PASSWORD) != 0) {
            type |= TEXT_PASSWORD;
        }
        if ((constraints & TextField.UNEDITABLE) != 0) {
            type |= TEXT_UNEDITABLE;
        }
        return type;
    }

    static void log(String text) {
        System.out.println("[lcdui] form: " + text);
    }

    /* Screens: an unstacked peer, 0 headless or on failure. */
    static native int createForm0(String title);
    static native int createTextBox0(String title, String text, int maxChars,
                                     int constraint);

    /* Items: each append returns the new item's index, -1 on failure. */
    static native int appendString0(int form, String label, String text);
    static native int appendTextField0(int form, String label, String text,
                                       int maxChars, int constraint);
    static native int appendChoice0(int form, String label, int kind,
                                    String[] choices, int count);
    static native int appendGauge0(int form, String label, int minimum,
                                   int maximum, int value,
                                   boolean interactive);
    /* Wall-clock time in the system time zone; month 0-11. */
    static native int appendDate0(int form, String label, int mode, int year,
                                  int month, int day, int hour, int minute);
    static native int appendImage0(int form, String label,
                                   ImageData image);
    /* A hidden empty item that keeps the indices of an unsupported item. */
    static native int appendPlaceholder0(int form);

    static native boolean moveItem0(int form, int from, int to);
    static native void deleteItem0(int form, int index);
    static native void deleteAll0(int form);
    static native int getFocus0(int form);
    static native void setFocus0(int form, int index);
    /* Item.PLAIN, HYPERLINK and BUTTON equal enum fplinux_ui_appearance. */
    static native boolean setAppearance0(int form, int index, int appearance);
    /* type is a LFNative.CMD_* value. */
    static native boolean addItemCommand0(int form, int index, int id,
                                          int type, String label,
                                          int priority);
    static native void removeItemCommand0(int form, int index, int id);

    static native boolean setString0(int form, int index, String text);
    static native String getText0(int form, int index);
    static native boolean setText0(int form, int index, String text);
    static native boolean setConstraint0(int form, int index, int constraint);
    static native int getChoice0(int form, int index);
    static native void setChoice0(int form, int index, int choice);
    static native boolean isChosen0(int form, int index, int choice);
    static native void setChosen0(int form, int index, int choice,
                                  boolean chosen);
    static native int getValue0(int form, int index);
    static native void setValue0(int form, int index, int value);
    /* Fills year, month (0-11), day, hour and minute; false without a value. */
    static native boolean getDate0(int form, int index, int[] fields);
    static native void setDate0(int form, int index, int year, int month,
                                int day, int hour, int minute);

    static native String getBoxText0(int box);
    static native boolean setBoxText0(int box, String text);
    static native boolean setBoxConstraint0(int box, int constraint);
}
