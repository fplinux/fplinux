// SPDX-License-Identifier: GPL-2.0-only
package fplinux.fixtures.canvas;

import javax.microedition.lcdui.Graphics;
import javax.microedition.lcdui.game.GameCanvas;

/** Full-screen GameCanvas that counts showNotify calls. */
class ProbeGameCanvas extends GameCanvas {
    volatile int showCount;

    ProbeGameCanvas() {
        super(false);
        setFullScreenMode(true);
    }

    protected void showNotify() {
        showCount++;
    }

    /** Fills the buffer, draws a label and flushes it. */
    void drawFrame(int frame) {
        Graphics g = getGraphics();
        g.setColor(0x000000);
        g.fillRect(0, 0, getWidth(), getHeight());
        g.setColor(0xFFFFFF);
        g.drawString("frame " + frame, 2, 2, Graphics.TOP | Graphics.LEFT);
        flushGraphics();
    }
}
