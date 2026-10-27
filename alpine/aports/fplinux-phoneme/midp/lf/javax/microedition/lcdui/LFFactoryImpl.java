/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * fplinux-phoneme LCDUI look and feel: the factory that
 * javax.microedition.lcdui.LFFactory instantiates for screens, Form items
 * and Canvas.
 */
package javax.microedition.lcdui;

class LFFactoryImpl extends LFFactory {

    AlertLF getAlertLF(Alert a) {
        return new AlertLFImpl(a);
    }

    CanvasLF getCanvasLF(Canvas c) {
        return new CanvasLFImpl(c);
    }

    FormLF getFormLF(Form f) {
        return new FormLFImpl(f);
    }

    /* The List's ChoiceGroup gets the List LF's own choice LF here. */
    FormLF getListLF(List list) {
        return new ListLFImpl(list);
    }

    FormLF getTextBoxFormLF(TextBox tb) {
        return new TextBoxLFImpl(tb);
    }

    ChoiceGroupLF getChoiceGroupLF(ChoiceGroup cg) {
        return new ChoiceGroupLFImpl(cg);
    }

    CustomItemLF getCustomItemLF(CustomItem ci) {
        return new CustomItemLFImpl(ci);
    }

    DateFieldLF getDateFieldLF(DateField df) {
        return new DateFieldLFImpl(df);
    }

    GaugeLF getGaugeLF(Gauge g) {
        return new GaugeLFImpl(g);
    }

    ImageItemLF getImageItemLF(ImageItem imgItem) {
        return new ImageItemLFImpl(imgItem);
    }

    StringItemLF getStringItemLF(StringItem strItem) {
        return new StringItemLFImpl(strItem);
    }

    SpacerLF getSpacerLF(Spacer spacer) {
        return new SpacerLFImpl(spacer);
    }

    TextFieldLF getTextFieldLF(TextField tf) {
        return new TextFieldLFImpl(tf);
    }

    TextFieldLF getTextBoxLF(TextField tf) {
        return new TextFieldLFImpl(tf);
    }

    TickerLF getTickerLF(Ticker ticker) {
        return new TickerLFImpl(ticker);
    }
}
