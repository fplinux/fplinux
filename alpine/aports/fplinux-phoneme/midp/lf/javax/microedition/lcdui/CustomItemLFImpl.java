/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * fplinux-phoneme LCDUI look and feel: CustomItem (P2). The runtime has no
 * application-drawn form item yet, so the item is a hidden row: it is never
 * painted and gets no events. The first one is logged.
 */
package javax.microedition.lcdui;

import com.sun.midp.configurator.Constants;

class CustomItemLFImpl extends ItemLFImpl implements CustomItemLF {

    private static boolean logged;

    CustomItem customItem;

    CustomItemLFImpl(CustomItem ci) {
        super(ci);
        customItem = ci;
    }

    int lAppendPeer(int form) {
        if (!logged) {
            logged = true;
            FormPeer.log("CustomItem is not supported yet; hidden");
        }
        return FormPeer.appendPlaceholder0(form);
    }

    public void lSetLabel(String label) {
    }

    public void lRepaint() {
    }

    public void lRepaint(int x, int y, int w, int h) {
    }

    public void lInvalidate() {
    }

    public int lGetInteractionModes() {
        int result = CustomItem.TRAVERSE_HORIZONTAL | CustomItem.TRAVERSE_VERTICAL
            | CustomItem.KEY_PRESS | CustomItem.KEY_RELEASE;
        if (Constants.REPEAT_SUPPORTED) {
            result |= CustomItem.KEY_REPEAT;
        }
        if (Constants.POINTER_SUPPORTED) {
            result |= CustomItem.POINTER_PRESS | CustomItem.POINTER_RELEASE;
        }
        if (Constants.MOTION_SUPPORTED) {
            result |= CustomItem.POINTER_DRAG;
        }
        return result;
    }

    public void uCallSizeRefresh() {
    }
}
