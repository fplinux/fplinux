// SPDX-License-Identifier: GPL-2.0-only
package fplinux.fixtures.lfform;

import javax.microedition.lcdui.Graphics;
import javax.microedition.lcdui.Image;

/** Helpers shared by the demo and the self-check MIDlets. */
final class Shared {
    private Shared() {
    }

    /** ASCII form of s for System.out: newline as \n, any other char outside 32..126 as \\uXXXX. */
    static String ascii(String s) {
        StringBuffer b = new StringBuffer(s.length());
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if (c == '\n') {
                b.append("\\n");
            } else if (c >= ' ' && c < 127) {
                b.append(c);
            } else {
                String hex = Integer.toHexString(c);
                b.append("\\u");
                for (int k = hex.length(); k < 4; k++) {
                    b.append('0');
                }
                b.append(hex);
            }
        }
        return b.toString();
    }

    /** A mutable image drawn with Graphics: a red frame with a blue diagonal. */
    static Image drawnMutable(int w, int h) {
        Image img = Image.createImage(w, h);
        Graphics g = img.getGraphics();
        g.setColor(0xFFFFFF);
        g.fillRect(0, 0, w, h);
        g.setColor(0xCC0000);
        g.drawRect(0, 0, w - 1, h - 1);
        g.setColor(0x0000CC);
        g.drawLine(0, 0, w - 1, h - 1);
        return img;
    }

    /** An immutable image from ARGB pixels: horizontal green gradient. */
    static Image rgbImage(int w, int h) {
        int[] argb = new int[w * h];
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                argb[y * w + x] = 0xFF000000 | ((x * 255 / w) << 8);
            }
        }
        return Image.createRGBImage(argb, w, h, false);
    }
}
