/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * fplinux-phoneme LCDUI look and feel: base of the Form items. An item is
 * one row of the owner's native form peer, at the item's index in the Form;
 * layout directives and preferred sizes are ignored. The Java model holds
 * the state; the peer is updated whenever it exists, and the user's changes
 * are copied back into the model when the runtime reports them, before a
 * command runs and before the peer is deleted.
 */
package javax.microedition.lcdui;

abstract class ItemLFImpl implements ItemLF {

    Item item;

    /* Item commands on the current peer, with ids 0 .. peerCommands - 1. */
    int peerCommands;

    ItemLFImpl(Item item) {
        this.item = item;
    }

    /* ---- the native item ---------------------------------------------- */

    /** The look and feel of the Form showing this item, or null. */
    FormLFImpl lForm() {
        Screen s = item.owner;
        if (s instanceof Form && s.displayableLF instanceof FormLFImpl) {
            return (FormLFImpl) s.displayableLF;
        }
        return null;
    }

    /** The owner's form peer, 0 without one. */
    int lPeer() {
        FormLFImpl f = lForm();
        return f == null ? 0 : f.nativeId;
    }

    /** The item's index in its Form, -1 outside a Form. */
    int lIndex() {
        FormLFImpl f = lForm();
        return f == null ? -1 : f.lIndexOf(item);
    }

    /** Appends the native counterpart to a form peer: its index, or -1. */
    abstract int lAppendPeer(int form);

    /**
     * Copies the user's changes in the peer into the model; true when the
     * model changed.
     */
    boolean lSyncFromPeer(int form, int index) {
        return false;
    }

    /**
     * The runtime reported a change by the user: copies it into the model;
     * true to notify the ItemStateListener. The runtime reports only user
     * changes, and an earlier getter may already have copied this one, so
     * every report notifies (as upstream lfplatform does).
     */
    boolean lPeerChanged(int form, int index) {
        lSyncFromPeer(form, index);
        return true;
    }

    /** Copies the peer's state into the model when there is a peer. */
    void lSync() {
        int form = lPeer();
        int index = lIndex();
        if (form != 0 && index >= 0) {
            lSyncFromPeer(form, index);
        }
    }

    /**
     * Replaces the native item, for a change the runtime cannot make in
     * place. sync first keeps the user's unsaved input; a structural model
     * change must not, because the peer no longer matches the model.
     */
    void lRebuildPeer(boolean sync) {
        FormLFImpl f = lForm();
        if (f != null) {
            f.lRebuildItem(item, sync);
        }
    }

    /* ---- item commands -------------------------------------------------- */

    void lAddPeerCommands(int form, int index) {
        FormLFImpl f = lForm();
        for (int i = 0; i < item.numCommands; i++) {
            Command c = item.commands[i];
            // The runtime gives OK to the first item command by priority.
            int priority = c == item.defaultCommand
                ? Integer.MIN_VALUE : c.getPriority();
            FormPeer.addItemCommand0(form, index, i, f.lPeerCommandType(c),
                                     DisplayableLFImpl.commandLabel(c),
                                     priority);
        }
        peerCommands = item.numCommands;
    }

    private void lUpdatePeerCommands() {
        int form = lPeer();
        int index = lIndex();
        if (form == 0 || index < 0) {
            return;
        }
        for (int i = 0; i < peerCommands; i++) {
            FormPeer.removeItemCommand0(form, index, i);
        }
        lAddPeerCommands(form, index);
    }

    /** The item command with a native id, or null. */
    Command lPeerCommand(int id) {
        return id >= 0 && id < item.numCommands ? item.commands[id] : null;
    }

    /* ---- ItemLF ------------------------------------------------------------ */

    int contentHeight() {
        int line = Font.getDefaultFont().getHeight();
        return item.label != null && item.label.length() > 0 ? 2 * line : line;
    }

    public int lGetPreferredWidth(int h) {
        if (item.lockedWidth != -1) {
            return item.lockedWidth;
        }
        FormLFImpl f = lForm();
        return f != null ? f.lGetWidth() : Display.WIDTH;
    }

    public int lGetPreferredHeight(int w) {
        return item.lockedHeight != -1 ? item.lockedHeight : contentHeight();
    }

    public int lGetMinimumWidth() {
        return 0;
    }

    public int lGetMinimumHeight() {
        return 0;
    }

    public void lSetLabel(String label) {
        lRebuildPeer(true);
    }

    public void lSetLayout(int layout) {
    }

    public void lAddCommand(Command cmd, int i) {
        lUpdatePeerCommands();
    }

    public void lRemoveCommand(Command cmd, int i) {
        lUpdatePeerCommands();
    }

    public void lSetPreferredSize(int width, int height) {
    }

    public void lSetDefaultCommand(Command cmd, int i) {
        lUpdatePeerCommands();
    }

    public void lSetOwner(Screen oldOwner) {
        // Form.insert, set and delete reach FormLFImpl, which builds the row.
    }

    public void lCommitPendingInteraction() {
        lSync();
    }

    public boolean isRequestedSizesValid() {
        return true;
    }
}
