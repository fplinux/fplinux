/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * fplinux-phoneme LCDUI look and feel: the base of every Displayable LF. A
 * Displayable is shown as a native libfplinux-ui screen, its peer, which the
 * subclass builds in createNativeResource(); this class puts peers on the
 * runtime stack, keeps their titles and commands, and follows the app's
 * screen stack contract (port/app/fplinux-pm-app.h):
 *
 *   suite screen (the app's)  <-  base peer  <-  alert peer (optional)
 *
 * Display.setCurrent() replaces the base peer with fplinux_ui_replace(); an
 * Alert replaces the base with its return screen, then is pushed over it.
 * Peers outlive hide and freeze: only showNotify/hideNotify happen there,
 * and a hidden Displayable's peer can stay as the base under an Alert. The
 * peer goes when another one replaces it (lPeerDeleting() runs first).
 */
package javax.microedition.lcdui;

import javax.microedition.lcdui.game.GameCanvas;

import com.sun.midp.lcdui.EventConstants;
import com.sun.midp.lcdui.GameCanvasLFImpl;
import com.sun.midp.lcdui.GameMap;

abstract class DisplayableLFImpl implements DisplayableLF {

    static final int HIDDEN = 0;
    static final int SHOWN  = 1;
    static final int FROZEN = 2;

    /** Native command ids are peerSerial * COMMAND_SLOTS + index. */
    static final int COMMAND_SLOTS = 1024;
    /** Serials stay below this so that command ids stay positive. */
    private static final int SERIAL_LIMIT = Integer.MAX_VALUE / COMMAND_SLOTS;

    /* The MIDlet's part of the runtime stack (see the file comment). */
    static DisplayableLFImpl basePeer;
    static AlertLFImpl alertPeer;
    /** The Displayable Display showed last; its commands are on the keys. */
    static DisplayableLFImpl shownLF;
    private static int lastSerial;

    Displayable owner;
    Display currentDisplay;
    int width, height;
    int state; // = HIDDEN
    boolean deferredSizeChange = true;
    boolean pendingInvalidate;
    boolean sawKeyPress;
    boolean sawPointerPress;

    /** The peer, a struct fplinux_ui_screen * as int; 0 without one. */
    int nativeId;
    /** Identifies the peer in events and command ids; 0 without one. */
    int peerSerial;
    /** Display's initial Displayable, shown before any setCurrent(). */
    boolean initialDisplayable;

    /* The commands on the peer, their native types and indexes. */
    private Command[] peerCommands = new Command[4];
    private int[] peerCommandTypes = new int[4];
    private int[] peerCommandIndexes = new int[4];
    private int peerCommandCount;

    /* GameCanvas.getKeyStates() latching (see lfplatform). */
    private int stickyKeyMask;
    private int currentKeyMask;

    DisplayableLFImpl(Displayable d) {
        owner = d;
        width = Display.WIDTH;
        height = Display.HEIGHT;
    }

    /* ---- hooks for subclasses (FACTORY.md section 2) ------------------ */

    /**
     * Builds the peer: sets nativeId to a new, unstacked screen with all
     * its content. The default builds none, so the Displayable has no
     * native screen.
     */
    void createNativeResource() {
    }

    /**
     * The runtime is about to delete the peer; nativeId is still valid.
     * Copy user state (focus, text, marks) into the model here.
     */
    void lPeerDeleting() {
    }

    /** The title the peer shows. */
    String lPeerTitle() {
        return owner.title;
    }

    /**
     * True while the peer can take native calls. An Alert's peer can be
     * deleted by the runtime behind Java's back (a command removes it);
     * AlertLFImpl then forgets it here.
     */
    boolean lHasPeer() {
        return nativeId != 0;
    }

    /** Applies lPeerTitle() to the peer. */
    void lUpdatePeerTitle() {
        if (lHasPeer()) {
            LFNative.setTitle0(nativeId, lPeerTitle());
        }
    }

    /** False puts no commands on the peer (full-screen Canvas). */
    boolean lPeerHasCommands() {
        return true;
    }

    /** False keeps one command off the peer (it is on another key). */
    boolean lPeerShowsCommand(Command c) {
        return true;
    }

    /** The screen mode to ask for while this Displayable is shown. */
    boolean lFullScreenMode() {
        return owner.isInFullScreenMode;
    }

    /** The native type (LFNative.CMD_*) of a command of this Displayable. */
    int lPeerCommandType(Command c) {
        switch (c.getCommandType()) {
        case Command.BACK:   return LFNative.CMD_BACK;
        case Command.CANCEL: return LFNative.CMD_CANCEL;
        case Command.EXIT:   return LFNative.CMD_EXIT;
        case Command.STOP:   return LFNative.CMD_STOP;
        case Command.OK:     return LFNative.CMD_OK;
        case Command.ITEM:   return LFNative.CMD_ITEM;
        case Command.HELP:   return LFNative.CMD_HELP;
        default:             return LFNative.CMD_SCREEN;
        }
    }

    /** Called with the peer kept when the Displayable is hidden. */
    void lCallHide() {
    }

    /** Called with the peer kept when the app goes to the background. */
    void lCallFreeze() {
    }

    /* ---- the runtime stack ------------------------------------------- */

    /** Builds and attaches the peer if needed; false without one. */
    final boolean lEnsurePeer() {
        if (nativeId != 0) {
            return true;
        }
        if (LFNative.HEADLESS) {
            return false;
        }
        createNativeResource();
        if (nativeId == 0) {
            return false;
        }
        if (++lastSerial >= SERIAL_LIMIT) {
            lastSerial = 1;
        }
        peerSerial = lastSerial;
        LFNative.attach0(nativeId, peerSerial, this instanceof AlertLFImpl);
        peerCommandCount = 0;
        lUpdatePeerTitle();
        lSyncPeerCommands();
        return true;
    }

    /** Puts the peer on the stack for a show; an Alert overrides it. */
    boolean lShowPeer() {
        return lShowAsBase();
    }

    /**
     * Makes this Displayable's peer the base of the MIDlet's screens:
     * removes the alert, and replaces the old base (or, the first time,
     * the app's loading alert).
     */
    final boolean lShowAsBase() {
        if (!lEnsurePeer()) {
            return LFNative.HEADLESS;
        }
        if (alertPeer != null && alertPeer != this) {
            if (!alertPeer.lRemoveAlertPeer()) {
                return false;
            }
        }
        /*
         * Already the base (a show after freeze or after an alert): keep
         * any runtime screen over it, such as an open popup choice.
         */
        if (basePeer == this && nativeId != 0) {
            return true;
        }
        DisplayableLFImpl old = basePeer;
        if (old != null && old.nativeId != 0) {
            if (!popAbove(old.nativeId)) {
                return false;
            }
            old.lPeerDeleting();
        }
        if (!LFNative.replace0(nativeId)) {
            return false;
        }
        if (old != null) {
            old.lForgetPeer();
        }
        basePeer = this;
        return true;
    }

    /** Pops runtime screens (such as a popup choice) above a peer. */
    static boolean popAbove(int screen) {
        int top;
        while ((top = LFNative.current0()) != 0 && top != screen) {
            if (!LFNative.pop0()) {
                return false;
            }
        }
        return top == screen;
    }

    /** The peer is about to go: last call, then forget it. */
    final void lPeerGone() {
        if (nativeId != 0) {
            lPeerDeleting();
        }
        lForgetPeer();
    }

    /** Forgets a peer whose native resource has already gone. */
    final void lForgetPeer() {
        nativeId = 0;
        peerSerial = 0;
        peerCommandCount = 0;
    }

    /**
     * Puts the Displayable's commands on the peer, renumbered as
     * peerSerial * COMMAND_SLOTS + index, replacing the previous set. An
     * unchanged set stays as it is (Display renumbers at every show).
     */
    void lSyncPeerCommands() {
        synchronized (Display.LCDUILock) {
            if (lHasPeer()) {
                lSyncPeerCommandsLocked();
            }
        }
    }

    /*
     * Display.updateCommandSet() may run without LCDUILock (upstream
     * Displayable.setCommandListener), so the sync takes it itself.
     */
    private void lSyncPeerCommandsLocked() {
        int count = Math.min(owner.numCommands, COMMAND_SLOTS);
        boolean shown = lPeerHasCommands();
        /*
         * Command objects can be shared between Displayables; only the
         * shown one's ids may change, as Display matches events by them.
         */
        boolean current = shownLF == this && state == SHOWN;
        Command[] commands = new Command[count];
        int[] types = new int[count];
        int[] indexes = new int[count];
        int wanted = 0;
        for (int i = 0; i < count; i++) {
            Command c = owner.commands[i];
            if (current) {
                c.setInternalID(peerSerial * COMMAND_SLOTS + i);
            }
            if (shown && lPeerShowsCommand(c)) {
                commands[wanted] = c;
                types[wanted] = lPeerCommandType(c);
                indexes[wanted] = i;
                wanted++;
            }
        }
        if (wanted == peerCommandCount) {
            int same = 0;
            while (same < wanted && commands[same] == peerCommands[same]
                   && types[same] == peerCommandTypes[same]
                   && indexes[same] == peerCommandIndexes[same]) {
                same++;
            }
            if (same == wanted) {
                return;
            }
        }
        for (int i = 0; i < peerCommandCount; i++) {
            LFNative.removeCommand0(nativeId,
                peerSerial * COMMAND_SLOTS + peerCommandIndexes[i]);
        }
        peerCommandCount = 0;
        if (wanted > peerCommands.length) {
            peerCommands = new Command[wanted];
            peerCommandTypes = new int[wanted];
            peerCommandIndexes = new int[wanted];
        }
        for (int i = 0; i < wanted; i++) {
            Command c = commands[i];
            int id = peerSerial * COMMAND_SLOTS + indexes[i];
            if (LFNative.addCommand0(nativeId, id, types[i], commandLabel(c),
                                     c.getPriority())) {
                peerCommands[peerCommandCount] = c;
                peerCommandTypes[peerCommandCount] = types[i];
                peerCommandIndexes[peerCommandCount] = indexes[i];
                peerCommandCount++;
            }
        }
    }

    /** The short label, else the long one; null for the type's word. */
    static String commandLabel(Command c) {
        String label = c.getLabel();
        if (label == null || label.length() == 0) {
            label = c.getLongLabel();
        }
        return (label == null || label.length() == 0) ? null : label;
    }

    /** Display renumbered the current Displayable's commands. */
    static void lCommandsChanged(Command[] commands) {
        DisplayableLFImpl lf = shownLF;
        if (lf != null && lf.state == SHOWN && lf.owner.commands == commands) {
            lf.lSyncPeerCommands();
        }
    }

    /* ---- DisplayableLF: model to LF ---------------------------------- */

    public boolean lIsShown() {
        return currentDisplay != null && currentDisplay.isShown(this);
    }

    public int lGetWidth() {
        if (lHasPeer()) {
            int w = LFNative.contentWidth0(nativeId);
            if (w > 0) {
                return w;
            }
        }
        return width;
    }

    public int lGetHeight() {
        if (lHasPeer()) {
            int h = LFNative.contentHeight0(nativeId);
            if (h > 0) {
                return h;
            }
        }
        return height;
    }

    public void lSetTitle(String oldTitle, String newTitle) {
        if (state == SHOWN) {
            StubLog.line("title " + StubLog.name(owner) + " " + StubLog.quote(newTitle));
        }
        lUpdatePeerTitle();
    }

    public void lSetTicker(Ticker oldTicker, Ticker newTicker) {
        if (newTicker != null) {
            newTicker.tickerLF.lSetOwner(this);
        }
    }

    public void lAddCommand(Command cmd, int i) {
        updateCommandSet();
        lSyncHiddenPeer();
    }

    public void lRemoveCommand(Command cmd, int i) {
        updateCommandSet();
        lSyncHiddenPeer();
    }

    /** A hidden peer (the base under an Alert) gets the change too. */
    private void lSyncHiddenPeer() {
        if (state != SHOWN && nativeId != 0) {
            lSyncPeerCommands();
        }
    }

    public void updateCommandSet() {
        if (state == SHOWN && currentDisplay != null) {
            currentDisplay.updateCommandSet();
        }
    }

    public Display lGetCurrentDisplay() {
        return currentDisplay;
    }

    public void uSetFullScreenMode(boolean mode) {
        boolean requestRepaint = false;
        StubLog.line("setFullScreenMode " + StubLog.name(owner) + " " + mode);
        synchronized (Display.LCDUILock) {
            if (lIsShown()) {
                currentDisplay.lSetFullScreen(mode);
                updateCommandSet();
                requestRepaint = true;
            }
        }
        if (currentDisplay != null) {
            uCallSizeChanged(currentDisplay.width, currentDisplay.height);
        } else {
            uCallSizeChanged(Display.WIDTH, Display.HEIGHT);
        }
        synchronized (Display.LCDUILock) {
            if (requestRepaint) {
                lRequestPaint();
            }
        }
    }

    /* ---- DisplayableLF: Display to LF -------------------------------- */

    public Displayable lGetDisplayable() {
        return owner;
    }

    public void lSetDisplay(Display d) {
        if (d != null) {
            StubLog.line("setCurrent " + StubLog.name(owner) + " title="
                         + StubLog.quote(owner.title) + describe());
        }
        currentDisplay = d;
    }

    /** Extra detail for the setCurrent and show lines. */
    String describe() {
        return "";
    }

    public void uCallShow() {
        boolean sizeChange;
        synchronized (Display.LCDUILock) {
            currentDisplay.lSetFullScreen(lFullScreenMode());
            width = currentDisplay.width;
            height = currentDisplay.height;
            sizeChange = deferredSizeChange;
            deferredSizeChange = false;
        }
        if (sizeChange) {
            synchronized (Display.calloutLock) {
                try {
                    owner.sizeChanged(width, height);
                } catch (Throwable t) {
                    Display.handleThrowable(t);
                }
            }
        }
        synchronized (Display.LCDUILock) {
            if (!lCallShow()) {
                return;
            }
            if (pendingInvalidate || sizeChange) {
                lRequestInvalidate();
            }
        }
    }

    boolean lCallShow() {
        sawPointerPress = sawKeyPress = false;
        if (owner instanceof GameCanvas) {
            GameMap.registerDisplayAccess(owner, currentDisplay.accessor);
            stickyKeyMask = currentKeyMask = 0;
        } else {
            stickyKeyMask = currentKeyMask = -1;
        }
        /*
         * At foreground, before the MIDlet's first setCurrent(), Display
         * shows its initial Displayable; getCurrent() is null only then.
         * It gets no peer, so the app's loading alert stays.
         */
        if (currentDisplay.getCurrent() == null) {
            initialDisplayable = true;
        }
        if (state != SHOWN) {
            StubLog.line("show " + StubLog.name(owner) + " title="
                         + StubLog.quote(owner.title) + " size=" + width + "x"
                         + height + (owner.isInFullScreenMode ? " fullscreen" : "")
                         + describe());
        }
        if (!initialDisplayable) {
            if (!lShowPeer()) {
                return false;
            }
            LFNative.traceShown0(nativeId);
        }
        state = SHOWN;
        shownLF = this;
        return true;
    }

    public void uCallHide() {
        synchronized (Display.LCDUILock) {
            if (state == SHOWN) {
                StubLog.line("hide " + StubLog.name(owner));
            }
            if (state != HIDDEN) {
                lCallHide();
            }
            state = HIDDEN;
            if (shownLF == this) {
                shownLF = null;
            }
        }
    }

    public void uCallFreeze() {
        synchronized (Display.LCDUILock) {
            if (state == SHOWN) {
                StubLog.line("freeze " + StubLog.name(owner));
                lCallFreeze();
            }
            state = FROZEN;
        }
    }

    public void uCallInvalidate() {
        synchronized (Display.LCDUILock) {
            pendingInvalidate = false;
        }
    }

    /** Native screens return true: Display then skips the buffer refresh. */
    public boolean uIsScrollNative() {
        return false;
    }

    public void uCallSizeChanged(int w, int h) {
        boolean deferred;
        synchronized (Display.LCDUILock) {
            if (owner instanceof GameCanvas) {
                GameCanvasLFImpl gameCanvasLF =
                    GameMap.getGameCanvasImpl((GameCanvas) owner);
                if (gameCanvasLF != null) {
                    gameCanvasLF.lCallSizeChanged(w, h);
                }
            }
            deferredSizeChange = (state != SHOWN);
            deferred = deferredSizeChange;
            width = w;
            height = h;
            if (!deferred) {
                lRequestInvalidate();
            }
        }
        if (!deferred) {
            synchronized (Display.calloutLock) {
                try {
                    owner.sizeChanged(w, h);
                } catch (Throwable t) {
                    Display.handleThrowable(t);
                }
            }
        }
    }

    public void uCallScrollContent(int scrollType, int thumbPosition) {
    }

    public boolean uIsDraggable(int x, int y) {
        return true;
    }

    public int uCallDragContent(int deltaY) {
        return 0;
    }

    public boolean uSetRotatedStatus(boolean newStatus) {
        synchronized (Display.LCDUILock) {
            if (newStatus == owner.isRotated) {
                return false;
            }
            owner.isRotated = newStatus;
            return true;
        }
    }

    public int getVerticalScrollPosition() {
        return 0;
    }

    public int getVerticalScrollProportion() {
        return 100;
    }

    public void uCallPaint(Graphics g, Object target) {
    }

    public void lCommitPendingInteraction() {
    }

    public int uGetKeyMask() {
        synchronized (Display.LCDUILock) {
            int saved = stickyKeyMask | currentKeyMask;
            stickyKeyMask = 0;
            return saved;
        }
    }

    public void uCallKeyEvent(int type, int keyCode) {
        int eventType = -1;
        synchronized (Display.LCDUILock) {
            switch (type) {
            case EventConstants.PRESSED:
                sawKeyPress = true;
                eventType = 0;
                break;
            case EventConstants.RELEASED:
                if (sawKeyPress) {
                    eventType = 1;
                }
                break;
            case EventConstants.REPEATED:
                if (sawKeyPress) {
                    eventType = 2;
                }
                break;
            }
            if (currentKeyMask > -1 && eventType != -1) {
                if (eventType == 1) {
                    releaseKeyMask(keyCode);
                } else {
                    setKeyMask(keyCode);
                }
            }
        }
        switch (eventType) {
        case 0:
            uCallKeyPressed(keyCode);
            break;
        case 1:
            uCallKeyReleased(keyCode);
            break;
        case 2:
            uCallKeyRepeated(keyCode);
            break;
        default:
            break;
        }
    }

    void uCallKeyPressed(int keyCode) { }
    void uCallKeyReleased(int keyCode) { }
    void uCallKeyRepeated(int keyCode) { }

    public void uCallPointerEvent(int type, int x, int y) {
        int eventType = -1;
        synchronized (Display.LCDUILock) {
            switch (type) {
            case EventConstants.PRESSED:
                sawPointerPress = true;
                eventType = 0;
                break;
            case EventConstants.RELEASED:
                if (sawPointerPress) {
                    eventType = 1;
                }
                break;
            case EventConstants.DRAGGED:
                if (sawPointerPress) {
                    eventType = 2;
                }
                break;
            }
        }
        switch (eventType) {
        case 0:
            uCallPointerPressed(x, y);
            break;
        case 1:
            uCallPointerReleased(x, y);
            break;
        case 2:
            uCallPointerDragged(x, y);
            break;
        default:
            break;
        }
    }

    void uCallPointerPressed(int x, int y) { }
    void uCallPointerReleased(int x, int y) { }
    void uCallPointerDragged(int x, int y) { }

    /* ---- helpers ------------------------------------------------------ */

    void lRequestPaint(int x, int y, int w, int h, Object target) {
        if (lIsShown()) {
            currentDisplay.repaintImpl(this, x, y, w, h, target);
        }
    }

    void lRequestPaint() {
        lRequestPaint(0, 0, width, height, null);
    }

    void lRequestInvalidate() {
        pendingInvalidate = true;
        if (state == SHOWN && currentDisplay != null) {
            currentDisplay.invalidate();
        }
    }

    private static int gameMask(int keyCode) {
        switch (KeyConverter.getGameAction(keyCode)) {
        case Canvas.UP:     return GameCanvas.UP_PRESSED;
        case Canvas.DOWN:   return GameCanvas.DOWN_PRESSED;
        case Canvas.LEFT:   return GameCanvas.LEFT_PRESSED;
        case Canvas.RIGHT:  return GameCanvas.RIGHT_PRESSED;
        case Canvas.FIRE:   return GameCanvas.FIRE_PRESSED;
        case Canvas.GAME_A: return GameCanvas.GAME_A_PRESSED;
        case Canvas.GAME_B: return GameCanvas.GAME_B_PRESSED;
        case Canvas.GAME_C: return GameCanvas.GAME_C_PRESSED;
        case Canvas.GAME_D: return GameCanvas.GAME_D_PRESSED;
        default:            return 0;
        }
    }

    private void setKeyMask(int keyCode) {
        int m = gameMask(keyCode);
        stickyKeyMask |= m;
        currentKeyMask |= m;
    }

    private void releaseKeyMask(int keyCode) {
        currentKeyMask &= ~gameMask(keyCode);
    }
}
