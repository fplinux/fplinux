/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * fplinux-phoneme LCDUI look and feel: StringItem as a read-only form row.
 * HYPERLINK and BUTTON become a link or a button, which the runtime makes
 * focusable while the item has commands (OK runs the first by priority, the
 * default command first). Fonts are the theme's (P2).
 */
package javax.microedition.lcdui;

class StringItemLFImpl extends ItemLFImpl implements StringItemLF {

    StringItem stringItem;

    StringItemLFImpl(StringItem stringItem) {
        super(stringItem);
        this.stringItem = stringItem;
    }

    int lAppendPeer(int form) {
        int index = FormPeer.appendString0(form, item.label, stringItem.str);
        if (index >= 0 && stringItem.appearanceMode != Item.PLAIN) {
            FormPeer.setAppearance0(form, index, stringItem.appearanceMode);
        }
        return index;
    }

    public void lSetText(String str) {
        int form = lPeer();
        int index = lIndex();
        if (form != 0 && index >= 0) {
            FormPeer.setString0(form, index, str);
        }
    }

    public void lSetFont(Font font) {
    }

    public Font getDefaultFont() {
        return Font.getDefaultFont();
    }
}
