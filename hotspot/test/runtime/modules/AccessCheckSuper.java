/*
 * Copyright (c) 2016, Oracle and/or its affiliates. All rights reserved.
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

/*
 * @test
 * @library /testlibrary /test/lib /compiler/whitebox ..
 * @compile p2/c2.java
 * @compile p3/c3.java
 * @build sun.hotspot.WhiteBox
 * @compile/module=java.base java/lang/reflect/ModuleHelper.java
 * @run main ClassFileInstaller sun.hotspot.WhiteBox
 *                              sun.hotspot.WhiteBox$WhiteBoxPermission
 * @run main/othervm -Xbootclasspath/a:. -XX:+UnlockDiagnosticVMOptions -XX:+WhiteBoxAPI AccessCheckSuper
 */

import java.lang.reflect.Module;
import static jdk.test.lib.Asserts.*;

public class AccessCheckSuper {

    // Test that when a class cannot access its super class the message
    // contains  both "superclass" text and module text.
    public static void main(String args[]) throws Throwable {

        // Get the class loader for AccessCheckSuper and assume it's also used to
        // load class p2.c2 and class p3.c3.
        ClassLoader this_cldr = AccessCheckSuper.class.getClassLoader();

        // Define a module for p2.
        Object m2 = ModuleHelper.ModuleObject("module2", this_cldr, new String[] { "p2" });
        assertNotNull(m2, "Module should not be null");
        ModuleHelper.DefineModule(m2, "9.0", "m2/there", new String[] { "p2" });

        // Define a module for p3.
        Object m3 = ModuleHelper.ModuleObject("module3", this_cldr, new String[] { "p3" });
        assertNotNull(m3, "Module should not be null");
        ModuleHelper.DefineModule(m3, "9.0", "m3/there", new String[] { "p3" });

        // Since a readability edge has not been established between module2
        // and module3, p3.c3 cannot read its superclass p2.c2.
        try {
            Class p3_c3_class = Class.forName("p3.c3");
            throw new RuntimeException("Failed to get IAE (can't read superclass)");
        } catch (IllegalAccessError e) {
            if (!e.getMessage().contains("superclass access check failed") ||
                !e.getMessage().contains("does not read")) {
                throw new RuntimeException("Wrong message: " + e.getMessage());
            }
        }
    }
}
