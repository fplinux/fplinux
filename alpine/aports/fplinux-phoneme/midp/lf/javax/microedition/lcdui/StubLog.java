/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * fplinux-phoneme headless LCDUI stub: one System.out line per observable
 * LCDUI event, each starting with "[lcdui] ", so tests can follow a MIDlet
 * without a screen.
 */
package javax.microedition.lcdui;

final class StubLog {
    private static final String PREFIX = "[lcdui] ";

    private static int paints;
    private static int repaints;

    private StubLog() {
    }

    static void line(String text) {
        System.out.println(PREFIX + text);
    }

    /**
     * First three events and then every 100th one are logged
     * (flushGraphics is logged the same way by GameCanvasLFImpl).
     */
    private static boolean sampled(int n) {
        return n <= 3 || n % 100 == 0;
    }

    static synchronized void paint(Object owner) {
        paints++;
        if (sampled(paints)) {
            line("paint " + name(owner) + " n=" + paints);
        }
    }

    static synchronized void repaint(Object owner) {
        repaints++;
        if (sampled(repaints)) {
            line("repaint " + name(owner) + " n=" + repaints);
        }
    }

    static String name(Object o) {
        return o == null ? "null" : o.getClass().getName();
    }

    static String quote(String s) {
        if (s == null) {
            return "null";
        }
        StringBuffer b = new StringBuffer(s.length() + 2);
        b.append('"');
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if (c == '\n') {
                b.append("\\n");
            } else if (c == '"' || c == '\\') {
                b.append('\\').append(c);
            } else if (c < ' ') {
                b.append(' ');
            } else {
                b.append(c);
            }
        }
        return b.append('"').toString();
    }

    static String commandType(int type) {
        switch (type) {
        case Command.SCREEN: return "SCREEN";
        case Command.BACK:   return "BACK";
        case Command.CANCEL: return "CANCEL";
        case Command.OK:     return "OK";
        case Command.HELP:   return "HELP";
        case Command.STOP:   return "STOP";
        case Command.EXIT:   return "EXIT";
        case Command.ITEM:   return "ITEM";
        default:             return String.valueOf(type);
        }
    }

    static String command(Command c) {
        if (c == null) {
            return "null";
        }
        return quote(c.getLabel()) + " type=" + commandType(c.getCommandType())
            + " priority=" + c.getPriority();
    }
}
