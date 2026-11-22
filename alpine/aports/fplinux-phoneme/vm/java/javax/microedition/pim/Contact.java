// SPDX-License-Identifier: GPL-2.0-only
package javax.microedition.pim;

public interface Contact extends PIMItem {
    int ADDR = 100;
    int BIRTHDAY = 101;
    int CLASS = 102;
    int EMAIL = 103;
    int FORMATTED_ADDR = 104;
    int FORMATTED_NAME = 105;
    int NAME = 106;
    int NICKNAME = 107;
    int NOTE = 108;
    int ORG = 109;
    int PHOTO = 110;
    int PUBLIC_KEY = 111;
    int REVISION = 112;
    int TEL = 113;
    int TITLE = 114;
    int PHOTO_URL = 115;
    int PUBLIC_KEY_STRING = 116;
    int UID = 117;
    int URL = 118;

    int ATTR_ASST = 1;
    int ATTR_AUTO = 2;
    int ATTR_FAX = 4;
    int ATTR_HOME = 8;
    int ATTR_MOBILE = 16;
    int ATTR_OTHER = 32;
    int ATTR_PAGER = 64;
    int ATTR_PREFERRED = 128;
    int ATTR_SMS = 256;
    int ATTR_WORK = 512;

    int ADDR_COUNTRY = 0;
    int ADDR_EXTRA = 1;
    int ADDR_LOCALITY = 2;
    int ADDR_POBOX = 3;
    int ADDR_POSTALCODE = 4;
    int ADDR_REGION = 5;
    int ADDR_STREET = 6;

    int NAME_FAMILY = 0;
    int NAME_GIVEN = 1;
    int NAME_OTHER = 2;
    int NAME_PREFIX = 3;
    int NAME_SUFFIX = 4;

    int CLASS_CONFIDENTIAL = 200;
    int CLASS_PRIVATE = 201;
    int CLASS_PUBLIC = 202;

    int getPreferredIndex(int field);
}
