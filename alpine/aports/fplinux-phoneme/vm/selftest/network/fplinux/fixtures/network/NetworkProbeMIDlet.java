// SPDX-License-Identifier: GPL-2.0-only
package fplinux.fixtures.network;

import java.io.InputStream;
import java.io.OutputStream;
import javax.microedition.io.Connector;
import javax.microedition.io.HttpConnection;
import javax.microedition.lcdui.Command;
import javax.microedition.lcdui.CommandListener;
import javax.microedition.lcdui.Display;
import javax.microedition.lcdui.Displayable;
import javax.microedition.lcdui.Form;
import javax.microedition.lcdui.StringItem;
import javax.microedition.midlet.MIDlet;

/** Controlled plain-HTTP probe through the public MIDP Connector API. */
public class NetworkProbeMIDlet extends MIDlet implements CommandListener {
    private final Command get = new Command("GET", Command.SCREEN, 1);
    private final Command redirect = new Command("Redirect", Command.SCREEN, 2);
    private final Command post = new Command("POST", Command.SCREEN, 3);
    private final Command slow = new Command("Slow", Command.SCREEN, 4);
    private final Command ui = new Command("UI", Command.SCREEN, 5);
    private final Command exit = new Command("Exit", Command.EXIT, 6);
    private Display display;
    private Form form;
    private StringItem status;
    private String base;
    private boolean busy;
    private boolean destroyed;
    private int uiCount;

    protected void startApp() {
        if (form == null) {
            base = getAppProperty("NetProbe-Base-URL");
            if (base == null || base.length() == 0) {
                base = "http://10.0.2.2:39081";
            }
            if (base.endsWith("/")) {
                base = base.substring(0, base.length() - 1);
            }
            display = Display.getDisplay(this);
            form = new Form("PM Network");
            form.append(new StringItem("Server", base));
            status = new StringItem("Result", "Ready. GET / Redirect / POST / Slow.\nUI stays usable during Slow; Home tests VM teardown.");
            form.append(status);
            form.addCommand(get);
            form.addCommand(redirect);
            form.addCommand(post);
            form.addCommand(slow);
            form.addCommand(ui);
            form.addCommand(exit);
            form.setCommandListener(this);
            System.out.println("PM-NET ready " + base);
        }
        display.setCurrent(form);
    }

    protected void pauseApp() {
    }

    protected synchronized void destroyApp(boolean unconditional) {
        // Deliberately no blocking I/O on the UI/lifecycle thread. The VM must
        // terminate its pending native request and reclaim process sockets.
        destroyed = true;
    }

    public void commandAction(Command command, Displayable screen) {
        if (command == exit) {
            destroyApp(true);
            notifyDestroyed();
            return;
        }
        if (command == ui) {
            uiCount++;
            status.setText("UI event " + uiCount + (isBusy() ? "; request running" : "; idle"));
            System.out.println("PM-NET ui " + uiCount);
            return;
        }
        final String kind = command == post ? "POST" : command == redirect ? "Redirect"
            : command == slow ? "Slow" : "GET";
        synchronized (this) {
            if (busy || destroyed) {
                return;
            }
            busy = true;
        }
        status.setText(kind + " running...");
        new Thread(new Runnable() {
            public void run() {
                probe(kind);
            }
        }).start();
    }

    private synchronized boolean isBusy() {
        return busy;
    }

    private void probe(String kind) {
        String path = kind.equals("POST") ? "/post" : kind.equals("Redirect") ? "/redirect"
            : kind.equals("Slow") ? "/slow" : "/probe";
        String result;
        long started = System.currentTimeMillis();
        System.out.println("PM-NET start " + kind + " " + base + path);
        try {
            result = request(base + path, kind.equals("POST"), kind.equals("Redirect"));
        } catch (Throwable error) {
            result = "ERROR " + error.toString();
        }
        final String message = kind + " " + result + "\nms=" + (System.currentTimeMillis() - started);
        System.out.println("PM-NET result " + message);
        synchronized (this) {
            busy = false;
            if (destroyed) {
                return;
            }
        }
        display.callSerially(new Runnable() {
            public void run() {
                synchronized (NetworkProbeMIDlet.this) {
                    if (!destroyed) {
                        status.setText(message);
                    }
                }
            }
        });
    }

    private String request(String url, boolean sendPost, boolean followRedirect) throws Exception {
        HttpConnection connection = null;
        InputStream input = null;
        OutputStream output = null;
        String target = null;
        String result = null;
        try {
            connection = (HttpConnection) Connector.open(url, Connector.READ_WRITE, true);
            connection.setRequestMethod(sendPost ? HttpConnection.POST : HttpConnection.GET);
            if (sendPost) {
                byte[] body = "pm-net-post=20261006".getBytes("UTF-8");
                connection.setRequestProperty("Content-Type", "application/x-www-form-urlencoded");
                connection.setRequestProperty("Content-Length", String.valueOf(body.length));
                output = connection.openOutputStream();
                output.write(body);
                output.close();
                output = null;
            }
            int code = connection.getResponseCode();
            String location = connection.getHeaderField("Location");
            System.out.println("PM-NET response " + code + " location=" + location);
            if (followRedirect && (code == 301 || code == 302 || code == 303 || code == 307)) {
                if (location == null) {
                    throw new java.io.IOException("redirect without Location");
                }
                target = location.startsWith("/") ? base + location : location;
            } else {
                input = connection.openInputStream();
                StringBuffer body = new StringBuffer();
                int next;
                while (body.length() < 4096 && (next = input.read()) != -1) {
                    body.append((char) next); // Controlled server bodies are ASCII.
                }
                result = "HTTP " + code + "\n" + body.toString();
            }
        } finally {
            if (input != null) { try { input.close(); } catch (Exception ignored) {} }
            if (output != null) { try { output.close(); } catch (Exception ignored) {} }
            if (connection != null) { try { connection.close(); } catch (Exception ignored) {} }
        }
        if (target != null) {
            return "redirect -> " + request(target, false, false);
        }
        return result;
    }
}
