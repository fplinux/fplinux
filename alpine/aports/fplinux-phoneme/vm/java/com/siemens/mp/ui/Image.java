// SPDX-License-Identifier: GPL-2.0-only
package com.siemens.mp.ui;

import java.io.IOException;

public class Image {
    protected Image() {
    }

    public static javax.microedition.lcdui.Image createImageWithoutScaling(String name)
            throws IOException {
        return javax.microedition.lcdui.Image.createImage(name);
    }
}
