/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * fplinux-phoneme LCDUI look and feel: List as a native libfplinux-ui list
 * (IMPLICIT, EXCLUSIVE, MULTIPLE), with element images as row icons. The
 * List's ChoiceGroup gets this LF's own choice LF, so element changes reach
 * the peer directly. While the peer exists, the user's focus and marks live
 * in it: queries read them, and lPeerDeleting() copies them into the model.
 * OK on an IMPLICIT list delivers the select command (List.SELECT_COMMAND
 * by default) to the List's listener.
 */
package javax.microedition.lcdui;

class ListLFImpl extends DisplayableLFImpl implements FormLF {

    private final List list;
    private final ChoiceGroup cg;
    /** IMPLICIT and EXCLUSIVE: the selected element; -1 when empty. */
    private int selectedIndex = -1;
    /* The OK legend last given to the peer of serial selectLabelPeer. */
    private String selectLabel;
    private int selectLabelPeer;

    ListLFImpl(List list) {
        super(list);
        this.list = list;
        cg = list.cg;
        Elements elements = new Elements();
        cg.choiceGroupLF = elements;
        cg.itemLF = elements;
        if (cg.choiceType != Choice.MULTIPLE) {
            for (int i = 0; i < cg.numOfEls; i++) {
                if (cg.cgElements[i].selected) {
                    if (selectedIndex == -1) {
                        selectedIndex = i;
                    } else {
                        cg.cgElements[i].setSelected(false);
                    }
                }
            }
            if (selectedIndex == -1 && cg.numOfEls > 0) {
                selectedIndex = 0;
                cg.cgElements[0].setSelected(true);
            }
        }
    }

    private int nativeKind() {
        switch (cg.choiceType) {
        case Choice.EXCLUSIVE: return LFNative.LIST_EXCLUSIVE;
        case Choice.MULTIPLE:  return LFNative.LIST_MULTIPLE;
        default:               return LFNative.LIST_IMPLICIT;
        }
    }

    String describe() {
        return " elements=" + cg.numOfEls;
    }

    /* ---- peer ---------------------------------------------------------- */

    void createNativeResource() {
        int peer = LFNative.listNew0(lPeerTitle(), nativeKind());
        if (peer == 0) {
            return;
        }
        for (int i = 0; i < cg.numOfEls; i++) {
            LFNative.listInsert0(peer, i, cg.cgElements[i].stringEl);
            setIcon(peer, i);
            if (cg.choiceType != Choice.IMPLICIT && cg.cgElements[i].selected) {
                LFNative.listSetSelected0(peer, i, true);
            }
        }
        if (selectedIndex >= 0) {
            LFNative.listSetFocused0(peer, selectedIndex);
        }
        nativeId = peer;
    }

    void lPeerDeleting() {
        lPullSelection();
    }

    void lSyncPeerCommands() {
        super.lSyncPeerCommands();
        if (nativeId == 0 || cg.choiceType != Choice.IMPLICIT) {
            return;
        }
        Command select = list.selectCommand;
        String label = (select == null || select == List.SELECT_COMMAND)
            ? null : commandLabel(select);
        if (selectLabelPeer != peerSerial || (label == null
                ? selectLabel != null : !label.equals(selectLabel))) {
            LFNative.listSetSelectLabel0(nativeId, label);
            selectLabel = label;
            selectLabelPeer = peerSerial;
        }
    }

    public boolean uIsScrollNative() {
        return true;
    }

    /* An IMPLICIT list's select command is on OK, not again on a soft key. */
    boolean lPeerShowsCommand(Command c) {
        return cg.choiceType != Choice.IMPLICIT || c != list.selectCommand;
    }

    private void setIcon(int peer, int index) {
        Image image = cg.cgElements[index].imageEl;
        LFNative.listSetIcon0(peer, index, image == null ? null : image.getImageData());
    }

    /** Copies the peer's focus (IMPLICIT) or marks into the model. */
    private void lPullSelection() {
        if (nativeId == 0) {
            return;
        }
        int n = cg.numOfEls;
        if (cg.choiceType == Choice.IMPLICIT) {
            int focused = LFNative.listGetFocused0(nativeId);
            if (focused >= 0 && focused < n) {
                selectElement(focused);
            }
        } else if (cg.choiceType == Choice.EXCLUSIVE) {
            for (int i = 0; i < n; i++) {
                if (LFNative.listIsSelected0(nativeId, i)) {
                    selectElement(i);
                    break;
                }
            }
        } else {
            for (int i = 0; i < n; i++) {
                cg.cgElements[i].setSelected(LFNative.listIsSelected0(nativeId, i));
            }
        }
    }

    /** IMPLICIT and EXCLUSIVE: makes index the one selected element. */
    private void selectElement(int index) {
        if (selectedIndex >= 0 && selectedIndex < cg.numOfEls) {
            cg.cgElements[selectedIndex].setSelected(false);
        }
        selectedIndex = index;
        if (index >= 0 && index < cg.numOfEls) {
            cg.cgElements[index].setSelected(true);
        }
    }

    /*
     * After a row edit on the peer, the runtime has kept the focus (and an
     * EXCLUSIVE mark) on the same row or moved it as it does for the user;
     * the model follows. A list that gained its first row gets one.
     */
    private void lSelectionFromPeer() {
        if (cg.choiceType != Choice.MULTIPLE) {
            for (int i = 0; i < cg.numOfEls; i++) {
                cg.cgElements[i].setSelected(false);
            }
            selectedIndex = -1;
        }
        lPullSelection();
        if (cg.choiceType != Choice.MULTIPLE && selectedIndex == -1
            && cg.numOfEls > 0) {
            selectElement(0);
            pushSelection();
        }
    }

    /** Shows the model's selection on the peer after a programmatic change. */
    private void pushSelection() {
        if (nativeId == 0 || selectedIndex < 0) {
            return;
        }
        if (cg.choiceType == Choice.EXCLUSIVE) {
            LFNative.listSetSelected0(nativeId, selectedIndex, true);
        }
        LFNative.listSetFocused0(nativeId, selectedIndex);
    }

    /* ---- FormLF: List has no items ------------------------------------- */

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
        CommandListener listener;
        Command select;
        synchronized (Display.LCDUILock) {
            if (serial != peerSerial || serial == 0) {
                return;
            }
            if (subType != LFNative.PEER_LIST_SELECT) {
                lPullSelection();
                return;
            }
            if (index >= 0 && index < cg.numOfEls) {
                selectElement(index);
            }
            listener = list.listener;
            select = list.selectCommand;
            if (listener == null || select == null || state != SHOWN) {
                return;
            }
            StubLog.line("select " + (selectedIndex < 0 ? "none"
                : StubLog.quote(cg.cgElements[selectedIndex].stringEl)));
        }
        try {
            synchronized (Display.calloutLock) {
                listener.commandAction(select, list);
            }
        } catch (Throwable t) {
            Display.handleThrowable(t);
        }
    }

    /* ---- the List's ChoiceGroup LF -------------------------------------- */

    private final class Elements implements ChoiceGroupLF {

        public void lInsert(int index, String text, Image image) {
            if (nativeId != 0) {
                LFNative.listInsert0(nativeId, index, text);
                setIcon(nativeId, index);
                lSelectionFromPeer();
                return;
            }
            if (cg.choiceType != Choice.MULTIPLE) {
                if (selectedIndex == -1) {
                    selectedIndex = 0;
                    cg.cgElements[0].setSelected(true);
                } else if (index <= selectedIndex) {
                    selectedIndex++;
                }
            }
        }

        public void lDelete(int index) {
            if (nativeId != 0) {
                LFNative.listDelete0(nativeId, index);
                lSelectionFromPeer();
                return;
            }
            if (cg.numOfEls == 0) {
                selectedIndex = -1;
                return;
            }
            if (cg.choiceType != Choice.MULTIPLE) {
                if (index < selectedIndex) {
                    selectedIndex--;
                } else if (index == selectedIndex) {
                    if (selectedIndex >= cg.numOfEls) {
                        selectedIndex = cg.numOfEls - 1;
                    }
                    cg.cgElements[selectedIndex].setSelected(true);
                }
            }
        }

        public void lDeleteAll() {
            selectedIndex = -1;
            if (nativeId != 0) {
                LFNative.listDeleteAll0(nativeId);
            }
        }

        public void lSet(int index, String text, Image image) {
            if (nativeId != 0) {
                LFNative.listSet0(nativeId, index, text);
                setIcon(nativeId, index);
            }
        }

        public void lSetSelectedIndex(int index, boolean selected) {
            if (cg.choiceType == Choice.MULTIPLE) {
                cg.cgElements[index].setSelected(selected);
                if (nativeId != 0) {
                    LFNative.listSetSelected0(nativeId, index, selected);
                }
            } else if (selected) {
                selectElement(index);
                pushSelection();
            }
        }

        public void lSetSelectedFlags(boolean[] flags) {
            if (cg.choiceType != Choice.MULTIPLE || nativeId == 0) {
                return;
            }
            for (int i = 0; i < cg.numOfEls; i++) {
                LFNative.listSetSelected0(nativeId, i, flags[i]);
            }
        }

        public void lSetFitPolicy(int fitPolicy) {
        }

        public void lSetFont(int index, Font font) {
        }

        public int lGetSelectedIndex() {
            if (cg.choiceType == Choice.MULTIPLE) {
                return -1;
            }
            lPullSelection();
            return selectedIndex;
        }

        public int lGetSelectedFlags(boolean[] flags) {
            lPullSelection();
            int count = 0;
            for (int i = 0; i < cg.numOfEls; i++) {
                flags[i] = cg.cgElements[i].selected;
                if (flags[i]) {
                    count++;
                }
            }
            return count;
        }

        public boolean lIsSelected(int index) {
            lPullSelection();
            return cg.cgElements[index].selected;
        }

        public Font getDefaultFont() {
            return Font.getDefaultFont();
        }

        /* ItemLF: the group is never laid out or given commands. */

        public int lGetPreferredWidth(int h) {
            return Display.WIDTH;
        }

        public int lGetPreferredHeight(int w) {
            return Display.HEIGHT;
        }

        public int lGetMinimumWidth() {
            return 0;
        }

        public int lGetMinimumHeight() {
            return 0;
        }

        public void lSetLabel(String label) {
        }

        public void lSetLayout(int layout) {
        }

        public void lAddCommand(Command cmd, int i) {
        }

        public void lRemoveCommand(Command cmd, int i) {
        }

        public void lSetPreferredSize(int width, int height) {
        }

        public void lSetDefaultCommand(Command cmd, int i) {
        }

        public void lSetOwner(Screen oldOwner) {
        }

        public void lCommitPendingInteraction() {
        }

        public boolean isRequestedSizesValid() {
            return true;
        }
    }
}
