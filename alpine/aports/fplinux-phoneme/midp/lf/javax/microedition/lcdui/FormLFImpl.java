/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * fplinux-phoneme LCDUI look and feel: Form on a libfplinux-ui form. Item i
 * of the Form is row i of the peer, one item per row (layout directives are
 * ignored). The runtime moves focus, scrolls and edits; user changes come
 * back as PEER_CHANGED events (FACTORY.md section 4) and reach the
 * ItemStateListener. Item commands are the items' own native commands with
 * this LF's item listener, so lGetCurrentItem() is null and Display only
 * handles screen commands. The focused item survives a rebuilt peer.
 */
package javax.microedition.lcdui;

class FormLFImpl extends DisplayableLFImpl implements FormLF {

    Form form;

    /* The item to focus when the peer is (re)built, or null. */
    private Item focusItem;

    FormLFImpl(Form form) {
        super(form);
        this.form = form;
    }

    String describe() {
        StringBuffer b = new StringBuffer(" items=").append(form.numOfItems);
        if (LFNative.HEADLESS) {
            // Without a screen the log names each item, with a StringItem's text.
            for (int i = 0; i < form.numOfItems; i++) {
                Item item = form.items[i];
                String type = StubLog.name(item);
                b.append(" [").append(type.substring(type.lastIndexOf('.') + 1))
                    .append(' ').append(StubLog.quote(item.label));
                if (item instanceof StringItem) {
                    b.append(' ').append(StubLog.quote(((StringItem) item).str));
                }
                b.append(']');
            }
        }
        return b.toString();
    }

    int lIndexOf(Item item) {
        for (int i = 0; i < form.numOfItems; i++) {
            if (form.items[i] == item) {
                return i;
            }
        }
        return -1;
    }

    /* ---- the peer ------------------------------------------------------- */

    void createNativeResource() {
        nativeId = FormPeer.createForm0(lPeerTitle());
        if (nativeId == 0) {
            return;
        }
        for (int i = 0; i < form.numOfItems; i++) {
            lAppendItemPeer(form.items[i], i);
        }
        int focus = focusItem == null ? -1 : lIndexOf(focusItem);
        if (focus >= 0) {
            FormPeer.setFocus0(nativeId, focus);
        }
    }

    void lPeerDeleting() {
        int focus = FormPeer.getFocus0(nativeId);
        focusItem = focus >= 0 && focus < form.numOfItems
            ? form.items[focus] : null;
        lSyncItems();
    }

    /** Appends the item's row and moves it to index. */
    private void lAppendItemPeer(Item item, int index) {
        ItemLFImpl lf = (ItemLFImpl) item.itemLF;
        lf.peerCommands = 0;
        int row = lf.lAppendPeer(nativeId);
        if (row < 0) {
            FormPeer.log("item " + index + " (" + StubLog.name(item)
                         + ") has no native row; hidden");
            row = FormPeer.appendPlaceholder0(nativeId);
            if (row < 0) {
                FormPeer.log("out of memory: rows after " + index
                             + " are misplaced");
                return;
            }
        }
        if (row != index) {
            FormPeer.moveItem0(nativeId, row, index);
        }
        lf.lAddPeerCommands(nativeId, index);
    }

    /** Replaces an item's row in place, keeping its focus. */
    void lRebuildItem(Item item, boolean sync) {
        int index = lIndexOf(item);
        if (nativeId == 0 || index < 0) {
            return;
        }
        if (sync) {
            ((ItemLFImpl) item.itemLF).lSyncFromPeer(nativeId, index);
        }
        boolean focused = FormPeer.getFocus0(nativeId) == index;
        FormPeer.deleteItem0(nativeId, index);
        lAppendItemPeer(item, index);
        if (focused) {
            FormPeer.setFocus0(nativeId, index);
        }
    }

    private void lSyncItems() {
        for (int i = 0; i < form.numOfItems; i++) {
            ((ItemLFImpl) form.items[i].itemLF).lSyncFromPeer(nativeId, i);
        }
    }

    public void lCommitPendingInteraction() {
        if (nativeId != 0) {
            lSyncItems();
        }
    }

    /** The runtime draws and scrolls the screen; Display skips the buffer. */
    public boolean uIsScrollNative() {
        return true;
    }

    /* ---- FormLF ---------------------------------------------------------- */

    public Item lGetCurrentItem() {
        return null;
    }

    public void uItemMakeVisible(Item item) {
        if (item == null) {
            // Display.setCurrent(form) passes null: keep the restored focus.
            return;
        }
        synchronized (Display.LCDUILock) {
            focusItem = item;
            int index = lIndexOf(item);
            if (nativeId != 0 && index >= 0) {
                FormPeer.setFocus0(nativeId, index);
            }
        }
    }

    public void lSet(int itemNum, Item item) {
        if (nativeId == 0) {
            return;
        }
        boolean focused = FormPeer.getFocus0(nativeId) == itemNum;
        FormPeer.deleteItem0(nativeId, itemNum);
        lAppendItemPeer(item, itemNum);
        if (focused) {
            FormPeer.setFocus0(nativeId, itemNum);
        }
    }

    public void lInsert(int itemNum, Item item) {
        if (nativeId != 0) {
            lAppendItemPeer(item, itemNum);
        }
    }

    public void lDelete(int itemNum, Item deletedItem) {
        if (focusItem == deletedItem) {
            focusItem = null;
        }
        if (nativeId != 0) {
            FormPeer.deleteItem0(nativeId, itemNum);
        }
    }

    public void lDeleteAll() {
        focusItem = null;
        if (nativeId != 0) {
            FormPeer.deleteAll0(nativeId);
        }
    }

    public void uCallPeerStateChanged(int serial, int subType, int index,
                                      int hint) {
        Item changed = null;
        Item commandItem = null;
        Command command = null;
        ItemCommandListener listener = null;

        synchronized (Display.LCDUILock) {
            if (serial == 0 || serial != peerSerial || nativeId == 0
                || index < 0 || index >= form.numOfItems) {
                return;
            }
            Item item = form.items[index];
            ItemLFImpl lf = (ItemLFImpl) item.itemLF;
            if (subType == FormPeer.PEER_ITEM_CHANGED) {
                if (lf.lPeerChanged(nativeId, index)) {
                    changed = item;
                }
            } else if (subType == FormPeer.PEER_ITEM_COMMAND) {
                command = lf.lPeerCommand(hint);
                listener = item.commandListener;
                if (command == null || listener == null) {
                    return;
                }
                lf.lCommitPendingInteraction();
                commandItem = item;
            }
        }

        // SYNC NOTE: application code runs outside LCDUILock.
        if (changed != null) {
            form.uCallItemStateChanged(changed);
        }
        if (commandItem != null) {
            try {
                synchronized (Display.calloutLock) {
                    listener.commandAction(command, commandItem);
                }
            } catch (Throwable t) {
                Display.handleThrowable(t);
            }
        }
    }
}
