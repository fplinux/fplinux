/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * fplinux-phoneme LCDUI look and feel: ChoiceGroup as a form choice row
 * (EXCLUSIVE radio rows, MULTIPLE check rows, POPUP one line that opens the
 * choices). The model keeps the selection: the single selection of
 * EXCLUSIVE and POPUP groups as in upstream, the element flags for
 * MULTIPLE. The runtime cannot edit the choices of a row, so a changed
 * element list rebuilds the row; element images and fonts are not shown
 * (P2), and an empty group shows only its label.
 *
 * A List builds its own group LF (ListLFImpl): the instance the factory
 * makes for a List's group only ever runs the constructor and lSetOwner.
 */
package javax.microedition.lcdui;

class ChoiceGroupLFImpl extends ItemLFImpl implements ChoiceGroupLF {

    ChoiceGroup cg;
    int selectedIndex = -1;

    ChoiceGroupLFImpl(ChoiceGroup cg) {
        super(cg);
        this.cg = cg;
        if (cg.choiceType != Choice.MULTIPLE && cg.numOfEls > 0) {
            for (int i = 0; i < cg.numOfEls; i++) {
                if (cg.cgElements[i].selected) {
                    selectedIndex = i;
                    break;
                }
            }
            if (selectedIndex == -1) {
                selectedIndex = 0;
                cg.cgElements[0].setSelected(true);
            }
        }
    }

    /* ---- the native row --------------------------------------------- */

    private int peerKind() {
        switch (cg.choiceType) {
        case Choice.MULTIPLE:
            return FormPeer.CHOICE_MULTIPLE;
        case Choice.POPUP:
            return FormPeer.CHOICE_POPUP;
        default:
            return FormPeer.CHOICE_EXCLUSIVE;
        }
    }

    int lAppendPeer(int form) {
        int count = cg.numOfEls;
        if (count == 0) {
            return FormPeer.appendString0(form, item.label, null);
        }
        String[] choices = new String[count];
        for (int i = 0; i < count; i++) {
            choices[i] = cg.cgElements[i].stringEl;
        }
        int index = FormPeer.appendChoice0(form, item.label, peerKind(),
                                           choices, count);
        if (index >= 0) {
            lPushSelection(form, index);
        }
        return index;
    }

    private void lPushSelection(int form, int index) {
        if (cg.choiceType == Choice.MULTIPLE) {
            for (int i = 0; i < cg.numOfEls; i++) {
                FormPeer.setChosen0(form, index, i, cg.cgElements[i].selected);
            }
        } else if (selectedIndex >= 0) {
            FormPeer.setChoice0(form, index, selectedIndex);
        }
    }

    boolean lSyncFromPeer(int form, int index) {
        int count = cg.numOfEls;
        if (count == 0) {
            return false;
        }
        if (cg.choiceType == Choice.MULTIPLE) {
            boolean changed = false;
            for (int i = 0; i < count; i++) {
                boolean chosen = FormPeer.isChosen0(form, index, i);
                if (chosen != cg.cgElements[i].selected) {
                    cg.cgElements[i].setSelected(chosen);
                    changed = true;
                }
            }
            return changed;
        }
        int chosen = FormPeer.getChoice0(form, index);
        if (chosen < 0 || chosen >= count || chosen == selectedIndex) {
            return false;
        }
        lSelect(chosen);
        return true;
    }

    /** Moves the single selection of EXCLUSIVE and POPUP groups. */
    private void lSelect(int elementNum) {
        if (selectedIndex >= 0 && selectedIndex < cg.numOfEls) {
            cg.cgElements[selectedIndex].setSelected(false);
        }
        selectedIndex = elementNum;
        cg.cgElements[elementNum].setSelected(true);
    }

    /* ---- ChoiceGroupLF ------------------------------------------------------ */

    public void lInsert(int elementNum, String stringPart, Image imagePart) {
        if (cg.choiceType != Choice.MULTIPLE) {
            if (selectedIndex == -1) {
                selectedIndex = 0;
                cg.cgElements[0].setSelected(true);
            } else if (elementNum <= selectedIndex) {
                selectedIndex++;
            }
        }
        lRebuildPeer(false);
    }

    public void lDelete(int elementNum) {
        if (cg.numOfEls == 0) {
            selectedIndex = -1;
        } else if (cg.choiceType != Choice.MULTIPLE) {
            if (elementNum < selectedIndex) {
                selectedIndex--;
            } else if (elementNum == selectedIndex
                       && selectedIndex == cg.numOfEls) {
                selectedIndex = cg.numOfEls - 1;
            }
            cg.cgElements[selectedIndex].setSelected(true);
        }
        lRebuildPeer(false);
    }

    public void lDeleteAll() {
        selectedIndex = -1;
        lRebuildPeer(false);
    }

    public void lSet(int elementNum, String stringPart, Image imagePart) {
        lRebuildPeer(false);
    }

    public void lSetSelectedIndex(int elementNum, boolean selected) {
        if (cg.choiceType == Choice.MULTIPLE) {
            cg.cgElements[elementNum].setSelected(selected);
        } else if (selected) {
            lSelect(elementNum);
        } else {
            return;
        }
        int form = lPeer();
        int index = lIndex();
        if (form != 0 && index >= 0 && cg.numOfEls > 0) {
            lPushSelection(form, index);
        }
    }

    public void lSetSelectedFlags(boolean[] selectedArray) {
        if (cg.choiceType != Choice.MULTIPLE) {
            for (int i = 0; i < cg.numOfEls; i++) {
                if (cg.cgElements[i].selected) {
                    selectedIndex = i;
                    break;
                }
            }
        }
        int form = lPeer();
        int index = lIndex();
        if (form != 0 && index >= 0 && cg.numOfEls > 0) {
            lPushSelection(form, index);
        }
    }

    public void lSetFitPolicy(int fitPolicy) {
    }

    public void lSetFont(int elementNum, Font font) {
    }

    public int lGetSelectedIndex() {
        lSync();
        return selectedIndex;
    }

    public int lGetSelectedFlags(boolean[] selectedArray_return) {
        lSync();
        int count = 0;
        for (int i = 0; i < cg.numOfEls; i++) {
            selectedArray_return[i] = cg.cgElements[i].selected;
            if (selectedArray_return[i]) {
                count++;
            }
        }
        return count;
    }

    public boolean lIsSelected(int elementNum) {
        lSync();
        return cg.cgElements[elementNum].selected;
    }

    public Font getDefaultFont() {
        return Font.getDefaultFont();
    }
}
