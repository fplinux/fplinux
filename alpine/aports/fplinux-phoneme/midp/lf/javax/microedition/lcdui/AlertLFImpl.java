/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * fplinux-phoneme LCDUI look and feel: Alert as a native libfplinux-ui
 * alert. Display.setCurrent(alert, next) replaces the base screen with next
 * and pushes the alert over it, so an alert is never the only screen. A
 * finite timeout is a Java timer (the runtime timeout stays 0), and the
 * alert always carries a command: the LF's own dismiss command ("Close" on
 * the right soft key) when the MIDlet adds none. An indicator Gauge is the
 * alert's progress bar or spinner.
 *
 * The runtime removes an alert before it calls the command listener. When
 * the MIDlet then keeps the alert current (no setCurrent() from its
 * listener), the alert is shown again with a new peer.
 */
package javax.microedition.lcdui;

import java.util.Timer;
import java.util.TimerTask;

import com.sun.midp.i18n.Resource;
import com.sun.midp.i18n.ResourceConstants;

class AlertLFImpl extends DisplayableLFImpl implements AlertLF {

    private static final int DEFAULT_TIMEOUT = 2000;
    /** Stands for Alert.DISMISS_COMMAND; natively "Close", right key. */
    private static final Command DISMISS_COMMAND =
        new Command(Resource.getString(ResourceConstants.DONE), Command.CANCEL, 0);
    private static Timer timeoutTimer;

    private Alert alert;
    private TimerTask timerTask;

    AlertLFImpl(Alert a) {
        super(a);
        alert = a;
    }

    /* ---- AlertLF ------------------------------------------------------ */

    public boolean lIsModal() {
        return alert.numCommands > 1;
    }

    public int lGetDefaultTimeout() {
        return DEFAULT_TIMEOUT;
    }

    public Command lGetDismissCommand() {
        return DISMISS_COMMAND;
    }

    public void lSetTimeout(int timeout) {
        if (timerTask != null) {
            timerTask.cancel();
            timerTask = null;
            if (timeout != Alert.FOREVER) {
                schedule(timeout);
            }
        }
    }

    /** The runtime cannot change an alert's kind: a new peer shows it. */
    public void lSetType(AlertType type) {
        if (lPeerAlive() && state != HIDDEN) {
            if (lRemoveAlertPeer()) {
                lShowAsAlert();
            }
        }
    }

    public void lSetString(String oldStr, String newStr) {
        if (state == SHOWN) {
            StubLog.line("alert text " + StubLog.quote(newStr));
        }
        if (lPeerAlive()) {
            LFNative.alertSetText0(nativeId, newStr);
        }
    }

    /** Alert images are not shown (no runtime support yet). */
    public void lSetImage(Image oldImg, Image newImg) {
    }

    public void lSetIndicator(Gauge oldIndicator, Gauge newIndicator) {
        if (newIndicator != null) {
            lApplyIndicator();
        } else if (oldIndicator != null && lPeerAlive() && state != HIDDEN) {
            /* No call clears an indicator: a new peer has none. */
            if (lRemoveAlertPeer()) {
                lShowAsAlert();
            }
        }
    }

    /**
     * The Form LF calls this when the value or range of a Gauge changes:
     * an Alert's indicator updates the alert.
     */
    static void lIndicatorChanged(Gauge gauge) {
        if (gauge != null && gauge.owner instanceof Alert) {
            AlertLF lf = ((Alert) gauge.owner).alertLF;
            if (lf instanceof AlertLFImpl) {
                ((AlertLFImpl) lf).lApplyIndicator();
            }
        }
    }

    private void lApplyIndicator() {
        if (lPeerAlive()) {
            applyIndicator(nativeId);
        }
    }

    /** A spinner for an INDEFINITE gauge, else a progress bar. */
    private void applyIndicator(int peer) {
        Gauge gauge = alert.indicator;
        if (gauge == null) {
            return;
        }
        if (gauge.maxValue == Gauge.INDEFINITE) {
            LFNative.alertSetProgress0(peer, 0, 0);
        } else {
            LFNative.alertSetProgress0(peer, gauge.value, gauge.maxValue);
        }
    }

    /* ---- peer ---------------------------------------------------------- */

    void createNativeResource() {
        int peer = LFNative.alertNew0(alertKind(alert.type), alert.title,
                                      alert.text);
        if (peer != 0) {
            applyIndicator(peer);
        }
        nativeId = peer;
    }

    /*
     * An alert over a full-screen Canvas keeps full screen: the runtime
     * shows an alert's legends in either mode, and the Canvas does not
     * switch twice.
     */
    boolean lFullScreenMode() {
        Displayable next = alert.getReturnScreen();
        return next != null && next.isInFullScreenMode;
    }

    int lPeerCommandType(Command c) {
        if (c == DISMISS_COMMAND) {
            return LFNative.CMD_DISMISS;
        }
        return super.lPeerCommandType(c);
    }

    /** True while the peer exists, not removed by a command. */
    private boolean lPeerAlive() {
        return nativeId != 0 && LFNative.alive0(nativeId, peerSerial);
    }

    /* A peer that a command removed is forgotten before any native call. */
    boolean lHasPeer() {
        if (nativeId != 0 && !LFNative.alive0(nativeId, peerSerial)) {
            lPeerGone();
        }
        return nativeId != 0;
    }

    boolean lShowPeer() {
        return lShowAsAlert();
    }

    /** The return screen becomes the base, then the alert goes over it. */
    final boolean lShowAsAlert() {
        if (nativeId != 0 && !LFNative.alive0(nativeId, peerSerial)) {
            lPeerGone();
        }
        if (alertPeer == this && nativeId != 0) {
            return true;
        }
        if (alertPeer != null && alertPeer != this) {
            if (!alertPeer.lRemoveAlertPeer()) {
                return false;
            }
        }
        Displayable next = alert.getReturnScreen();
        if (next != null && next.displayableLF instanceof DisplayableLFImpl
            && next.displayableLF != this) {
            if (!((DisplayableLFImpl) next.displayableLF).lShowAsBase()) {
                return false;
            }
        }
        if (!lEnsurePeer()) {
            return LFNative.HEADLESS;
        }
        if (!LFNative.push0(nativeId)) {
            return false;
        }
        alertPeer = this;
        if (state == SHOWN) {
            /* A new peer for a shown alert (lCallShow reports the others). */
            LFNative.traceShown0(nativeId);
        }
        return true;
    }

    /** Takes the alert off the stack unless a command already did. */
    final boolean lRemoveAlertPeer() {
        if (nativeId == 0) {
            if (alertPeer == this) {
                alertPeer = null;
            }
            return true;
        }
        if (LFNative.alive0(nativeId, peerSerial)) {
            if (!popAbove(nativeId) || !LFNative.pop0()) {
                return false;
            }
        }
        lForgetPeer();
        if (alertPeer == this) {
            alertPeer = null;
        }
        return true;
    }

    /* ---- Displayable life cycle ---------------------------------------- */

    String describe() {
        return " type=" + typeName(alert.type) + " text=" + StubLog.quote(alert.text)
            + " timeout=" + (alert.time == Alert.FOREVER ? "FOREVER" : String.valueOf(alert.time));
    }

    boolean lCallShow() {
        boolean wasShown = (state == SHOWN);
        wrapListener();
        if (!super.lCallShow()) {
            return false;
        }
        if (wasShown) {
            return true;
        }
        StubLog.line("alert" + describe() + " title=" + StubLog.quote(alert.title));
        if (alert.type != null) {
            currentDisplay.playAlertSound(alert.type);
        }
        if (alert.time != Alert.FOREVER && !lIsModal()) {
            schedule(alert.time);
        }
        return true;
    }

    void lCallHide() {
        cancelTimer();
        lRemoveAlertPeer();
        super.lCallHide();
    }

    void lCallFreeze() {
        cancelTimer();
        super.lCallFreeze();
    }

    public boolean uIsScrollNative() {
        return true;
    }

    public void uCallSizeChanged(int w, int h) {
        super.uCallSizeChanged(w, h);
        Displayable returnScreen = alert.getReturnScreen();
        if (returnScreen != null) {
            returnScreen.displayableLF.uCallSizeChanged(w, h);
        }
    }

    private void cancelTimer() {
        if (timerTask != null) {
            timerTask.cancel();
            timerTask = null;
        }
    }

    private void schedule(int timeout) {
        if (timeoutTimer == null) {
            timeoutTimer = new Timer();
        }
        timerTask = new TimeoutTask();
        timeoutTimer.schedule(timerTask, timeout);
    }

    /**
     * Display calls the alert's listener for its commands and Alert for the
     * timeout; the wrapper notices a command that left the alert current.
     */
    private void wrapListener() {
        CommandListener listener = alert.listener;
        if (listener != null && !(listener instanceof ReshowListener)) {
            alert.listener = new ReshowListener(listener);
        }
    }

    private static int alertKind(AlertType t) {
        if (t == null || t.equals(AlertType.INFO)) {
            return LFNative.ALERT_INFO;
        } else if (t.equals(AlertType.CONFIRMATION)) {
            return LFNative.ALERT_CONFIRMATION;
        } else if (t.equals(AlertType.ERROR)) {
            return LFNative.ALERT_ERROR;
        }
        return LFNative.ALERT_WARNING; // WARNING, ALARM
    }

    private static String typeName(AlertType t) {
        if (t == null) {
            return "none";
        } else if (t.equals(AlertType.INFO)) {
            return "INFO";
        } else if (t.equals(AlertType.WARNING)) {
            return "WARNING";
        } else if (t.equals(AlertType.ERROR)) {
            return "ERROR";
        } else if (t.equals(AlertType.ALARM)) {
            return "ALARM";
        } else if (t.equals(AlertType.CONFIRMATION)) {
            return "CONFIRMATION";
        }
        return "other";
    }

    private class TimeoutTask extends TimerTask {
        public void run() {
            alert.uNotifyTimeout();
        }
    }

    /*
     * After the listener, a still current alert whose peer a command
     * removed is shown again. callSerially() runs after any screen change
     * the listener asked for, so a dismissed alert is hidden by then.
     */
    private final class ReshowListener implements CommandListener, Runnable {
        private final CommandListener inner;

        ReshowListener(CommandListener inner) {
            this.inner = inner;
        }

        public void commandAction(Command c, Displayable d) {
            try {
                inner.commandAction(c, d);
            } finally {
                Display display;
                synchronized (Display.LCDUILock) {
                    display = (state == SHOWN && !LFNative.HEADLESS
                               && !lPeerAlive()) ? currentDisplay : null;
                }
                if (display != null) {
                    display.callSerially(this);
                }
            }
        }

        public void run() {
            synchronized (Display.LCDUILock) {
                if (state == SHOWN && !lPeerAlive()) {
                    lShowAsAlert();
                }
            }
        }
    }
}
