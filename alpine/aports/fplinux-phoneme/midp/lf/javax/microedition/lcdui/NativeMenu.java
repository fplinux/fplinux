/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * fplinux-phoneme LCDUI look and feel: the command menu Display drives when
 * Chameleon is not built. The runtime draws the soft keys and the Options
 * menu itself, so this class only forwards each new command set of the
 * current Displayable to its peer (DisplayableLFImpl) and logs it.
 */
package javax.microedition.lcdui;

class NativeMenu {
    private static boolean inMenu;
    private static String lastSet = "";

    static void updateCommands(Command[] itemCommands, int numItemCommands,
                               Command[] commands, int numCommands) {
        DisplayableLFImpl.lCommandsChanged(commands);
        StringBuffer b = new StringBuffer();
        for (int i = 0; i < numCommands; i++) {
            b.append(i == 0 ? "" : ", ").append(StubLog.command(commands[i]));
        }
        for (int i = 0; i < numItemCommands; i++) {
            b.append(i == 0 ? " | item: " : ", ").append(StubLog.command(itemCommands[i]));
        }
        String set = b.toString();
        if (!set.equals(lastSet)) {
            lastSet = set;
            StubLog.line("commands [" + set + "]");
        }
    }

    static void show() {
        inMenu = true;
    }

    static void dismiss() {
        inMenu = false;
    }

    static boolean getState() {
        return inMenu;
    }

    static void clearState() {
        inMenu = false;
    }
}
