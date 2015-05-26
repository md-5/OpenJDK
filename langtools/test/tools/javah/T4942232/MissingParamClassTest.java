/*
 * Copyright (c) 2013, 2015, Oracle and/or its affiliates. All rights reserved.
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
 * @bug 4942232
 * @summary Verifies that javah won't attempt to generate a header file if a
 * native method in a supplied class contains a parameter type whose corresponding
 * class is missing or not in the classpath
 * @library /tools/lib
 * @modules jdk.compiler/com.sun.tools.javac.api
 *          jdk.compiler/com.sun.tools.javac.file
 *          jdk.compiler/com.sun.tools.javac.main
 * @build ToolBox
 * @run compile MissingParamClassTest.java
 * @clean MissingParamClassException
 * @run main MissingParamClassTest
 * @run compile MissingParamClassTest.java
 * @clean Param
 * @run main MissingParamClassTest
 */

import java.nio.file.Files;
import java.nio.file.Paths;

// Original test: test/tools/javah/MissingParamClassTest.sh
public class MissingParamClassTest {

    public static void main(String[] args) throws Exception {
        ToolBox tb = new ToolBox();

        String out = tb.new JavahTask()
                .classpath(ToolBox.testClasses)
                .classes("ParamClassTest")
                .run(ToolBox.Expect.FAIL)
                .getOutput(ToolBox.OutputKind.DIRECT);

        if (Files.exists(Paths.get("ParamClassTest.h")) || out.isEmpty())
            throw new AssertionError("The only output generated by javah must be an error message");
    }

}

class MissingParamClassException extends Exception {
    public MissingParamClassException() {
        System.out.println("MissingParamClassException constructor called");
    }
}

class ParamClassTest {
    public native void method(Param s);

    public static void main(String args[]) {
    }
}

class Param extends MissingParamClassException {
    Param() {
        System.out.println("Param constructor");
    }
}
