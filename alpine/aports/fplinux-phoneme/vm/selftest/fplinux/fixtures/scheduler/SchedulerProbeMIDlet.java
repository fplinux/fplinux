// SPDX-License-Identifier: GPL-2.0-only
package fplinux.fixtures.scheduler;

import javax.microedition.lcdui.Canvas;
import javax.microedition.lcdui.Display;
import javax.microedition.lcdui.Graphics;
import javax.microedition.midlet.MIDlet;

public final class SchedulerProbeMIDlet extends MIDlet implements Runnable {
    private final ProbeCanvas canvas = new ProbeCanvas();
    private final Object monitor = new Object();
    private Thread worker;

    public void startApp() {
        if (worker == null) {
            canvas.setFullScreenMode(true);
            Display.getDisplay(this).setCurrent(canvas);
            worker = new Thread(this);
            worker.start();
        }
    }

    public void pauseApp() {
    }

    public void destroyApp(boolean unconditional) {
        if (worker != null) {
            worker.interrupt();
        }
    }

    public void run() {
        try {
            for (int frame = 0; frame < 12; frame++) {
                canvas.repaint();
                canvas.serviceRepaints();
                long start = System.currentTimeMillis();
                boolean sleeping = (frame & 1) == 0;
                if (sleeping) {
                    Thread.sleep(62);
                } else {
                    synchronized (monitor) {
                        monitor.wait(62);
                    }
                }
                long elapsed = System.currentTimeMillis() - start;
                System.out.println("PM-SLEEP frame=" + frame + " type=" +
                                   (sleeping ? "sleep" : "monitor") +
                                   " elapsed_ms=" + elapsed);
            }
        } catch (InterruptedException stopped) {
            return;
        }
        notifyDestroyed();
    }

    private static final class ProbeCanvas extends Canvas {
        protected void paint(Graphics graphics) {
            graphics.setColor(0x123456);
            graphics.fillRect(0, 0, getWidth(), getHeight());
        }
    }
}
