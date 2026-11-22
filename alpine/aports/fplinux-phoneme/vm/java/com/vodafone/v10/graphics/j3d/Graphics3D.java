// SPDX-License-Identifier: GPL-2.0-only
package com.vodafone.v10.graphics.j3d;

/** Figure drawing on a MIDP graphics context. */
public interface Graphics3D {
    void drawFigure(Figure figure, int x, int y, FigureLayout layout, Effect3D effect);
}
