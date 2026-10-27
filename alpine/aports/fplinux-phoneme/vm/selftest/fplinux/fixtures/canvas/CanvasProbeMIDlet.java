// SPDX-License-Identifier: GPL-2.0-only
package fplinux.fixtures.canvas;

import javax.microedition.lcdui.Canvas;
import javax.microedition.lcdui.Display;
import javax.microedition.lcdui.Font;
import javax.microedition.midlet.MIDlet;

/**
 * Canvas/GameCanvas probe. All steps run on a thread of their own so the
 * event thread is never blocked; every output line starts with "PM-CANVAS ".
 */
public class CanvasProbeMIDlet extends MIDlet implements Runnable {
    private static final int POLL_MS = 20;
    private static final int TIMEOUT_MS = 5000;
    private static final String[] ACTION_NAMES = {
        "UP", "DOWN", "LEFT", "RIGHT", "FIRE",
        "GAME_A", "GAME_B", "GAME_C", "GAME_D"
    };
    private static final int[] ACTIONS = {
        Canvas.UP, Canvas.DOWN, Canvas.LEFT, Canvas.RIGHT, Canvas.FIRE,
        Canvas.GAME_A, Canvas.GAME_B, Canvas.GAME_C, Canvas.GAME_D
    };
    // Cyrillic word "Klavishi" as escapes
    private static final String SAMPLE =
        "Keys \u041a\u043b\u0430\u0432\u0438\u0448\u0438";

    private Display display;
    private boolean started;

    protected void startApp() {
        if (started) {
            return;
        }
        started = true;
        display = Display.getDisplay(this);
        new Thread(this).start();
    }

    protected void pauseApp() {
    }

    protected void destroyApp(boolean unconditional) {
    }

    private static void say(String line) {
        System.out.println("PM-CANVAS " + line);
    }

    public void run() {
        try {
            probe();
        } catch (Throwable t) {
            say("exception " + t.getClass().getName());
        }
        say("done");
        notifyDestroyed();
    }

    private void probe() throws InterruptedException {
        say("start");
        ProbeCanvas canvas = new ProbeCanvas();
        canvas.setTitle("Probe");
        display.setCurrent(canvas);
        waitFor(canvas, "show", 0);
        waitFor(canvas, "paint", 0);
        say("shown size=" + canvas.getWidth() + "x" + canvas.getHeight());

        keys(canvas);
        say("hasRepeatEvents " + canvas.hasRepeatEvents());
        say("hasPointerEvents " + canvas.hasPointerEvents());
        say("isDoubleBuffered " + canvas.isDoubleBuffered());

        fullScreen(canvas, true, "fullscreen");
        fullScreen(canvas, false, "normal");

        game(canvas);
        showFull();
        font();
    }

    /** setFullScreenMode(true) from showNotify(): Display follows. */
    private void showFull() throws InterruptedException {
        ProbeShowFullCanvas c = new ProbeShowFullCanvas();
        display.setCurrent(c);
        waitFor(c, "paint", 0);
        Thread.sleep(200);
        say("shownotify full size=" + c.getWidth() + "x" + c.getHeight()
            + " clip=" + c.lastClipH);
    }

    /** Polls until a canvas condition holds; prints "timeout <what>" if not. */
    private void waitFor(ProbeCanvas c, String what, int notHeight)
            throws InterruptedException {
        for (int waited = 0; waited < TIMEOUT_MS; waited += POLL_MS) {
            boolean ok;
            if (what.equals("show")) {
                ok = c.showCount > 0;
            } else if (what.equals("paint")) {
                ok = c.paintCount > 0;
            } else {
                ok = c.lastH != -1 && c.lastH != notHeight;
            }
            if (ok) {
                return;
            }
            Thread.sleep(POLL_MS);
        }
        say("timeout " + what);
    }

    private void keys(Canvas c) {
        int[] codes = new int[19];
        int n = 0;
        for (int k = -1; k >= -7; k--) {
            codes[n++] = k;
        }
        for (int k = 48; k <= 57; k++) {
            codes[n++] = k;
        }
        codes[n++] = 42;
        codes[n++] = 35;
        for (int i = 0; i < codes.length; i++) {
            String action;
            String name;
            try {
                action = Integer.toString(c.getGameAction(codes[i]));
            } catch (IllegalArgumentException e) {
                action = "IAE";
            }
            try {
                name = c.getKeyName(codes[i]);
            } catch (IllegalArgumentException e) {
                name = "IAE";
            }
            say("key " + codes[i] + " action=" + action + " name=" + name);
        }
        for (int i = 0; i < ACTIONS.length; i++) {
            String code;
            try {
                code = Integer.toString(c.getKeyCode(ACTIONS[i]));
            } catch (IllegalArgumentException e) {
                code = "IAE";
            }
            say("code " + ACTION_NAMES[i] + " " + code);
        }
    }

    private void fullScreen(ProbeCanvas c, boolean on, String label)
            throws InterruptedException {
        int oldHeight = c.getHeight();
        c.lastW = -1;
        c.lastH = -1;
        c.setFullScreenMode(on);
        waitFor(c, "sizeChanged " + label, oldHeight);
        say(label + " size=" + c.getWidth() + "x" + c.getHeight()
            + " sizeChanged=" + c.lastW + "x" + c.lastH);
    }

    private void game(ProbeCanvas first) throws InterruptedException {
        ProbeGameCanvas game = new ProbeGameCanvas();
        display.setCurrent(game);
        boolean ok = false;
        for (int waited = 0; waited < TIMEOUT_MS && !ok; waited += POLL_MS) {
            ok = game.showCount > 0 && first.hideCount > 0;
            if (!ok) {
                Thread.sleep(POLL_MS);
            }
        }
        if (!ok) {
            say("timeout game show");
        }
        say("hide count=" + first.hideCount);
        for (int i = 0; i < 10; i++) {
            game.drawFrame(i);
        }
        say("game size=" + game.getWidth() + "x" + game.getHeight()
            + " flushed=10 keyStates=" + game.getKeyStates());
    }

    private void font() {
        Font f = Font.getDefaultFont();
        char[] chars = SAMPLE.toCharArray();
        boolean height = f.getHeight() > 0;
        boolean width = f.stringWidth(SAMPLE)
            == f.charsWidth(chars, 0, chars.length);
        boolean substring = f.substringWidth(SAMPLE, 0, SAMPLE.length())
                == f.stringWidth(SAMPLE)
            && f.substringWidth(SAMPLE, 2, 5)
                == f.stringWidth(SAMPLE.substring(2, 7));
        say("font height=" + height + " width=" + width + " substring="
            + substring);
    }
}
