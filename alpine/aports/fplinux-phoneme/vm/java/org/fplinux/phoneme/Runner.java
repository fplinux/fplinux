/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Launcher of one MIDlet suite in the fplinux-phoneme VM. It sets up MIDP
 * the way the stock single-VM AMS does, without the installer, suite
 * storage or push: the MIDlet JAR is on the classpath after the system
 * classes.
 */
package org.fplinux.phoneme;

import java.io.IOException;

import com.sun.j2me.security.AccessController;
import com.sun.midp.main.CldcAccessControlContext;
import com.sun.midp.events.EventQueue;
import com.sun.midp.events.EventTypes;
import com.sun.midp.events.NativeEvent;
import com.sun.midp.lcdui.DisplayAccess;
import com.sun.midp.lcdui.DisplayContainer;
import com.sun.midp.lcdui.ForegroundController;
import com.sun.midp.lcdui.LCDUIEnvironment;
import com.sun.midp.midlet.MIDletLoader;
import com.sun.midp.midlet.MIDletStateHandler;
import com.sun.midp.midlet.MIDletStateListener;
import com.sun.midp.midlet.MIDletSuite;
import com.sun.midp.midlet.MIDletSuiteExceptionListener;
import com.sun.midp.midlet.PlatformRequest;
import com.sun.midp.rms.RmsEnvironment;
import com.sun.midp.rms.SuiteContainer;
import com.sun.midp.security.ImplicitlyTrustedClass;
import com.sun.midp.security.SecurityInitializer;
import com.sun.midp.security.SecurityToken;

import javax.microedition.io.ConnectionNotFoundException;
import javax.microedition.lcdui.Displayable;
import javax.microedition.midlet.MIDlet;

public final class Runner {
    /* Exit status reported through fplinux_pm_vm_exit_code(). */
    private static final int EXIT_OK = 0;
    private static final int EXIT_SUITE_FAILED = 1;
    private static final int EXIT_NO_MIDLET = 2;
    private static final int EXIT_BAD_MANIFEST = 3;

    /* Requests from the app, see fplinux-pm-vm.c. */
    private static final int REQUEST_DESTROY = 1;
    private static final int REQUEST_SHOW = 2;
    private static final int REQUEST_HIDE = 3;

    /* The patched SecurityInitializer trusts exactly this class. */
    private static final class SecurityTrusted
        implements ImplicitlyTrustedClass {}

    private static final Object lock = new Object();
    private static boolean destroyRequested;
    private static boolean suiteRunning;
    private static boolean failed;
    private static int foregroundDisplayId = -1;
    private static EventQueue eventQueue;

    private Runner() {}

    /** Blocks until the app sends a request; returns REQUEST_*. */
    private static native int waitRequest0();

    /** Stops the VM with this status; never returns. */
    private static native void exit0(int status);

    public static void main(String[] args) {
        if (args.length != 1) {
            System.err.println("fplinux-phoneme: usage: Runner <MIDlet class>");
            exit0(EXIT_SUITE_FAILED);
            return;
        }
        exit0(run(args[0]));
    }

    private static int run(String midletClass) {
        SecurityToken token =
            SecurityInitializer.requestToken(new SecurityTrusted());
        eventQueue = EventQueue.getEventQueue(token);
        Foreground foreground = new Foreground();
        LCDUIEnvironment lcdui =
            new LCDUIEnvironment(token, eventQueue, 0, foreground);
        foreground.container = lcdui.getDisplayContainer();

        MIDletStateHandler handler =
            MIDletStateHandler.getMidletStateHandler();
        handler.initMIDletStateHandler(token, new StateListener(),
                                       new Loader(), new NoPlatformRequest());

        JarMIDletSuite suite = new JarMIDletSuite();
        try {
            suite.loadManifest();
        } catch (IOException e) {
            System.err.println("fplinux-phoneme: cannot read the manifest: "
                               + e.getMessage());
            return EXIT_BAD_MANIFEST;
        }
        AccessController.setAccessControlContext(
            new CldcAccessControlContext(suite));
        RmsEnvironment.init(token, new SingleSuiteContainer(suite));

        Thread requests = new Thread() {
            public void run() {
                serveRequests();
            }
        };
        requests.start();

        try {
            synchronized (lock) {
                if (destroyRequested) {
                    return EXIT_OK;
                }
                suiteRunning = true;
            }
            /* Returns when the last MIDlet of the suite is destroyed. */
            handler.startSuite(new ExceptionListener(), suite, 0,
                               midletClass);
        } catch (ClassNotFoundException e) {
            System.err.println("fplinux-phoneme: MIDlet class not found: "
                               + midletClass);
            return EXIT_NO_MIDLET;
        } catch (Throwable e) {
            System.err.println("fplinux-phoneme: cannot start "
                               + midletClass + ": " + e);
            return EXIT_NO_MIDLET;
        } finally {
            lcdui.shutDown();
        }
        synchronized (lock) {
            return failed ? EXIT_SUITE_FAILED : EXIT_OK;
        }
    }

    private static void serveRequests() {
        for (;;) {
            int request = waitRequest0();
            if (request == REQUEST_DESTROY) {
                synchronized (lock) {
                    destroyRequested = true;
                    if (!suiteRunning) {
                        continue;
                    }
                }
                MIDletStateHandler.getMidletStateHandler().destroySuite();
            } else if (request == REQUEST_SHOW || request == REQUEST_HIDE) {
                postVisibility(request == REQUEST_SHOW);
            }
        }
    }

    /* Display.showNotify/hideNotify run on the event thread, as upstream. */
    private static void postVisibility(boolean visible) {
        int displayId;
        synchronized (lock) {
            displayId = foregroundDisplayId;
        }
        if (displayId < 0) {
            return;
        }
        NativeEvent event = new NativeEvent(
            visible ? EventTypes.FOREGROUND_NOTIFY_EVENT
                    : EventTypes.BACKGROUND_NOTIFY_EVENT);
        event.intParam4 = displayId;
        eventQueue.post(event);
    }

    /**
     * One suite, no AMS: a foreground request is granted at once, and the
     * granted display receives the app's later visibility changes.
     */
    private static final class Foreground implements ForegroundController {
        DisplayContainer container;

        public Displayable registerDisplay(int displayId, String owner) {
            return null;
        }

        public void requestForeground(int displayId, boolean isAlert) {
            if (container == null) {
                return;
            }
            DisplayAccess display = container.findDisplayById(displayId);
            if (display == null) {
                return;
            }
            synchronized (lock) {
                foregroundDisplayId = displayId;
            }
            display.getForegroundEventConsumer()
                .handleDisplayForegroundNotifyEvent();
        }

        public void requestBackground(int displayId) {}

        public void startPreempting(int displayId) {}

        public void stopPreempting(int displayId) {}
    }

    private static final class StateListener implements MIDletStateListener {
        public void midletPreStart(MIDletSuite suite, String className) {}

        public void midletCreated(MIDletSuite suite, String className,
                                  int externalAppId) {}

        public void preActivated(MIDletSuite suite, String className) {}

        public void midletActivated(MIDletSuite suite, MIDlet midlet) {}

        public void midletPaused(MIDletSuite suite, String className) {}

        public void midletPausedItself(MIDletSuite suite, String className) {}

        public void resumeRequest(MIDletSuite suite, String className) {}

        public void midletDestroyed(MIDletSuite suite, String className,
                                    MIDlet midlet) {}
    }

    private static final class Loader implements MIDletLoader {
        public MIDlet newInstance(MIDletSuite suite, String className)
            throws ClassNotFoundException, InstantiationException,
                   IllegalAccessException {
            return (MIDlet)Class.forName(className).newInstance();
        }
    }

    private static final class NoPlatformRequest implements PlatformRequest {
        public boolean dispatch(String url)
            throws ConnectionNotFoundException {
            throw new ConnectionNotFoundException(
                "platform requests are not supported");
        }
    }

    private static final class ExceptionListener
        implements MIDletSuiteExceptionListener {
        public void handleException(Throwable error) {
            synchronized (lock) {
                failed = true;
            }
            System.err.println("fplinux-phoneme: MIDlet failed: " + error);
            error.printStackTrace();
        }
    }

    /* RMS names record stores by suite; this VM runs exactly one suite. */
    private static final class SingleSuiteContainer implements SuiteContainer {
        private final String vendor;
        private final String name;

        SingleSuiteContainer(JarMIDletSuite suite) {
            vendor = suite.getProperty("MIDlet-Vendor");
            name = suite.getProperty("MIDlet-Name");
        }

        public int getCallersSuiteId() {
            return JarMIDletSuite.SUITE_ID;
        }

        public int getSuiteId(String vendorName, String suiteName) {
            if (vendor != null && name != null && vendor.equals(vendorName)
                && name.equals(suiteName)) {
                return JarMIDletSuite.SUITE_ID;
            }
            return MIDletSuite.UNUSED_SUITE_ID;
        }

        public String getSecureFilenameBase(int suiteId) {
            return JarMIDletSuite.filenameBase();
        }

        public int getStorageAreaId(int suiteId) {
            return 0;
        }
    }
}
