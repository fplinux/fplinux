// SPDX-License-Identifier: GPL-2.0-only
package javax.microedition.media.control;

import javax.microedition.media.MediaException;

/**
 * Display of video. No Player of this runtime plays video, so none returns
 * this Control; the interface lets MIDlets that ask for it link.
 */
public interface VideoControl extends GUIControl {
    int USE_DIRECT_VIDEO = 1;

    Object initDisplayMode(int mode, Object arg);

    void setDisplayLocation(int x, int y);

    int getDisplayX();

    int getDisplayY();

    void setVisible(boolean visible);

    void setDisplaySize(int width, int height) throws MediaException;

    void setDisplayFullScreen(boolean fullScreenMode) throws MediaException;

    int getSourceWidth();

    int getSourceHeight();

    int getDisplayWidth();

    int getDisplayHeight();

    byte[] getSnapshot(String imageType) throws MediaException;
}
