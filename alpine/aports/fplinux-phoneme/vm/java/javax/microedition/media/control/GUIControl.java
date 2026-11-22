// SPDX-License-Identifier: GPL-2.0-only
package javax.microedition.media.control;

import javax.microedition.media.Control;

/** A Control that shows media in a user interface component. */
public interface GUIControl extends Control {
    int USE_GUI_PRIMITIVE = 0;

    Object initDisplayMode(int mode, Object arg);
}
