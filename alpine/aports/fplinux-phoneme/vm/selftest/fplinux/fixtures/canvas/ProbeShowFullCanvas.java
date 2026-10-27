// SPDX-License-Identifier: GPL-2.0-only
package fplinux.fixtures.canvas;

import javax.microedition.lcdui.Graphics;

/**
 * Canvas that asks for full screen in showNotify(), a common MIDlet idiom:
 * the paint clip shows whether Display took the full-screen size too.
 */
class ProbeShowFullCanvas extends ProbeCanvas {
    volatile int lastClipH = -1;

    protected void showNotify() {
        super.showNotify();
        setFullScreenMode(true);
    }

    protected void paint(Graphics g) {
        lastClipH = g.getClipY() + g.getClipHeight();
        super.paint(g);
    }
}
