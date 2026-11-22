// SPDX-License-Identifier: GPL-2.0-only
package com.vodafone.v10.graphics.j3d;

import java.io.IOException;

/** The flag selects a model texture rather than a sphere map. */
public class Texture extends com.mascotcapsule.micro3d.v3.Texture {
    public Texture(byte[] data, boolean forModel) {
        super(data, forModel);
    }

    public Texture(String name, boolean forModel) throws IOException {
        super(Resources.absolute(name), forModel);
    }
}
