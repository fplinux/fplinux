/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * fplinux-phoneme LCDUI look and feel: ImageItem as a form image row,
 * centred and scaled down to the form's width. Without an image, or when
 * it cannot be shown, the row shows the alternate text. HYPERLINK and
 * BUTTON work as for StringItem.
 */
package javax.microedition.lcdui;

class ImageItemLFImpl extends ItemLFImpl implements ImageItemLF {

    ImageItem imgItem;

    ImageItemLFImpl(ImageItem imgItem) {
        super(imgItem);
        this.imgItem = imgItem;
    }

    int contentHeight() {
        Image img = imgItem.immutableImg;
        return img != null ? img.getHeight() : super.contentHeight();
    }

    int lAppendPeer(int form) {
        Image img = imgItem.immutableImg;
        int index = -1;
        if (img != null) {
            index = FormPeer.appendImage0(form, item.label, img.getImageData());
        }
        if (index < 0) {
            index = FormPeer.appendString0(form, item.label, imgItem.altText);
        }
        if (index >= 0 && imgItem.appearanceMode != Item.PLAIN) {
            FormPeer.setAppearance0(form, index, imgItem.appearanceMode);
        }
        return index;
    }

    public void lSetImage(Image img) {
        lRebuildPeer(false);
    }

    public void lSetAltText(String altText) {
        lRebuildPeer(false);
    }
}
