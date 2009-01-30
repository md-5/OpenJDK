/*
 * Copyright 1997-2008 Sun Microsystems, Inc.  All Rights Reserved.
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
 * Please contact Sun Microsystems, Inc., 4150 Network Circle, Santa Clara,
 * CA 95054 USA or visit www.sun.com if you need additional information or
 * have any questions.
 *
 */

# include "incls/_precompiled.incl"
# include "incls/_objArrayOop.cpp.incl"

#define ObjArrayOop_OOP_ITERATE_DEFN(OopClosureType, nv_suffix)                    \
                                                                                   \
int objArrayOopDesc::oop_iterate_range(OopClosureType* blk, int start, int end) {  \
  SpecializationStats::record_call();                                              \
  return ((objArrayKlass*)blueprint())->oop_oop_iterate_range##nv_suffix(this, blk, start, end); \
}

ALL_OOP_OOP_ITERATE_CLOSURES_1(ObjArrayOop_OOP_ITERATE_DEFN)
ALL_OOP_OOP_ITERATE_CLOSURES_2(ObjArrayOop_OOP_ITERATE_DEFN)
