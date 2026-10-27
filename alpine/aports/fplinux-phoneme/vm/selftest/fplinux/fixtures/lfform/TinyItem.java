// SPDX-License-Identifier: GPL-2.0-only
package fplinux.fixtures.lfform;

import javax.microedition.lcdui.CustomItem;
import javax.microedition.lcdui.Graphics;

/** Appending a CustomItem must not crash, although custom painting is unavailable. */
final class TinyItem extends CustomItem {
    TinyItem(String label) {
        super(label);
    }

    protected int getMinContentWidth() {
        return 16;
    }

    protected int getMinContentHeight() {
        return 8;
    }

    protected int getPrefContentWidth(int height) {
        return 32;
    }

    protected int getPrefContentHeight(int width) {
        return 8;
    }

    protected void paint(Graphics g, int w, int h) {
        g.setColor(0x008800);
        g.fillRect(0, 0, w, h);
    }
}
