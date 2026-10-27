/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * fplinux-phoneme LCDUI look and feel: Spacer (P2). The runtime places one
 * item per row with its own spacing, so a Spacer is a hidden row that only
 * keeps the item indices.
 */
package javax.microedition.lcdui;

class SpacerLFImpl extends ItemLFImpl implements SpacerLF {

    Spacer spacer;

    SpacerLFImpl(Spacer spacer) {
        super(spacer);
        this.spacer = spacer;
    }

    int lAppendPeer(int form) {
        return FormPeer.appendPlaceholder0(form);
    }

    public int lGetMinimumWidth() {
        return spacer.width;
    }

    public int lGetMinimumHeight() {
        return spacer.height;
    }

    public int lGetPreferredWidth(int h) {
        return spacer.width;
    }

    public int lGetPreferredHeight(int w) {
        return spacer.height;
    }

    public void lSetMinimumSize(int minWidth, int minHeight) {
    }

    public void lSetLabel(String label) {
    }
}
