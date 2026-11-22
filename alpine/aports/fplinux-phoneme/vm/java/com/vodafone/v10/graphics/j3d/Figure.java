// SPDX-License-Identifier: GPL-2.0-only
package com.vodafone.v10.graphics.j3d;

import java.io.IOException;

public class Figure extends com.mascotcapsule.micro3d.v3.Figure {
    public Figure(byte[] data) {
        super(data);
    }

    public Figure(String name) throws IOException {
        super(Resources.absolute(name));
    }

    public void setPosture(ActionTable table, int action, int frame) {
        super.setPosture((com.mascotcapsule.micro3d.v3.ActionTable)table, action, frame);
    }

    public void setTexture(Texture texture) {
        super.setTexture((com.mascotcapsule.micro3d.v3.Texture)texture);
    }
}
