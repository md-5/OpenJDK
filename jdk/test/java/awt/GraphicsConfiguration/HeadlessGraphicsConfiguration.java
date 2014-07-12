/*
 * Copyright (c) 2007, 2014, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

import java.awt.*;
import java.awt.geom.AffineTransform;
import java.awt.image.BufferedImage;
import java.awt.image.ColorModel;

/*
 * @test
 * @summary Check that GraphicsConfiguration methods do not throw unexpected
 *          exceptions in headless mode
 * @run main/othervm -Djava.awt.headless=true HeadlessGraphicsConfiguration
 */

public class HeadlessGraphicsConfiguration {
    public static void main(String args[]) {
        GraphicsEnvironment ge = GraphicsEnvironment.getLocalGraphicsEnvironment();
        Graphics2D gd = ge.createGraphics(new BufferedImage(100, 100, BufferedImage.TYPE_4BYTE_ABGR));
        GraphicsConfiguration gc = gd.getDeviceConfiguration();
        GraphicsDevice gdev = gc.getDevice();
        BufferedImage bi = gc.createCompatibleImage(100, 100);
        bi = gc.createCompatibleImage(100, 100, Transparency.TRANSLUCENT);

        ColorModel cm = gc.getColorModel();
        cm = gc.getColorModel(Transparency.TRANSLUCENT);

        AffineTransform at = gc.getDefaultTransform();
        at = gc.getNormalizingTransform();

        Rectangle r = gc.getBounds();
    }
}
