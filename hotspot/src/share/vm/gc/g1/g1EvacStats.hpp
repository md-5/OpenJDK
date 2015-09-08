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
 *
 */

#ifndef SHARE_VM_gc_G1_G1EVACSTATS_HPP
#define SHARE_VM_gc_G1_G1EVACSTATS_HPP

#include "gc/shared/plab.hpp"
#include "runtime/atomic.hpp"

// Records various memory allocation statistics gathered during evacuation.
class G1EvacStats : public PLABStats {
 private:
  size_t _region_end_waste; // Number of words wasted due to skipping to the next region.
  uint   _regions_filled;   // Number of regions filled completely.
  size_t _direct_allocated; // Number of words allocated directly into the regions.

  // Number of words in live objects remaining in regions that ultimately suffered an
  // evacuation failure. This is used in the regions when the regions are made old regions.
  size_t _failure_used;
  // Number of words wasted in regions which failed evacuation. This is the sum of space
  // for objects successfully copied out of the regions (now dead space) plus waste at the
  // end of regions.
  size_t _failure_waste;

  virtual void reset() {
    PLABStats::reset();
    _region_end_waste = 0;
    _regions_filled = 0;
    _direct_allocated = 0;
    _failure_used = 0;
    _failure_waste = 0;
  }

 public:
  G1EvacStats(size_t desired_plab_sz_, unsigned wt) : PLABStats(desired_plab_sz_, wt),
    _region_end_waste(0), _regions_filled(0), _direct_allocated(0),
    _failure_used(0), _failure_waste(0) {
  }

  virtual void adjust_desired_plab_sz();

  size_t allocated() const { return _allocated; }
  size_t wasted() const { return _wasted; }
  size_t unused() const { return _unused; }
  size_t used() const { return allocated() - (wasted() + unused()); }
  size_t undo_wasted() const { return _undo_wasted; }

  uint regions_filled() const { return _regions_filled; }
  size_t region_end_waste() const { return _region_end_waste; }
  size_t direct_allocated() const { return _direct_allocated; }

  // Amount of space in heapwords used in the failing regions when an evacuation failure happens.
  size_t failure_used() const { return _failure_used; }
  // Amount of space in heapwords wasted (unused) in the failing regions when an evacuation failure happens.
  size_t failure_waste() const { return _failure_waste; }

  void add_direct_allocated(size_t value) {
    Atomic::add_ptr(value, &_direct_allocated);
  }

  void add_region_end_waste(size_t value) {
    Atomic::add_ptr(value, &_region_end_waste);
    Atomic::add_ptr(1, &_regions_filled);
  }

  void add_failure_used_and_waste(size_t used, size_t waste) {
    Atomic::add_ptr(used, &_failure_used);
    Atomic::add_ptr(waste, &_failure_waste);
  }
};

#endif // SHARE_VM_gc_G1_G1EVACSTATS_HPP
