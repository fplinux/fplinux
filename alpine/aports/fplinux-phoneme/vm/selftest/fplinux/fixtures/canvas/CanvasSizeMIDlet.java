// SPDX-License-Identifier: GPL-2.0-only
package fplinux.fixtures.canvas;

import javax.microedition.lcdui.Display;
import javax.microedition.midlet.MIDlet;

/** Reports actual Canvas dimensions before and after fullscreen changes. */
public class CanvasSizeMIDlet extends MIDlet implements Runnable {
    private Display display;
    private boolean started;

    protected void startApp() {
        if (!started) {
            started = true;
            display = Display.getDisplay(this);
            new Thread(this).start();
        }
    }

    protected void pauseApp() { }
    protected void destroyApp(boolean unconditional) { }

    public void run() {
        ProbeCanvas canvas = new ProbeCanvas();
        canvas.setTitle("Size");
        display.setCurrent(canvas);
        try {
            for (int i = 0; i < 250 && canvas.showCount == 0; i++) {
                Thread.sleep(20);
            }
            if (canvas.showCount == 0) {
                System.out.println("PM-SIZE timeout");
            } else {
                System.out.println("PM-SIZE shown " + canvas.getWidth() + "x" + canvas.getHeight());
                canvas.setFullScreenMode(true);
                System.out.println("PM-SIZE full " + canvas.getWidth() + "x" + canvas.getHeight()
                    + " changed " + canvas.lastW + "x" + canvas.lastH);
                canvas.setFullScreenMode(false);
                System.out.println("PM-SIZE normal " + canvas.getWidth() + "x" + canvas.getHeight()
                    + " changed " + canvas.lastW + "x" + canvas.lastH);
            }
        } catch (InterruptedException e) {
            System.out.println("PM-SIZE interrupted");
        }
        notifyDestroyed();
    }
}
