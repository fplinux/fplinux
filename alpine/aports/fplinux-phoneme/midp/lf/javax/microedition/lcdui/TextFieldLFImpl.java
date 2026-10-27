/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * fplinux-phoneme LCDUI look and feel: TextField (a single-line form field)
 * and the field of a TextBox (the text box peer), both edited with the
 * system text input: multi-tap, input languages, # and the symbol picker.
 * Every model change rewrites the whole native text, and the caret is
 * emulated at the end, where the runtime puts it. lUpdateContents() copies
 * the native text into the model only when it differs from what this LF
 * last left in the peer, i.e. when the user typed: a text the peer shows
 * differently (a form field shows newlines as spaces) or refuses keeps the
 * model's. EMAILADDR and URL edit as ANY (FormPeer.textConstraint).
 * PHONENUMBER also takes p and w, which the runtime offers for pauses. A
 * DECIMAL value is kept with '.', as MIDP specifies.
 * The runtime's code-point limit is an upper bound; peer synchronization
 * enforces the MIDP capacity in UTF-16 char units and keeps pairs whole.
 */
package javax.microedition.lcdui;

import com.sun.midp.lcdui.DynamicCharacterArray;
import com.sun.midp.lcdui.TextPolicy;

class TextFieldLFImpl extends ItemLFImpl implements TextFieldLF {

    TextField tf;

    /* The peer's text after this LF last changed it; null without a peer. */
    private String peerShown;
    /* The MIDP constraints the peer's text was typed with. */
    private int peerConstraints;
    /* A constraint the field refused for its text, to retry with the next text. */
    private int pendingConstraint = -1;

    TextFieldLFImpl(TextField tf) {
        super(tf);
        this.tf = tf;
    }

    /* ---- the native field --------------------------------------------- */

    /** The text box peer when this is a TextBox's field, else 0. */
    private int lBoxPeer() {
        Screen s = item.owner;
        if (s instanceof TextBox && s.displayableLF instanceof TextBoxLFImpl) {
            return ((TextBoxLFImpl) s.displayableLF).nativeId;
        }
        return 0;
    }

    private int peerConstraint() {
        return FormPeer.textConstraint(tf.constraints);
    }

    int lAppendPeer(int form) {
        // A form field is a single line.
        String text = tf.buffer.toString().replace('\n', ' ');
        int index = FormPeer.appendTextField0(form, item.label, text,
                                              tf.buffer.capacity(),
                                              peerConstraint());
        if (index < 0 && text.length() > 0) {
            FormPeer.log("text field " + StubLog.quote(item.label)
                         + ": text refused, shown empty");
            index = FormPeer.appendTextField0(form, item.label, "",
                                              tf.buffer.capacity(),
                                              peerConstraint());
        }
        if (index >= 0) {
            peerConstraints = tf.constraints;
            peerShown = FormPeer.getText0(form, index);
        }
        return index;
    }

    /** TextBoxLFImpl made the box peer from the model. */
    void lBoxPeerCreated() {
        peerConstraints = tf.constraints;
        peerShown = lPeerText();
    }

    /** The native text, or null without a peer. */
    private String lPeerText() {
        int box = lBoxPeer();
        if (box != 0) {
            return FormPeer.getBoxText0(box);
        }
        int form = lPeer();
        int index = lIndex();
        return form != 0 && index >= 0 ? FormPeer.getText0(form, index) : null;
    }

    private boolean lSetPeerConstraint(int constraint) {
        int box = lBoxPeer();
        if (box != 0) {
            return FormPeer.setBoxConstraint0(box, constraint);
        }
        int form = lPeer();
        int index = lIndex();
        return form == 0 || index < 0
            || FormPeer.setConstraint0(form, index, constraint);
    }

    /** Writes the model text into the peer. */
    private void lPushText() {
        String text = tf.buffer.toString();
        int box = lBoxPeer();
        int form = lPeer();
        int index = lIndex();
        boolean done;
        if (box != 0) {
            done = FormPeer.setBoxText0(box, text);
        } else if (form != 0 && index >= 0) {
            done = FormPeer.setText0(form, index, text.replace('\n', ' '));
        } else {
            return;
        }
        if (pendingConstraint >= 0) {
            int constraint = pendingConstraint;
            pendingConstraint = -1;
            if (!lSetPeerConstraint(constraint) && box == 0) {
                lRebuildPeer(false);
                return;
            }
        }
        if (!done) {
            FormPeer.log("text field " + StubLog.quote(item.label)
                         + ": text refused by the native field");
        }
        peerShown = lPeerText();
    }

    boolean lSyncFromPeer(int form, int index) {
        return lUpdateContents();
    }

    /* ---- TextFieldLF ------------------------------------------------------ */

    public boolean lUpdateContents() {
        String text = lPeerText();
        if (text == null || text.equals(peerShown)) {
            return false;
        }
        peerShown = text;
        int type = peerConstraints & TextField.CONSTRAINT_MASK;
        if (type == TextField.DECIMAL) {
            text = text.replace(',', '.');
        }
        if (type == TextField.NUMERIC || type == TextField.DECIMAL
            || type == TextField.PHONENUMBER) {
            // OK in a text box types a new line even in a number (runtime).
            text = withoutNewlines(text);
        }
        DynamicCharacterArray buffer = tf.buffer;
        if (text.equals(buffer.toString())) {
            return false;
        }
        boolean truncated = text.length() > buffer.capacity();
        if (truncated) {
            int length = buffer.capacity();
            // Java char capacity must never leave half a supplementary character.
            if (length > 0 && text.charAt(length - 1) >= '\ud800'
                && text.charAt(length - 1) <= '\udbff'
                && text.charAt(length) >= '\udc00'
                && text.charAt(length) <= '\udfff') {
                length--;
            }
            text = text.substring(0, length);
        }
        buffer.delete(0, buffer.length());
        buffer.insert(0, text);
        if (truncated) {
            // Keep the native text consistent with the char-limited Java model.
            lPushText();
        }
        return true;
    }

    private static String withoutNewlines(String text) {
        if (text.indexOf('\n') < 0) {
            return text;
        }
        StringBuffer b = new StringBuffer(text.length());
        for (int i = 0; i < text.length(); i++) {
            char c = text.charAt(i);
            if (c != '\n') {
                b.append(c);
            }
        }
        return b.toString();
    }

    public void lSetChars() {
        lPushText();
    }

    public void lInsert(char data[], int offset, int length, int position) {
        lPushText();
    }

    public void lDelete(int offset, int length) {
        lPushText();
    }

    public void lSetMaxSize(int maxSize) {
        if (lBoxPeer() != 0) {
            // A text box keeps its limit until it is shown again (P2).
            lPushText();
        } else {
            lRebuildPeer(false);
        }
    }

    public int lGetCaretPosition() {
        // The caret is at the end of the user's text.
        lUpdateContents();
        return tf.buffer.length();
    }

    public void lSetConstraints() {
        // TextField has set the constraints; keep the user's text first.
        lUpdateContents();
        peerConstraints = tf.constraints;
        int constraint = peerConstraint();
        pendingConstraint = -1;
        if (!lSetPeerConstraint(constraint)) {
            int box = lBoxPeer();
            if (!lValidate(tf.buffer, tf.constraints)) {
                // TextField clears the text next (lDelete), which applies it.
                pendingConstraint = constraint;
            } else if (box != 0) {
                FormPeer.log("text box: text refused by the new constraint");
                FormPeer.setBoxText0(box, "");
                FormPeer.setBoxConstraint0(box, constraint);
            } else {
                lRebuildPeer(false);
                return;
            }
        }
        String text = lPeerText();
        if (text != null) {
            peerShown = text;
        }
    }

    public boolean lValidate(DynamicCharacterArray buffer, int constraints) {
        if ((constraints & TextField.CONSTRAINT_MASK) == TextField.PHONENUMBER) {
            String s = buffer.toString();
            for (int i = 0; i < s.length(); i++) {
                char c = s.charAt(i);
                if ((c < '0' || c > '9') && "#*+pw".indexOf(c) < 0) {
                    return false;
                }
            }
            return true;
        }
        return TextPolicy.isValidString(buffer, constraints);
    }

    public void lSetInitialInputMode(String characterSubset) {
    }

    public void itemDeleted() {
    }
}
