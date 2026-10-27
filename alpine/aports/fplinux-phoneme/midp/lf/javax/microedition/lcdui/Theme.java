/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * fplinux-phoneme headless LCDUI stub: the theme values Display uses when
 * Chameleon is not built (same values as upstream lfplatform Theme).
 */
package javax.microedition.lcdui;

class Theme {
    static int CONTENT_HEIGHT = Font.getDefaultFont().getHeight();
    static final int PREFERRED_IMG_W = 12;
    static final int PREFERRED_IMG_H = 12;

    static int getColor(int colorSpecifier) {
        switch (colorSpecifier) {
        case Display.COLOR_BACKGROUND:
        case Display.COLOR_HIGHLIGHTED_FOREGROUND:
            return 0xffffff;
        default:
            return 0;
        }
    }

    static int getBorderStyle(boolean highlighted) {
        return highlighted ? Graphics.SOLID : Graphics.DOTTED;
    }
}
