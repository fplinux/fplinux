/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * fplinux-phoneme LCDUI look and feel: TextBox on a libfplinux-ui text box,
 * a full-screen multi-line field with the system text input. The runtime
 * makes OK a new line, offers OK commands on the left soft key or in
 * Options, and puts Clear on the right soft key while there is text (Back
 * returns when it is empty). The text lives in the peer and is copied into
 * the TextField model on demand (TextFieldLFImpl).
 */
package javax.microedition.lcdui;

class TextBoxLFImpl extends DisplayableLFImpl implements FormLF {

    TextBox textBox;

    TextBoxLFImpl(TextBox textBox) {
        super(textBox);
        this.textBox = textBox;
    }

    String describe() {
        return " text=" + StubLog.quote(textBox.textField.buffer.toString());
    }

    void createNativeResource() {
        TextField tf = textBox.textField;
        nativeId = FormPeer.createTextBox0(lPeerTitle(), tf.buffer.toString(),
                                           tf.buffer.capacity(),
                                           FormPeer.textConstraint(tf.constraints));
        if (nativeId != 0) {
            ((TextFieldLFImpl) tf.textFieldLF).lBoxPeerCreated();
        }
    }

    void lPeerDeleting() {
        textBox.textField.textFieldLF.lUpdateContents();
    }

    public void lCommitPendingInteraction() {
        textBox.textField.textFieldLF.lUpdateContents();
    }

    /** The runtime draws and scrolls the screen; Display skips the buffer. */
    public boolean uIsScrollNative() {
        return true;
    }

    /* ---- FormLF: a TextBox has no items -------------------------------- */

    public Item lGetCurrentItem() {
        return null;
    }

    public void uItemMakeVisible(Item item) {
    }

    public void lSet(int itemNum, Item item) {
    }

    public void lInsert(int itemNum, Item item) {
    }

    public void lDelete(int itemNum, Item deletedItem) {
    }

    public void lDeleteAll() {
    }

    public void uCallPeerStateChanged(int serial, int subType, int index,
                                      int hint) {
    }
}
