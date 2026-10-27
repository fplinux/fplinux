// SPDX-License-Identifier: GPL-2.0-only
package fplinux.fixtures.lfform;

import java.util.Date;
import java.util.TimeZone;
import javax.microedition.lcdui.Choice;
import javax.microedition.lcdui.ChoiceGroup;
import javax.microedition.lcdui.Command;
import javax.microedition.lcdui.DateField;
import javax.microedition.lcdui.Display;
import javax.microedition.lcdui.Displayable;
import javax.microedition.lcdui.Form;
import javax.microedition.lcdui.Gauge;
import javax.microedition.lcdui.Image;
import javax.microedition.lcdui.ImageItem;
import javax.microedition.lcdui.Item;
import javax.microedition.lcdui.Spacer;
import javax.microedition.lcdui.StringItem;
import javax.microedition.lcdui.TextBox;
import javax.microedition.lcdui.TextField;
import javax.microedition.lcdui.Ticker;
import javax.microedition.midlet.MIDlet;

/**
 * Headless self-check of the LCDUI Form and TextBox model API. Shows a Form
 * with one item of each kind, changes the items through the public API and
 * prints one ASCII line "PM-LFFORM check value" per check. Every value is
 * what MIDP 2.0 requires, whatever the look and feel does with the screen.
 */
public class FormCheckMIDlet extends MIDlet implements Runnable {
    private static final TimeZone GMT = TimeZone.getTimeZone("GMT");
    /** 2001-09-09 01:46:40.123 GMT. */
    private static final long T = 1000000000123L;
    private static final char[] DIGITS = "0123456789AB".toCharArray();

    private boolean started;
    private Display display;
    private Form form;
    private TextBox box;
    private Item[] items;
    private int spacerIndex;
    private int customIndex;
    private Image mutable;
    private Image immutable;
    private Image rgb;

    private StringItem si;
    private StringItem link;
    private StringItem button;
    private ImageItem ii;
    private TextField tfAny;
    private TextField tfNum;
    private TextField tfDec;
    private TextField tfPhone;
    private TextField tfPass;
    private TextField tfPin;
    private TextField tfRo;
    private TextField tfMail;
    private TextField tfUrl;
    private TextField tfCaps;
    private ChoiceGroup cgEx;
    private ChoiceGroup cgMu;
    private ChoiceGroup cgPo;
    private Gauge gInt;
    private Gauge gProg;
    private Gauge gIndef;
    private DateField dDate;
    private DateField dTime;
    private DateField dDateTime;
    private Spacer spacer;
    private TinyItem custom;
    private Ticker ticker;

    protected void startApp() {
        if (!started) {
            started = true;
            display = Display.getDisplay(this);
            new Thread(this).start();
        }
    }

    protected void pauseApp() {
    }

    protected void destroyApp(boolean unconditional) {
    }

    public void run() {
        try {
            build();
            display.setCurrent(form);
            await(form);
            p("show", current());
            group("items", 0);
            group("textfield", 1);
            group("choicegroup", 2);
            group("gauge", 3);
            group("datefield", 4);
            group("p2", 5);
            group("form", 6);
            switchTo(box, "switch1");
            group("textbox", 7);
            switchTo(form, "switch2");
            switchTo(box, "switch3");
            switchTo(form, "switch4");
        } catch (Throwable t) {
            p("run.error", t.toString());
        }
        System.out.println("PM-LFFORM done");
        notifyDestroyed();
    }

    /** Runs one check group; an unexpected exception ends only that group. */
    private void group(String name, int n) {
        try {
            switch (n) {
            case 0: checkItems(); break;
            case 1: checkTextFields(); break;
            case 2: checkChoiceGroups(); break;
            case 3: checkGauges(); break;
            case 4: checkDateFields(); break;
            case 5: checkP2(); break;
            case 6: checkForm(); break;
            default: checkTextBox(); break;
            }
        } catch (Throwable t) {
            p(name + ".error", t.toString());
        }
    }

    private void build() {
        mutable = Shared.drawnMutable(8, 6);
        immutable = Image.createImage(mutable);
        rgb = Shared.rgbImage(4, 3);
        si = new StringItem("Label", "Text");
        link = new StringItem("Link", "go", Item.HYPERLINK);
        button = new StringItem(null, "Press", Item.BUTTON);
        ii = new ImageItem("Img", immutable, Item.LAYOUT_DEFAULT, "alt");
        tfAny = new TextField("Any", "Hello", 10, TextField.ANY);
        tfNum = new TextField("Num", "42", 6, TextField.NUMERIC);
        tfDec = new TextField("Dec", null, 10, TextField.DECIMAL);
        tfPhone = new TextField("Phone", null, 20, TextField.PHONENUMBER);
        tfPass = new TextField("Pass", "pw", 8, TextField.ANY | TextField.PASSWORD);
        tfPin = new TextField("Pin", null, 4, TextField.NUMERIC | TextField.PASSWORD);
        tfRo = new TextField("Ro", "Fixed text", 32, TextField.ANY | TextField.UNEDITABLE);
        tfMail = new TextField("Mail", null, 64, TextField.EMAILADDR);
        tfUrl = new TextField("Url", null, 64, TextField.URL);
        tfCaps = new TextField("Caps", null, 64, TextField.ANY | TextField.INITIAL_CAPS_SENTENCE);
        cgEx = new ChoiceGroup("Ex", Choice.EXCLUSIVE, new String[] {"A", "B", "C"}, null);
        cgMu = new ChoiceGroup("Mu", Choice.MULTIPLE, new String[] {"A", "B", "C", "D"}, null);
        cgPo = new ChoiceGroup("Po", Choice.POPUP, new String[] {"S", "M", "L"}, null);
        gInt = new Gauge("G", true, 10, 5);
        gProg = new Gauge("P", false, 20, 0);
        gIndef = new Gauge("I", false, Gauge.INDEFINITE, Gauge.CONTINUOUS_RUNNING);
        dDate = new DateField("D", DateField.DATE, GMT);
        dTime = new DateField("T", DateField.TIME, GMT);
        dDateTime = new DateField("DT", DateField.DATE_TIME, GMT);
        items = new Item[] {si, link, button, ii, tfAny, tfNum, tfDec, tfPhone, tfPass,
            tfPin, tfRo, tfMail, tfUrl, tfCaps, cgEx, cgMu, cgPo, gInt, gProg, gIndef,
            dDate, dTime, dDateTime};
        form = new Form("Check");
        for (int i = 0; i < items.length; i++) {
            form.append(items[i]);
        }
        spacer = new Spacer(10, 5);
        custom = new TinyItem("Custom");
        ticker = new Ticker("Tick");
        try { // A failure here is reported; the other checks still run.
            spacerIndex = form.append(spacer);
            customIndex = form.append(custom);
            form.setTicker(ticker);
        } catch (RuntimeException e) {
            p("p2.build.error", e.toString());
        }
        box = new TextBox("Box", "Line 1\nLine 2", 100, TextField.ANY);
    }

    private void checkItems() {
        p("si.text", q(si.getText()));
        p("si.label", q(si.getLabel()));
        p("si.mode", si.getAppearanceMode() + " " + link.getAppearanceMode() + " "
            + button.getAppearanceMode());
        p("button.label", q(button.getLabel()));
        si.setText("Line A\nLine B");
        p("si.settext", q(si.getText()));
        si.setText(null);
        p("si.settextnull", q(si.getText()));
        si.setLabel("New label");
        p("si.setlabel", q(si.getLabel()));
        si.setLabel(null);
        p("si.setlabelnull", q(si.getLabel()));
        try {
            new StringItem("x", "y", 3);
            p("si.badmode", "ok");
        } catch (RuntimeException e) {
            p("si.badmode", exc(e));
        }
        Command open = new Command("Open", Command.ITEM, 1);
        link.addCommand(open);
        link.setDefaultCommand(open);
        link.removeCommand(open);
        link.setDefaultCommand(open);
        p("link.commands", "ok");
        try {
            link.addCommand(null);
            p("link.addnull", "ok");
        } catch (RuntimeException e) {
            p("link.addnull", exc(e));
        }

        p("img.mutable", mutable.isMutable() + " " + size(mutable));
        p("img.immutable", immutable.isMutable() + " " + size(immutable));
        p("img.rgb", rgb.isMutable() + " " + size(rgb));
        try {
            immutable.getGraphics();
            p("img.immutable.graphics", "ok");
        } catch (RuntimeException e) {
            p("img.immutable.graphics", exc(e));
        }
        p("ii.image", (ii.getImage() != null) + " " + (ii.getImage() == immutable));
        p("ii.alt", q(ii.getAltText()));
        p("ii.mode", ii.getAppearanceMode());
        ii.setImage(rgb);
        p("ii.setrgb", ii.getImage() == rgb);
        ii.setImage(mutable);
        p("ii.setmutable", ii.getImage() != null);
        ii.setImage(null);
        p("ii.setnull", ii.getImage() == null);
        ii.setImage(immutable);
        try {
            new ImageItem(null, null, Item.LAYOUT_DEFAULT, null, 5);
            p("ii.badmode", "ok");
        } catch (RuntimeException e) {
            p("ii.badmode", exc(e));
        }
    }

    private void checkTextFields() {
        p("tf.any.init", q(tfAny.getString()));
        p("tf.any.size", tfAny.size());
        p("tf.any.max", tfAny.getMaxSize());
        p("tf.any.constraints", tfAny.getConstraints());
        set("tf.any.set", tfAny, "abc def");
        ins("tf.any.insert3", tfAny, "XY", 3);
        ins("tf.any.insertneg", tfAny, "<", -5);
        ins("tf.any.insertfull", tfAny, "!", 99);
        p("tf.any.after", q(tfAny.getString()));
        del("tf.any.delete", tfAny, 1, 3);
        del("tf.any.deletebad", tfAny, 5, 5);
        chars("tf.any.chars", tfAny, DIGITS, 2, 5);
        chars("tf.any.charsbig", tfAny, DIGITS, 0, 11);
        chars("tf.any.charsbad", tfAny, DIGITS, 10, 5);
        chars("tf.any.charsnull", tfAny, null, 0, 0);
        set("tf.any.set3", tfAny, "abc");
        char[] buf = new char[5];
        int n = tfAny.getChars(buf);
        p("tf.any.getchars", n + " " + q(new String(buf, 0, n)));
        try {
            tfAny.getChars(new char[2]);
            p("tf.any.getcharssmall", "ok");
        } catch (RuntimeException e) {
            p("tf.any.getcharssmall", exc(e));
        }
        p("tf.any.caret", tfAny.getCaretPosition());
        set("tf.any.setnull", tfAny, null);
        set("tf.any.set6", tfAny, "abcdef");
        max("tf.any.setmax4", tfAny, 4);
        max("tf.any.setmax0", tfAny, 0);
        p("tf.any.max4", tfAny.getMaxSize());

        p("tf.num.init", q(tfNum.getString()));
        set("tf.num.setneg", tfNum, "-123");
        set("tf.num.setalpha", tfNum, "12a");
        set("tf.num.setfrac", tfNum, "1.5");
        set("tf.num.setminus", tfNum, "-");
        p("tf.num.after", q(tfNum.getString()));
        set("tf.num.setempty", tfNum, "");
        set("tf.num.set42", tfNum, "42");
        ins("tf.num.insminus0", tfNum, "-", 0);
        ins("tf.num.insminus1", tfNum, "-", 1);
        ins("tf.num.insend", tfNum, "7", 99);
        del("tf.num.delminus", tfNum, 1, 3);
        p("tf.num.after2", q(tfNum.getString()));
        del("tf.num.del0", tfNum, 0, 1);
        chars("tf.num.chars", tfNum, "x12345".toCharArray(), 1, 5);
        chars("tf.num.charsbig", tfNum, "1234567".toCharArray(), 0, 7);
        ins("tf.num.insfull", tfNum, "67", 99);
        p("tf.num.constraints", tfNum.getConstraints());
        set("tf.num.setneg5", tfNum, "-5");
        max("tf.num.setmax1", tfNum, 1);

        p("tf.dec.init", q(tfDec.getString()));
        set("tf.dec.set", tfDec, "-12.50");
        set("tf.dec.setfrac", tfDec, ".5");
        set("tf.dec.settrail", tfDec, "5.");
        set("tf.dec.settwodots", tfDec, "1.2.3");
        set("tf.dec.setcomma", tfDec, "1,5");
        set("tf.dec.setdot", tfDec, ".");
        set("tf.dec.setminusdot", tfDec, "-.");
        set("tf.dec.setexp", tfDec, "1e5");
        p("tf.dec.after", q(tfDec.getString()));
        set("tf.dec.setpi", tfDec, "3.14");
        ins("tf.dec.insminus", tfDec, "-", 0);
        ins("tf.dec.insdot", tfDec, ".", 3);
        del("tf.dec.delwhole", tfDec, 1, 1);
        del("tf.dec.delfrac", tfDec, 2, 2);
        p("tf.dec.constraints", tfDec.getConstraints());

        set("tf.phone.set", tfPhone, "5551234");
        set("tf.phone.setsym", tfPhone, "+4930*31#");
        set("tf.phone.setalpha", tfPhone, "555-CALL");
        p("tf.phone.constraints", tfPhone.getConstraints());

        p("tf.pass.init", q(tfPass.getString()));
        set("tf.pass.set", tfPass, "s3cret");
        set("tf.pass.toolong", tfPass, "123456789");
        p("tf.pass.constraints", tfPass.getConstraints());
        set("tf.pin.set", tfPin, "1234");
        set("tf.pin.setalpha", tfPin, "12ab");
        p("tf.pin.constraints", tfPin.getConstraints());

        p("tf.ro.init", q(tfRo.getString()));
        set("tf.ro.set", tfRo, "changed");
        ins("tf.ro.ins", tfRo, "un", 0);
        p("tf.ro.constraints", tfRo.getConstraints());
        set("tf.mail.set", tfMail, "user@example.org");
        p("tf.mail.constraints", tfMail.getConstraints());
        set("tf.url.set", tfUrl, "http://example.org/a?b=1");
        p("tf.url.constraints", tfUrl.getConstraints());
        set("tf.caps.set", tfCaps, "hello world");
        p("tf.caps.constraints", tfCaps.getConstraints());

        constraints("tf.cons.any2num", tfAny, TextField.NUMERIC);
        set("tf.cons.set12", tfAny, "12");
        constraints("tf.cons.num2dec", tfAny, TextField.DECIMAL);
        constraints("tf.cons.dec2any", tfAny, TextField.ANY);
        constraints("tf.cons.bad", tfAny, 6);

        create("tf.new.badtext", "abc", 10, TextField.NUMERIC);
        create("tf.new.max0", null, 0, TextField.ANY);
        create("tf.new.toolong", "abcdef", 5, TextField.ANY);
        create("tf.new.badcons", null, 5, 6);
        create("tf.new.ok", "-7", 5, TextField.NUMERIC);
    }

    private void checkChoiceGroups() {
        count("cg.ex.init", cgEx);
        cgEx.setSelectedIndex(2, true);
        cg("cg.ex.sel2", cgEx);
        cgEx.setSelectedIndex(0, false);
        cg("cg.ex.unsel0", cgEx);
        p("cg.ex.issel", cgEx.isSelected(2) + " " + cgEx.isSelected(0));
        cgEx.insert(0, "Z", null);
        cg("cg.ex.insert0", cgEx);
        p("cg.ex.append", cgEx.append("D", null));
        cg("cg.ex.afterappend", cgEx);
        cgEx.delete(0);
        cg("cg.ex.delete0", cgEx);
        cgEx.set(2, "CC", null);
        p("cg.ex.set", q(cgEx.getString(2)) + " " + cgEx.getSelectedIndex());
        boolean[] big = {true, true, true, true, true, true};
        int n = cgEx.getSelectedFlags(big);
        p("cg.ex.bigflags", n + " " + bits(big));
        try {
            cgEx.getSelectedFlags(new boolean[2]);
            p("cg.ex.smallflags", "ok");
        } catch (RuntimeException e) {
            p("cg.ex.smallflags", exc(e));
        }
        try {
            cgEx.getString(9);
            p("cg.ex.getbad", "ok");
        } catch (RuntimeException e) {
            p("cg.ex.getbad", exc(e));
        }
        try {
            cgEx.setSelectedIndex(9, true);
            p("cg.ex.selbad", "ok");
        } catch (RuntimeException e) {
            p("cg.ex.selbad", exc(e));
        }
        cgEx.setSelectedFlags(new boolean[] {false, true, true, false});
        cg("cg.ex.setflags", cgEx);
        cgEx.setSelectedFlags(new boolean[4]);
        cg("cg.ex.setflagsnone", cgEx);
        cgEx.setSelectedIndex(2, true);
        cgEx.delete(2);
        count("cg.ex.delsel", cgEx);
        int ai = cgEx.append("Img", immutable);
        p("cg.ex.image", ai + " " + (cgEx.getImage(ai) != null));
        cgEx.deleteAll();
        cg("cg.ex.deleteall", cgEx);
        cgEx.append("Only", null);
        cg("cg.ex.only", cgEx);
        try {
            new ChoiceGroup("x", Choice.IMPLICIT);
            p("cg.implicit", "ok");
        } catch (RuntimeException e) {
            p("cg.implicit", exc(e));
        }
        cg("cg.empty", new ChoiceGroup("E", Choice.POPUP));

        cg("cg.mu.init", cgMu);
        cgMu.setSelectedIndex(0, true);
        cgMu.setSelectedIndex(2, true);
        cg("cg.mu.sel02", cgMu);
        cgMu.setSelectedIndex(0, false);
        cg("cg.mu.unsel0", cgMu);
        cgMu.setSelectedIndex(0, true);
        cgMu.insert(1, "X", null);
        cg("cg.mu.insert1", cgMu);
        cgMu.append("E", null);
        cg("cg.mu.append", cgMu);
        cgMu.delete(0);
        cg("cg.mu.delete0", cgMu);
        cgMu.set(2, "CC", null);
        p("cg.mu.set", q(cgMu.getString(2)) + " " + cgMu.isSelected(2));
        cgMu.setSelectedFlags(new boolean[] {true, true, false, false, true});
        cg("cg.mu.setflags", cgMu);
        cgMu.deleteAll();
        cg("cg.mu.deleteall", cgMu);
        cgMu.append("New", null);
        cg("cg.mu.appendempty", cgMu);

        count("cg.po.init", cgPo);
        cgPo.setSelectedIndex(1, true);
        cg("cg.po.sel1", cgPo);
        cgPo.setSelectedIndex(2, false);
        cg("cg.po.unsel2", cgPo);
        cgPo.insert(0, "XS", null);
        cg("cg.po.insert0", cgPo);
        cgPo.delete(0);
        cg("cg.po.delete0", cgPo);
        cgPo.delete(1);
        count("cg.po.delsel", cgPo);
        cgPo.deleteAll();
        cgPo.append("Only", null);
        cg("cg.po.only", cgPo);
    }

    private void checkGauges() {
        g("g.int.init", gInt);
        value("g.int.set12", gInt, 12);
        value("g.int.setneg", gInt, -3);
        value("g.int.set7", gInt, 7);
        maxValue("g.int.max4", gInt, 4);
        maxValue("g.int.max20", gInt, 20);
        maxValue("g.int.max0", gInt, 0);
        maxValue("g.int.maxindef", gInt, Gauge.INDEFINITE);
        g("g.int.after", gInt);
        gauge("g.new.int0", true, 0, 0);
        gauge("g.new.clamphi", true, 10, 15);
        gauge("g.new.clamplo", false, 10, -2);
        gauge("g.new.indefbad", false, Gauge.INDEFINITE, 7);
        gauge("g.new.indefint", true, Gauge.INDEFINITE, 0);

        g("g.prog.init", gProg);
        value("g.prog.set25", gProg, 25);
        value("g.prog.set13", gProg, 13);
        maxValue("g.prog.maxindef", gProg, Gauge.INDEFINITE);
        value("g.prog.running", gProg, Gauge.CONTINUOUS_RUNNING);
        value("g.prog.set5", gProg, 5);
        value("g.prog.updating", gProg, Gauge.INCREMENTAL_UPDATING);
        maxValue("g.prog.maxindef2", gProg, Gauge.INDEFINITE);
        maxValue("g.prog.max8", gProg, 8);

        g("g.indef.init", gIndef);
        value("g.indef.incidle", gIndef, Gauge.INCREMENTAL_IDLE);
        value("g.indef.contidle", gIndef, Gauge.CONTINUOUS_IDLE);
        value("g.indef.setneg", gIndef, -1);
        value("g.indef.set4", gIndef, 4);
        value("g.indef.running", gIndef, Gauge.CONTINUOUS_RUNNING);
    }

    private void checkDateFields() {
        p("df.date.init", date(dDate.getDate()));
        p("df.modes", dDate.getInputMode() + " " + dTime.getInputMode() + " "
            + dDateTime.getInputMode());
        dDate.setDate(new Date(T));
        p("df.date.set", date(dDate.getDate()));
        dTime.setDate(new Date(49062123L)); // 1970-01-01 13:37:42.123 GMT
        p("df.time.set", date(dTime.getDate()));
        dTime.setDate(new Date(T));
        p("df.time.setbeyondday", date(dTime.getDate()));
        dDateTime.setDate(new Date(T));
        p("df.datetime.set", date(dDateTime.getDate()));
        dDateTime.setDate(null);
        p("df.datetime.setnull", date(dDateTime.getDate()));
        dDateTime.setDate(new Date(0L));
        p("df.datetime.epoch", date(dDateTime.getDate()));
        try {
            dDate.setInputMode(4);
            p("df.badmode", "ok");
        } catch (RuntimeException e) {
            p("df.badmode", exc(e));
        }
        try {
            new DateField("x", 0);
            p("df.new.badmode", "ok");
        } catch (RuntimeException e) {
            p("df.new.badmode", exc(e));
        }
    }

    /** Spacer, CustomItem and Ticker must be safe to construct. */
    private void checkP2() {
        p("spacer.append", spacerIndex);
        p("custom.append", customIndex);
        Command c = new Command("C", Command.ITEM, 1);
        try {
            spacer.setLabel("x");
            p("spacer.label", "ok");
        } catch (RuntimeException e) {
            p("spacer.label", exc(e));
        }
        try {
            spacer.addCommand(c);
            p("spacer.command", "ok");
        } catch (RuntimeException e) {
            p("spacer.command", exc(e));
        }
        try {
            new Spacer(-1, 0);
            p("spacer.new.neg", "ok");
        } catch (RuntimeException e) {
            p("spacer.new.neg", exc(e));
        }
        p("ticker.form", (form.getTicker() == ticker) + " " + q(ticker.getString()));
        ticker.setString("Tock");
        p("ticker.set", q(ticker.getString()));
        box.setTicker(ticker);
        p("ticker.box", box.getTicker() == ticker);
        try {
            new Ticker(null);
            p("ticker.new.null", "ok");
        } catch (RuntimeException e) {
            p("ticker.new.null", exc(e));
        }
    }

    private void checkForm() {
        p("form.size", form.size());
        p("form.get", (form.get(0) == si) + " " + (form.get(customIndex) == custom));
        StringItem ins = new StringItem("Ins", "inserted");
        form.insert(0, ins);
        p("form.insert0", form.size() + " " + (form.get(0) == ins) + " " + (form.get(1) == si));
        form.delete(0);
        p("form.delete0", form.size() + " " + (form.get(0) == si));
        int last = form.append(ins);
        p("form.reappend", last);
        form.delete(last);
        StringItem rep = new StringItem("Rep", "replacement");
        form.set(1, rep);
        p("form.set1", form.size() + " " + (form.get(1) == rep));
        Form other = new Form("Other");
        p("form.set.oldfree", other.append(link));
        other.deleteAll();
        form.set(1, link);
        p("form.set.back", form.get(1) == link);
        try {
            other.append(si);
            p("form.append.owned", "ok");
        } catch (RuntimeException e) {
            p("form.append.owned", exc(e));
        }
        try {
            form.insert(form.size() + 1, new StringItem(null, "x"));
            p("form.insert.bad", "ok");
        } catch (RuntimeException e) {
            p("form.insert.bad", exc(e));
        }
        try {
            form.get(form.size());
            p("form.get.bad", "ok");
        } catch (RuntimeException e) {
            p("form.get.bad", exc(e));
        }
        try {
            form.delete(-1);
            p("form.delete.bad", "ok");
        } catch (RuntimeException e) {
            p("form.delete.bad", exc(e));
        }
        int is = form.append("plain string");
        Item sItem = form.get(is);
        p("form.appendstr", is + " " + (sItem instanceof StringItem) + " "
            + q(((StringItem) sItem).getText()));
        int im = form.append(immutable);
        p("form.appendimg", im + " " + (form.get(im) instanceof ImageItem));
        form.delete(im);
        form.delete(is);
        try {
            form.append((String) null);
            p("form.appendnull", "ok");
        } catch (RuntimeException e) {
            p("form.appendnull", exc(e));
        }
        Form two = new Form("Two", new Item[] {new StringItem(null, "a"), new Spacer(1, 1)});
        p("form.ctor", two.size());
        form.deleteAll();
        p("form.deleteall", form.size());
        for (int i = 0; i < items.length; i++) {
            form.append(items[i]);
        }
        form.append(spacer);
        form.append(custom);
        p("form.rebuild", form.size() + " " + (form.get(0) == si) + " "
            + (form.get(customIndex) == custom));
        form.setTitle("Check 2");
        p("form.title", q(form.getTitle()));
        try {
            display.setCurrentItem(new StringItem(null, "free"));
            p("display.item.unowned", "ok");
        } catch (RuntimeException e) {
            p("display.item.unowned", exc(e));
        }
        display.setCurrentItem(tfNum);
        await(form);
        p("display.item", current());
    }

    private void checkTextBox() {
        p("tb.init", q(box.getString()));
        p("tb.size", box.size());
        p("tb.max", box.getMaxSize());
        p("tb.title", q(box.getTitle()));
        p("tb.constraints", box.getConstraints());
        p("tb.setmax5", box.setMaxSize(5) + " " + q(box.getString()));
        box.setString("abc");
        box.setConstraints(TextField.NUMERIC);
        p("tb.cons.num", q(box.getString()) + " " + box.getConstraints());
        box.setString("2024");
        box.setConstraints(TextField.ANY);
        p("tb.cons.any", q(box.getString()) + " " + box.getConstraints());
        box.insert("x", 99);
        p("tb.insert", q(box.getString()));
        try {
            box.insert("y", 0);
            p("tb.insertfull", "ok");
        } catch (RuntimeException e) {
            p("tb.insertfull", exc(e));
        }
        box.delete(0, 2);
        p("tb.delete", q(box.getString()));
        box.setChars("hello".toCharArray(), 1, 3);
        p("tb.chars", q(box.getString()));
        p("tb.caret", box.getCaretPosition());
        box.setString(null);
        p("tb.setnull", q(box.getString()) + " " + box.size());
        try {
            new TextBox("x", "abc", 10, TextField.NUMERIC);
            p("tb.new.badtext", "ok");
        } catch (RuntimeException e) {
            p("tb.new.badtext", exc(e));
        }
        try {
            new TextBox("x", null, 0, TextField.ANY);
            p("tb.new.max0", "ok");
        } catch (RuntimeException e) {
            p("tb.new.max0", exc(e));
        }
        box.setString("End");
    }

    // ---- TextField helpers: print the contents after the call, or the exception

    private static void set(String n, TextField tf, String s) {
        try {
            tf.setString(s);
            p(n, q(tf.getString()));
        } catch (RuntimeException e) {
            p(n, exc(e));
        }
    }

    private static void ins(String n, TextField tf, String s, int pos) {
        try {
            tf.insert(s, pos);
            p(n, q(tf.getString()));
        } catch (RuntimeException e) {
            p(n, exc(e));
        }
    }

    private static void del(String n, TextField tf, int off, int len) {
        try {
            tf.delete(off, len);
            p(n, q(tf.getString()));
        } catch (RuntimeException e) {
            p(n, exc(e));
        }
    }

    private static void chars(String n, TextField tf, char[] data, int off, int len) {
        try {
            tf.setChars(data, off, len);
            p(n, q(tf.getString()));
        } catch (RuntimeException e) {
            p(n, exc(e));
        }
    }

    private static void max(String n, TextField tf, int maxSize) {
        try {
            int got = tf.setMaxSize(maxSize);
            p(n, got + " " + q(tf.getString()));
        } catch (RuntimeException e) {
            p(n, exc(e));
        }
    }

    private static void constraints(String n, TextField tf, int c) {
        try {
            tf.setConstraints(c);
            p(n, q(tf.getString()) + " " + tf.getConstraints());
        } catch (RuntimeException e) {
            p(n, exc(e) + " " + tf.getConstraints());
        }
    }

    private static void create(String n, String text, int maxSize, int c) {
        try {
            p(n, q(new TextField("x", text, maxSize, c).getString()));
        } catch (RuntimeException e) {
            p(n, exc(e));
        }
    }

    // ---- ChoiceGroup helpers

    /** Full selection state; used where MIDP fixes which element is selected. */
    private static void cg(String n, ChoiceGroup c) {
        boolean[] f = new boolean[c.size()];
        int k = c.getSelectedFlags(f);
        p(n, "size=" + c.size() + " sel=" + c.getSelectedIndex() + " flags=" + bits(f)
            + " count=" + k);
    }

    /** Only the count; MIDP lets the implementation pick the selected element. */
    private static void count(String n, ChoiceGroup c) {
        p(n, "size=" + c.size() + " count=" + c.getSelectedFlags(new boolean[c.size()]));
    }

    private static String bits(boolean[] f) {
        StringBuffer b = new StringBuffer(f.length);
        for (int i = 0; i < f.length; i++) {
            b.append(f[i] ? '1' : '0');
        }
        return b.toString();
    }

    // ---- Gauge helpers

    private static void g(String n, Gauge gauge) {
        p(n, "v=" + gauge.getValue() + " max=" + gauge.getMaxValue() + " interactive="
            + gauge.isInteractive());
    }

    private static void value(String n, Gauge gauge, int v) {
        try {
            gauge.setValue(v);
            p(n, "v=" + gauge.getValue() + " max=" + gauge.getMaxValue());
        } catch (RuntimeException e) {
            p(n, exc(e));
        }
    }

    private static void maxValue(String n, Gauge gauge, int m) {
        try {
            gauge.setMaxValue(m);
            p(n, "v=" + gauge.getValue() + " max=" + gauge.getMaxValue());
        } catch (RuntimeException e) {
            p(n, exc(e));
        }
    }

    private static void gauge(String n, boolean interactive, int m, int v) {
        try {
            g(n, new Gauge("x", interactive, m, v));
        } catch (RuntimeException e) {
            p(n, exc(e));
        }
    }

    // ---- display helpers

    /** Waits (up to 5 s) until d is current and shown; setCurrent is asynchronous. */
    private void await(Displayable d) {
        for (int i = 0; i < 100 && !(display.getCurrent() == d && d.isShown()); i++) {
            try {
                Thread.sleep(50);
            } catch (InterruptedException e) {
                return;
            }
        }
    }

    private void switchTo(Displayable d, String n) {
        display.setCurrent(d);
        await(d);
        p(n, current());
    }

    private String current() {
        Displayable c = display.getCurrent();
        String name = c == form ? "form" : c == box ? "textbox" : c == null ? "null" : "other";
        return name + (c != null && c.isShown() ? " shown" : " hidden");
    }

    // ---- output

    private static void p(String check, String value) {
        System.out.println("PM-LFFORM " + check + " " + value);
    }

    private static void p(String check, int value) {
        p(check, String.valueOf(value));
    }

    private static void p(String check, boolean value) {
        p(check, value ? "true" : "false");
    }

    private static String q(String s) {
        return s == null ? "null" : "[" + Shared.ascii(s) + "]";
    }

    private static String date(Date d) {
        return d == null ? "null" : String.valueOf(d.getTime());
    }

    private static String size(Image img) {
        return img.getWidth() + "x" + img.getHeight();
    }

    private static String exc(Throwable e) {
        if (e instanceof IllegalArgumentException) {
            return "IAE";
        } else if (e instanceof IllegalStateException) {
            return "ISE";
        } else if (e instanceof StringIndexOutOfBoundsException) {
            return "SIOOBE";
        } else if (e instanceof ArrayIndexOutOfBoundsException) {
            return "AIOOBE";
        } else if (e instanceof IndexOutOfBoundsException) {
            return "IOOBE";
        } else if (e instanceof NullPointerException) {
            return "NPE";
        }
        return e.getClass().getName();
    }
}
