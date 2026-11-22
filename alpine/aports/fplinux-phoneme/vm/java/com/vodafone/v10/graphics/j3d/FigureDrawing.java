// SPDX-License-Identifier: GPL-2.0-only
package com.vodafone.v10.graphics.j3d;

import javax.microedition.lcdui.Graphics;

/** Draws figures for Graphics.drawFigure() on a shared micro3d engine. */
public final class FigureDrawing {
    private static com.mascotcapsule.micro3d.v3.Graphics3D engine;

    private FigureDrawing() {
    }

    public static synchronized void draw(Graphics graphics, Figure figure, int x, int y,
                                         FigureLayout layout, Effect3D effect) {
        if (engine == null) {
            engine = new com.mascotcapsule.micro3d.v3.Graphics3D();
        }
        engine.bind(graphics);
        try {
            engine.drawFigure(figure, x, y, layout.micro3d, effect.micro3d);
        } finally {
            engine.release(graphics);
        }
    }
}
