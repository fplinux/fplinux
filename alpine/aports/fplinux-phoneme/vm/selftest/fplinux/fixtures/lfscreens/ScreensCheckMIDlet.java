// SPDX-License-Identifier: GPL-2.0-only
package fplinux.fixtures.lfscreens;

import javax.microedition.lcdui.Alert;
import javax.microedition.lcdui.AlertType;
import javax.microedition.lcdui.Choice;
import javax.microedition.lcdui.Command;
import javax.microedition.lcdui.CommandListener;
import javax.microedition.lcdui.Display;
import javax.microedition.lcdui.Displayable;
import javax.microedition.lcdui.Gauge;
import javax.microedition.lcdui.Graphics;
import javax.microedition.lcdui.Image;
import javax.microedition.lcdui.List;
import javax.microedition.midlet.MIDlet;

/**
 * Self-test of the List and Alert look and feel, driven by a traced
 * headless run (FPLINUX_PM_LF_TRACE, FPLINUX_PM_LF_SCRIPT in run.sh): lists
 * of the three kinds, a select command, list edits, alerts with a timeout
 * of FOREVER, a gauge indicator and a command that keeps the alert (and
 * retitles it while the runtime has removed it). Prints
 * "PM-LFSCREENS " lines for the model side.
 */
public class ScreensCheckMIDlet extends MIDlet implements CommandListener {
    private final Command exit = new Command("Exit", Command.EXIT, 1);
    private final Command help = new Command("Help", Command.HELP, 1);
    private final Command info = new Command("Info", Command.SCREEN, 1);
    private final Command edit = new Command("Edit", Command.SCREEN, 2);
    private final Command drop = new Command("Drop", Command.SCREEN, 3);
    private final Command open = new Command("Open", Command.ITEM, 1);
    private final Command done = new Command("Done", Command.OK, 1);
    private final Command back = new Command("Back", Command.BACK, 1);
    private final Command again = new Command("Again", Command.SCREEN, 1);
    private final Command ok = new Command("OK", Command.OK, 1);

    private Display display;
    private List menu;
    private List pick;
    private List multi;
    private Alert modal;

    private static void log(String text) {
        System.out.println("PM-LFSCREENS " + text);
    }

    protected void startApp() {
        if (display != null) {
            return;
        }
        display = Display.getDisplay(this);
        Image icon = Image.createImage(8, 8);
        Graphics g = icon.getGraphics();
        g.setColor(0x2060c0);
        g.fillRect(0, 0, 8, 8);
        menu = new List("Menu", Choice.IMPLICIT, new String[] {"Pick", "Multi", "Gauge"},
                        new Image[] {Image.createImage(icon), null, null});
        menu.addCommand(exit);
        menu.addCommand(help);
        menu.addCommand(info);
        menu.addCommand(edit);
        menu.addCommand(drop);
        menu.setSelectCommand(open);
        menu.setCommandListener(this);
        log("start");
        display.setCurrent(menu);
    }

    protected void pauseApp() {
    }

    protected void destroyApp(boolean unconditional) {
    }

    private List chooser(String title, int type, String[] rows) {
        List list = new List(title, type, rows, null);
        list.addCommand(done);
        list.addCommand(back);
        list.setCommandListener(this);
        return list;
    }

    private void picked(String text) {
        log(text);
        Alert alert = new Alert("Picked", text, null, AlertType.INFO);
        alert.setTimeout(Alert.FOREVER);
        display.setCurrent(alert, menu);
    }

    public void commandAction(Command c, Displayable d) {
        log("command " + c.getLabel() + " on " + d.getTitle());
        if (d == menu) {
            menuCommand(c);
        } else if (d == pick || d == multi) {
            if (c == back) {
                display.setCurrent(menu);
            } else if (d == pick) {
                picked("selected " + pick.getSelectedIndex());
            } else {
                boolean[] flags = new boolean[multi.size()];
                multi.getSelectedFlags(flags);
                StringBuffer b = new StringBuffer("flags");
                for (int i = 0; i < flags.length; i++) {
                    b.append(flags[i] ? " 1" : " 0");
                }
                picked(b.toString());
            }
        } else if (d == modal) {
            if (c == again) {
                /* The runtime has removed the alert; it is shown again. */
                modal.setTitle("Info 2");
                modal.setString("Second");
            } else {
                display.setCurrent(menu);
            }
        }
    }

    private void menuCommand(Command c) {
        if (c == open) {
            int index = menu.getSelectedIndex();
            log("select " + index + " " + menu.getString(index));
            if (index == 0) {
                pick = chooser("Pick", Choice.EXCLUSIVE, new String[] {"A", "B", "C"});
                pick.setSelectedIndex(2, true);
                display.setCurrent(pick);
            } else if (index == 1) {
                multi = chooser("Multi", Choice.MULTIPLE, new String[] {"X", "Y", "Z"});
                multi.setSelectedFlags(new boolean[] {true, false, false});
                display.setCurrent(multi);
            } else {
                Alert alert = new Alert("Gauge", "Two of five", null, null);
                alert.setIndicator(new Gauge(null, false, 5, 2));
                alert.setTimeout(Alert.FOREVER);
                display.setCurrent(alert, menu);
            }
        } else if (c == info) {
            modal = new Alert("Info", "First", null, AlertType.CONFIRMATION);
            modal.addCommand(again);
            modal.addCommand(ok);
            modal.setCommandListener(this);
            display.setCurrent(modal, menu);
        } else if (c == edit) {
            menu.insert(0, "Top", null);
            menu.set(2, "Multi*", null);
            log("rows " + menu.size() + " selected " + menu.getSelectedIndex());
        } else if (c == drop) {
            menu.delete(0);
            log("rows " + menu.size() + " selected " + menu.getSelectedIndex());
        } else if (c == exit) {
            notifyDestroyed();
        }
    }
}
