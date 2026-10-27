/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * fplinux-phoneme LCDUI look and feel: the natives of the high-level screens
 * (port/lf-screens/). A peer is a libfplinux-ui screen, passed as an int.
 * Every native does nothing, or returns 0 or false, without a runtime (the
 * headless runner), and Java then keeps only the model.
 */
package javax.microedition.lcdui;

final class LFNative {

    /* enum fplinux_ui_command_type, in order. */
    static final int CMD_BACK = 0;
    static final int CMD_CANCEL = 1;
    static final int CMD_EXIT = 2;
    static final int CMD_STOP = 3;
    static final int CMD_OK = 4;
    static final int CMD_ITEM = 5;
    static final int CMD_SCREEN = 6;
    static final int CMD_HELP = 7;
    /** An Alert's own dismiss command: the right soft key, "Close". */
    static final int CMD_DISMISS = 8;

    /* enum fplinux_ui_list_kind. */
    static final int LIST_IMPLICIT = 0;
    static final int LIST_EXCLUSIVE = 1;
    static final int LIST_MULTIPLE = 2;

    /* enum fplinux_ui_alert_kind. */
    static final int ALERT_INFO = 0;
    static final int ALERT_CONFIRMATION = 1;
    static final int ALERT_WARNING = 2;
    static final int ALERT_ERROR = 3;

    /* PEER_CHANGED_EVENT subtypes; 16 and up belong to the Form LF. */
    static final int PEER_ITEM_CHANGED = 1;
    static final int PEER_LIST_SELECT = 2;

    /** True without a runtime connection (the headless runner). */
    static final boolean HEADLESS = headless0();

    /*
     * A traced headless run (FPLINUX_PM_LF_TRACE, FPLINUX_PM_LF_SCRIPT;
     * port/lf-screens/fplinux-pm-lf-trace.c) plays its scripted key
     * presses from this thread.
     */
    static {
        if (traceNext0() >= 0) {
            new Thread() {
                public void run() {
                    int delay;
                    while ((delay = traceNext0()) >= 0) {
                        try {
                            Thread.sleep(delay);
                        } catch (InterruptedException e) {
                            return;
                        }
                        traceRun0();
                    }
                }
            }.start();
        }
    }

    private LFNative() {
    }

    private static native int traceNext0();
    private static native void traceRun0();
    /** Tells a traced run which peer Java shows; nothing otherwise. */
    static native void traceShown0(int screen);

    private static native boolean headless0();

    /* The screen stack; only DisplayableLFImpl calls these. */
    static native boolean replace0(int screen);
    static native boolean push0(int screen);
    static native boolean pop0();
    static native int current0();
    /** Sets the screen's listener and data; false when it cannot. */
    static native boolean attach0(int screen, int serial, boolean alert);
    /** False once the runtime deleted the screen behind Java's back. */
    static native boolean alive0(int screen, int serial);

    static native void setTitle0(int screen, String title);
    static native boolean addCommand0(int screen, int id, int type,
                                      String label, int priority);
    static native void removeCommand0(int screen, int id);
    static native int contentWidth0(int screen);
    static native int contentHeight0(int screen);

    /* List. */
    static native int listNew0(String title, int kind);
    static native void listInsert0(int list, int index, String text);
    static native void listDelete0(int list, int index);
    static native void listDeleteAll0(int list);
    static native void listSet0(int list, int index, String text);
    static native void listSetIcon0(int list, int index, ImageData image);
    static native int listGetFocused0(int list);
    static native void listSetFocused0(int list, int index);
    static native boolean listIsSelected0(int list, int index);
    static native void listSetSelected0(int list, int index,
                                        boolean selected);
    static native void listSetSelectLabel0(int list, String label);

    /* Alert. */
    static native int alertNew0(int kind, String title, String text);
    static native void alertSetText0(int alert, String text);
    static native void alertSetProgress0(int alert, int value, int maximum);
}
