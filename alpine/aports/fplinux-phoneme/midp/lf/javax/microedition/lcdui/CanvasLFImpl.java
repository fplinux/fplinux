/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * fplinux-phoneme LCDUI look and feel: Canvas. The peer is a libfplinux-ui
 * canvas screen whose lv_canvas shows a completed copy of the putpixel
 * screen buffer (port/canvas/fplinux-pm-screen.c). paint() changes stay
 * private until refresh publishes them. A normal Canvas keeps the integrated
 * layout: its title
 * in the band and its commands on the soft keys, except OK and ITEM
 * commands, which go to the soft keys or Options so that OK is always FIRE.
 * In full screen the app takes the whole display: no band, no commands on
 * the peer, and the soft keys reach keyPressed() as -6 and -7 (Nokia).
 * Keys, repeats and the drawing area's size come from port/canvas; the
 * size of a mode is known at once, and when the runtime's resize gives
 * another one, Display reads it again (ROTATION_EVENT) and sizeChanged()
 * follows.
 */
package javax.microedition.lcdui;

import java.util.Enumeration;
import java.util.Vector;

import javax.microedition.lcdui.game.GameCanvas;

import com.sun.midp.lcdui.GameCanvasLFImpl;
import com.sun.midp.lcdui.GameMap;

class CanvasLFImpl extends DisplayableLFImpl implements CanvasLF {

    Canvas canvas;
    private Vector embeddedVideos = new Vector(1);
    private static MMHelperImpl mmHelper = MMHelperImpl.getInstance();

    CanvasLFImpl(Canvas canvas) {
        super(canvas);
        this.canvas = canvas;
        /* A new Canvas is normal and untitled, whatever is shown now. */
        lPredictSize();
    }

    /** The size the Canvas will get when shown, while it is not. */
    private void lPredictSize() {
        int size = predictSize0(owner.isInFullScreenMode, lPeerTitle() != null);
        width = size >>> 16;
        height = size & 0xffff;
    }

    /* ---- port/canvas natives (all do nothing headless) --------------- */

    /** A new canvas peer showing the screen buffer; 0 headless. */
    private static native int createCanvas0();

    /** The Canvas about to be shown has a title band or not. */
    private static native void prepare0(boolean titled);

    /** Refreshes, keys and the display light follow this peer. */
    private static native void activate0(int peer);

    /** The peer is hidden: nothing follows it any more. */
    private static native void deactivate0(int peer);

    /** The shown Canvas got or lost its band; sizeChanged() follows. */
    private static native void titleChanged0(boolean titled);

    /** The drawing area of a mode, width << 16 | height. */
    private static native int predictSize0(boolean fullScreen, boolean titled);

    /** paint() runs: the half-drawn buffer must not be shown. */
    private static native void painting0(boolean painting);

    /* ---- peer hooks (DisplayableLFImpl, FACTORY.md section 2) -------- */

    void createNativeResource() {
        nativeId = createCanvas0();
    }

    String lPeerTitle() {
        return owner.isInFullScreenMode ? null : owner.title;
    }

    boolean lPeerHasCommands() {
        return !owner.isInFullScreenMode;
    }

    /** OK and ITEM commands never take OK, which stays FIRE (-5). */
    int lPeerCommandType(Command c) {
        int type = super.lPeerCommandType(c);
        if (type == LFNative.CMD_OK || type == LFNative.CMD_ITEM) {
            return LFNative.CMD_SCREEN;
        }
        return type;
    }

    /* ---- size and mode ------------------------------------------------ */

    /* The drawing area as Display last gave it, not the peer's surface. */
    public int lGetWidth() {
        return width;
    }

    public int lGetHeight() {
        return height;
    }

    public void lSetTitle(String oldTitle, String newTitle) {
        super.lSetTitle(oldTitle, newTitle);
        if (owner.isInFullScreenMode || (oldTitle == null) == (newTitle == null)) {
            return;
        }
        if (state == SHOWN) {
            titleChanged0(newTitle != null);
        } else {
            /* getHeight() counts the band at once; sizeChanged() at show. */
            lPredictSize();
            deferredSizeChange = true;
        }
    }

    public void uSetFullScreenMode(boolean mode) {
        boolean shown;
        int w;
        int h;
        StubLog.line("setFullScreenMode " + StubLog.name(owner) + " " + mode);
        synchronized (Display.LCDUILock) {
            /*
             * Not lIsShown(): Display makes this Canvas current only after
             * uCallShow(), so showNotify() calling setFullScreenMode() is
             * already in the shown state.
             */
            shown = state == SHOWN && currentDisplay != null;
            if (shown) {
                prepare0(lPeerTitle() != null);
                /* Asks the runtime for the mode; Display reads the size. */
                currentDisplay.lSetFullScreen(mode);
                lUpdatePeerTitle();
                updateCommandSet();
                /* Display's current may still be the previous screen. */
                lSyncPeerCommands();
                w = currentDisplay.width;
                h = currentDisplay.height;
            } else {
                int size = predictSize0(mode, lPeerTitle() != null);
                w = size >>> 16;
                h = size & 0xffff;
            }
        }
        /* Shown: sizeChanged() now; hidden: deferred to the next show. */
        uCallSizeChanged(w, h);
    }

    public void uCallSizeChanged(int w, int h) {
        boolean changed;
        synchronized (Display.LCDUILock) {
            changed = w != width || h != height;
        }
        super.uCallSizeChanged(w, h);
        if (changed) {
            synchronized (Display.LCDUILock) {
                lRequestPaint();
            }
        }
    }

    /* ---- show and hide ------------------------------------------------ */

    public void uCallShow() {
        boolean deferred;
        int oldWidth;
        int oldHeight;
        synchronized (Display.LCDUILock) {
            prepare0(lPeerTitle() != null);
            /*
             * Display skips a mode it already has; let it read the size of
             * this Canvas, whose band may differ from the last one's.
             */
            currentDisplay.screenModeKnown = false;
            deferred = deferredSizeChange;
            oldWidth = width;
            oldHeight = height;
        }
        super.uCallShow();
        if (state != SHOWN) {
            return;
        }
        int w;
        int h;
        boolean resized;
        synchronized (Display.LCDUILock) {
            w = width;
            h = height;
            resized = !deferred && (w != oldWidth || h != oldHeight);
            if (owner instanceof GameCanvas) {
                /* The off-screen buffer must cover the area shown now. */
                GameCanvasLFImpl game = GameMap.getGameCanvasImpl((GameCanvas) owner);
                if (game != null) {
                    game.lCallSizeChanged(w, h);
                }
            }
        }
        if (resized) {
            /* Shown at another size than the MIDlet knows: tell it. */
            uCallSizeChanged(w, h);
        }
        synchronized (Display.calloutLock) {
            try {
                canvas.showNotify();
                if (mmHelper != null) {
                    for (Enumeration e = embeddedVideos.elements(); e.hasMoreElements();) {
                        mmHelper.showVideo(e.nextElement());
                    }
                }
            } catch (Throwable t) {
                Display.handleThrowable(t);
            }
        }
    }

    boolean lCallShow() {
        if (!super.lCallShow()) {
            return false;
        }
        activate0(nativeId);
        return true;
    }

    void lCallHide() {
        deactivate0(nativeId);
        super.lCallHide();
    }

    void lCallFreeze() {
        deactivate0(nativeId);
        super.lCallFreeze();
    }

    public void uCallHide() {
        int oldState = state;
        super.uCallHide();
        notifyHidden(oldState);
    }

    public void uCallFreeze() {
        int oldState = state;
        super.uCallFreeze();
        notifyHidden(oldState);
    }

    private void notifyHidden(int oldState) {
        if (oldState != SHOWN) {
            return;
        }
        synchronized (Display.calloutLock) {
            try {
                canvas.hideNotify();
                if (mmHelper != null) {
                    for (Enumeration e = embeddedVideos.elements(); e.hasMoreElements();) {
                        mmHelper.hideVideo(e.nextElement());
                    }
                }
            } catch (Throwable t) {
                Display.handleThrowable(t);
            }
        }
    }

    /* ---- painting ----------------------------------------------------- */

    public void lRepaint(int x, int y, int w, int h, Object target) {
        if (LFNative.HEADLESS) {
            StubLog.repaint(owner);
        }
        lRequestPaint(x, y, w, h, target);
    }

    public void lRepaint() {
        if (LFNative.HEADLESS) {
            StubLog.repaint(owner);
        }
        lRequestPaint();
    }

    public void uServiceRepaints() {
        Display d = currentDisplay;
        if (d != null) {
            d.serviceRepaints(this);
        }
    }

    public void uCallPaint(Graphics g, Object target) {
        if (LFNative.HEADLESS) {
            StubLog.paint(owner);
        }
        g.resetGC();
        painting0(true);
        try {
            synchronized (Display.calloutLock) {
                canvas.paint(g);
            }
            if (mmHelper != null) {
                for (Enumeration e = embeddedVideos.elements(); e.hasMoreElements();) {
                    mmHelper.paintVideo(e.nextElement(), g);
                }
            }
        } catch (Throwable t) {
            Display.handleThrowable(t);
        } finally {
            painting0(false);
        }
    }

    /* ---- keys and pointer --------------------------------------------- */

    void uCallKeyPressed(int keyCode) {
        if (allowKey(keyCode)) {
            synchronized (Display.calloutLock) {
                try {
                    canvas.keyPressed(keyCode);
                } catch (Throwable t) {
                    Display.handleThrowable(t);
                }
            }
        }
    }

    void uCallKeyReleased(int keyCode) {
        if (allowKey(keyCode)) {
            synchronized (Display.calloutLock) {
                try {
                    canvas.keyReleased(keyCode);
                } catch (Throwable t) {
                    Display.handleThrowable(t);
                }
            }
        }
    }

    void uCallKeyRepeated(int keyCode) {
        if (allowKey(keyCode)) {
            synchronized (Display.calloutLock) {
                try {
                    canvas.keyRepeated(keyCode);
                } catch (Throwable t) {
                    Display.handleThrowable(t);
                }
            }
        }
    }

    void uCallPointerPressed(int x, int y) {
        synchronized (Display.calloutLock) {
            try {
                canvas.pointerPressed(x, y);
            } catch (Throwable t) {
                Display.handleThrowable(t);
            }
        }
    }

    void uCallPointerReleased(int x, int y) {
        synchronized (Display.calloutLock) {
            try {
                canvas.pointerReleased(x, y);
            } catch (Throwable t) {
                Display.handleThrowable(t);
            }
        }
    }

    void uCallPointerDragged(int x, int y) {
        synchronized (Display.calloutLock) {
            try {
                canvas.pointerDragged(x, y);
            } catch (Throwable t) {
                Display.handleThrowable(t);
            }
        }
    }

    /* Used by MMHelperImpl for MMAPI video in a Canvas. */
    void addEmbeddedVideo(Object video) {
        embeddedVideos.addElement(video);
    }

    void removeEmbeddedVideo(Object video) {
        embeddedVideos.removeElement(video);
    }

    /** GameCanvas with suppressKeyEvents gets no game-action key events. */
    private boolean allowKey(int keyCode) {
        if (!canvas.suppressKeyEvents) {
            return true;
        }
        switch (KeyConverter.getGameAction(keyCode)) {
        case Canvas.UP:
        case Canvas.DOWN:
        case Canvas.LEFT:
        case Canvas.RIGHT:
        case Canvas.FIRE:
        case Canvas.GAME_A:
        case Canvas.GAME_B:
        case Canvas.GAME_C:
        case Canvas.GAME_D:
            return false;
        default:
            return true;
        }
    }
}
