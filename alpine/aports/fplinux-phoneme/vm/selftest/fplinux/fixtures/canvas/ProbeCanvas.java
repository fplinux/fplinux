// SPDX-License-Identifier: GPL-2.0-only
package fplinux.fixtures.canvas;

import javax.microedition.lcdui.Canvas;
import javax.microedition.lcdui.Graphics;

/** Canvas that counts the callbacks the probe waits for. */
class ProbeCanvas extends Canvas {
    volatile int showCount;
    volatile int hideCount;
    volatile int paintCount;
    volatile int sizeChangedCount;
    volatile int lastW = -1;
    volatile int lastH = -1;

    protected void paint(Graphics g) {
        g.setColor(0xFFFFFF);
        g.fillRect(0, 0, getWidth(), getHeight());
        paintCount++;
    }

    protected void showNotify() {
        showCount++;
    }

    protected void hideNotify() {
        hideCount++;
    }

    protected void sizeChanged(int w, int h) {
        lastW = w;
        lastH = h;
        sizeChangedCount++;
    }
}
