// SPDX-License-Identifier: GPL-2.0-only
package javax.microedition.pim;

import java.io.InputStream;
import java.io.OutputStream;
import java.io.UnsupportedEncodingException;

/** Entry point of the PIM package; this runtime has no PIM lists. */
public abstract class PIM {
    public static final int CONTACT_LIST = 1;
    public static final int EVENT_LIST = 2;
    public static final int TODO_LIST = 3;
    public static final int READ_ONLY = 1;
    public static final int WRITE_ONLY = 2;
    public static final int READ_WRITE = 3;

    private static final PIM INSTANCE = new UnavailablePIM();

    protected PIM() {
    }

    public static PIM getInstance() {
        return INSTANCE;
    }

    public abstract PIMList openPIMList(int pimListType, int mode)
        throws PIMException;

    public abstract PIMList openPIMList(int pimListType, int mode, String name)
        throws PIMException;

    public abstract String[] listPIMLists(int pimListType);

    public abstract PIMItem[] fromSerialFormat(InputStream is, String enc)
        throws PIMException, UnsupportedEncodingException;

    public abstract void toSerialFormat(PIMItem item, OutputStream os,
                                        String enc, String dataFormat)
        throws PIMException, UnsupportedEncodingException;

    public abstract String[] supportedSerialFormats(int pimListType);

    private static void checkListType(int pimListType) {
        if (pimListType < CONTACT_LIST || pimListType > TODO_LIST) {
            throw new IllegalArgumentException("Invalid PIM list type");
        }
    }

    private static void checkMode(int mode) {
        if (mode < READ_ONLY || mode > READ_WRITE) {
            throw new IllegalArgumentException("Invalid PIM list mode");
        }
    }

    private static final class UnavailablePIM extends PIM {
        public PIMList openPIMList(int pimListType, int mode)
                throws PIMException {
            checkListType(pimListType);
            checkMode(mode);
            throw new PIMException("No PIM list of this type",
                                   PIMException.LIST_NOT_ACCESSIBLE);
        }

        public PIMList openPIMList(int pimListType, int mode, String name)
                throws PIMException {
            if (name == null) {
                throw new NullPointerException("null list name");
            }
            return openPIMList(pimListType, mode);
        }

        public String[] listPIMLists(int pimListType) {
            checkListType(pimListType);
            return new String[0];
        }

        public PIMItem[] fromSerialFormat(InputStream is, String enc)
                throws PIMException, UnsupportedEncodingException {
            if (is == null) {
                throw new NullPointerException("null input stream");
            }
            throw new PIMException("No serial format is supported",
                                   PIMException.FEATURE_NOT_SUPPORTED);
        }

        public void toSerialFormat(PIMItem item, OutputStream os, String enc,
                                   String dataFormat)
                throws PIMException, UnsupportedEncodingException {
            if (item == null || os == null) {
                throw new NullPointerException("null item or output stream");
            }
            if (dataFormat == null) {
                throw new IllegalArgumentException("null serial format");
            }
            throw new PIMException("No serial format is supported",
                                   PIMException.FEATURE_NOT_SUPPORTED);
        }

        public String[] supportedSerialFormats(int pimListType) {
            checkListType(pimListType);
            return new String[0];
        }
    }
}
