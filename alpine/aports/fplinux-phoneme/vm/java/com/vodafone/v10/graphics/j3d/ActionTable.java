// SPDX-License-Identifier: GPL-2.0-only
package com.vodafone.v10.graphics.j3d;

import java.io.IOException;

public class ActionTable extends com.mascotcapsule.micro3d.v3.ActionTable {
    public ActionTable(byte[] data) {
        super(data);
    }

    public ActionTable(String name) throws IOException {
        super(Resources.absolute(name));
    }
}
