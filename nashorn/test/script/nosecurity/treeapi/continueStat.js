/*
 * Copyright (c) 2015, Oracle and/or its affiliates. All rights reserved.
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

/**
 * Tests to check representation continue tree.
 *
 * @bug 8068306
 * @test
 * @option -scripting
 * @run
 */

load(__DIR__ + "utils.js")

var code = <<EOF

while (true) { continue; };
begin: { while (true) { continue begin; } };
start: { for(;;) { continue start; } };
do continue; while(false)
label:do continue label; while(true)

EOF

parse("continueStat.js", code, "-nse", new (Java.extend(visitor, {
    visitContinue : function (node, obj) {
        obj.push(convert(node))
    }
})))
