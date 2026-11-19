// SPDX-License-Identifier: GPL-2.0-only
package fplinux.fixtures.canvas;

import javax.microedition.lcdui.Canvas;
import javax.microedition.lcdui.Display;
import javax.microedition.lcdui.Graphics;
import javax.microedition.lcdui.game.GameCanvas;
import javax.microedition.midlet.MIDlet;

public final class GameCanvasKeysMIDlet extends MIDlet {
    private Display display;
    private final Keys game = new Keys();

    protected void startApp() {
        display = Display.getDisplay(this);
        display.setCurrent(game);
    }

    protected void pauseApp() {}

    protected void destroyApp(boolean unconditional) {}

    private static void result(String label, boolean pass) {
        System.out.println("PM-GAMEKEYS " + label + "=" + pass);
    }

    private void showPlainCanvas() {
        display.setCurrent(new Canvas() {
            protected void paint(Graphics graphics) {}

            protected void showNotify() {
                display.callSerially(new Runnable() {
                    public void run() {
                        result("hidden", !game.isShown() && game.getKeyStates() == 0);
                        display.setCurrent(game);
                    }
                });
            }
        });
    }

    private final class Keys extends GameCanvas {
        private int shows;

        Keys() {
            super(false);
        }

        protected void showNotify() {
            if (++shows > 1) {
                display.callSerially(new Runnable() {
                    public void run() {
                        result("reshown", isShown() && getKeyStates() == 0);
                        notifyDestroyed();
                    }
                });
            }
        }

        protected void keyPressed(int code) {
            if (getGameAction(code) == Canvas.UP) {
                // Consume the press latch so the release tests the live mask.
                getKeyStates();
            }
        }

        protected void keyReleased(int code) {
            if (code == -1) {
                int held = getKeyStates();
                result("alias", held == UP_PRESSED && getKeyStates() == UP_PRESSED);
            } else if (code == '2') {
                result("released", getKeyStates() == 0);
            } else if (code == '5') {
                int latched = getKeyStates();
                result("tap", latched == FIRE_PRESSED && getKeyStates() == 0);
                showPlainCanvas();
            }
        }
    }
}
