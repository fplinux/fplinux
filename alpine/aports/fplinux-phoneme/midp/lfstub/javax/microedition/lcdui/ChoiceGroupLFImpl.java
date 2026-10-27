/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * fplinux-phoneme headless LCDUI stub: ChoiceGroup (and List) look and feel.
 * Keeps the single selection of IMPLICIT, EXCLUSIVE and POPUP groups like the
 * upstream Chameleon implementation; MULTIPLE uses the element flags.
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

    int contentHeight() {
        return Math.max(1, cg.numOfEls) * Font.getDefaultFont().getHeight();
    }

    public void lInsert(int elementNum, String stringPart, Image imagePart) {
        if (cg.choiceType != Choice.MULTIPLE) {
            if (selectedIndex == -1) {
                selectedIndex = 0;
                cg.cgElements[0].setSelected(true);
            } else if (elementNum <= selectedIndex) {
                selectedIndex++;
            }
        }
    }

    public void lDelete(int elementNum) {
        if (cg.numOfEls == 0) {
            selectedIndex = -1;
            return;
        }
        if (cg.choiceType != Choice.MULTIPLE) {
            if (elementNum < selectedIndex) {
                selectedIndex--;
            } else if (elementNum == selectedIndex && selectedIndex == cg.numOfEls) {
                selectedIndex = cg.numOfEls - 1;
            }
            cg.cgElements[selectedIndex].setSelected(true);
        }
    }

    public void lDeleteAll() {
        selectedIndex = -1;
    }

    public void lSet(int elementNum, String stringPart, Image imagePart) {
    }

    public void lSetSelectedIndex(int elementNum, boolean selected) {
        if (cg.choiceType == Choice.MULTIPLE) {
            cg.cgElements[elementNum].setSelected(selected);
        } else if (selected) {
            if (selectedIndex >= 0 && selectedIndex < cg.numOfEls) {
                cg.cgElements[selectedIndex].setSelected(false);
            }
            selectedIndex = elementNum;
            cg.cgElements[elementNum].setSelected(true);
        }
    }

    public void lSetSelectedFlags(boolean[] selectedArray) {
        if (cg.choiceType != Choice.MULTIPLE) {
            for (int i = 0; i < cg.numOfEls; i++) {
                if (cg.cgElements[i].selected) {
                    selectedIndex = i;
                    return;
                }
            }
        }
    }

    public void lSetFitPolicy(int fitPolicy) {
    }

    public void lSetFont(int elementNum, Font font) {
    }

    public int lGetSelectedIndex() {
        return selectedIndex;
    }

    public int lGetSelectedFlags(boolean[] selectedArray_return) {
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
        return cg.cgElements[elementNum].selected;
    }

    public Font getDefaultFont() {
        return Font.getDefaultFont();
    }
}
