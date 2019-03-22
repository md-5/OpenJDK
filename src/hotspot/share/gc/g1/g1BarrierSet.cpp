/*
 * Copyright (c) 2001, 2019, Oracle and/or its affiliates. All rights reserved.
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

#include "precompiled.hpp"
#include "gc/g1/g1BarrierSet.inline.hpp"
#include "gc/g1/g1BarrierSetAssembler.hpp"
#include "gc/g1/g1CardTable.inline.hpp"
#include "gc/g1/g1CollectedHeap.inline.hpp"
#include "gc/g1/g1SATBMarkQueueSet.hpp"
#include "gc/g1/g1ThreadLocalData.hpp"
#include "gc/g1/heapRegion.hpp"
#include "gc/shared/satbMarkQueue.hpp"
#include "logging/log.hpp"
#include "oops/access.inline.hpp"
#include "oops/compressedOops.inline.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/interfaceSupport.inline.hpp"
#include "runtime/mutexLocker.hpp"
#include "runtime/thread.inline.hpp"
#include "utilities/macros.hpp"
#ifdef COMPILER1
#include "gc/g1/c1/g1BarrierSetC1.hpp"
#endif
#ifdef COMPILER2
#include "gc/g1/c2/g1BarrierSetC2.hpp"
#endif

class G1BarrierSetC1;
class G1BarrierSetC2;

G1BarrierSet::G1BarrierSet(G1CardTable* card_table) :
  CardTableBarrierSet(make_barrier_set_assembler<G1BarrierSetAssembler>(),
                      make_barrier_set_c1<G1BarrierSetC1>(),
                      make_barrier_set_c2<G1BarrierSetC2>(),
                      card_table,
                      BarrierSet::FakeRtti(BarrierSet::G1BarrierSet)),
  _satb_mark_queue_buffer_allocator("SATB Buffer Allocator", G1SATBBufferSize),
  _dirty_card_queue_buffer_allocator("DC Buffer Allocator", G1UpdateBufferSize),
  _satb_mark_queue_set(),
  _dirty_card_queue_set(),
  _shared_dirty_card_queue(&_dirty_card_queue_set)
{}

void G1BarrierSet::enqueue(oop pre_val) {
  // Nulls should have been already filtered.
  assert(oopDesc::is_oop(pre_val, true), "Error");
  G1ThreadLocalData::satb_mark_queue(Thread::current()).enqueue(pre_val);
}

template <class T> void
G1BarrierSet::write_ref_array_pre_work(T* dst, size_t count) {
  if (!_satb_mark_queue_set.is_active()) return;
  T* elem_ptr = dst;
  for (size_t i = 0; i < count; i++, elem_ptr++) {
    T heap_oop = RawAccess<>::oop_load(elem_ptr);
    if (!CompressedOops::is_null(heap_oop)) {
      enqueue(CompressedOops::decode_not_null(heap_oop));
    }
  }
}

void G1BarrierSet::write_ref_array_pre(oop* dst, size_t count, bool dest_uninitialized) {
  if (!dest_uninitialized) {
    write_ref_array_pre_work(dst, count);
  }
}

void G1BarrierSet::write_ref_array_pre(narrowOop* dst, size_t count, bool dest_uninitialized) {
  if (!dest_uninitialized) {
    write_ref_array_pre_work(dst, count);
  }
}

void G1BarrierSet::write_ref_field_post_slow(volatile CardValue* byte) {
  // In the slow path, we know a card is not young
  assert(*byte != G1CardTable::g1_young_card_val(), "slow path invoked without filtering");
  OrderAccess::storeload();
  if (*byte != G1CardTable::dirty_card_val()) {
    *byte = G1CardTable::dirty_card_val();
    Thread* thr = Thread::current();
    G1ThreadLocalData::dirty_card_queue(thr).enqueue(byte);
  }
}

void G1BarrierSet::invalidate(MemRegion mr) {
  if (mr.is_empty()) {
    return;
  }
  volatile CardValue* byte = _card_table->byte_for(mr.start());
  CardValue* last_byte = _card_table->byte_for(mr.last());
  // skip initial young cards
  for (; byte <= last_byte && *byte == G1CardTable::g1_young_card_val(); byte++);

  if (byte <= last_byte) {
    OrderAccess::storeload();
    // Enqueue if necessary.
    Thread* thr = Thread::current();
    G1DirtyCardQueue& queue = G1ThreadLocalData::dirty_card_queue(thr);
    for (; byte <= last_byte; byte++) {
      CardValue bv = *byte;
      if ((bv != G1CardTable::g1_young_card_val()) &&
          (bv != G1CardTable::dirty_card_val())) {
        *byte = G1CardTable::dirty_card_val();
        queue.enqueue(byte);
      }
    }
  }
}

void G1BarrierSet::on_thread_create(Thread* thread) {
  // Create thread local data
  G1ThreadLocalData::create(thread);
}

void G1BarrierSet::on_thread_destroy(Thread* thread) {
  // Destroy thread local data
  G1ThreadLocalData::destroy(thread);
}

void G1BarrierSet::on_thread_attach(Thread* thread) {
  assert(!G1ThreadLocalData::satb_mark_queue(thread).is_active(), "SATB queue should not be active");
  assert(G1ThreadLocalData::satb_mark_queue(thread).is_empty(), "SATB queue should be empty");
  assert(G1ThreadLocalData::dirty_card_queue(thread).is_active(), "Dirty card queue should be active");
  // Can't assert that the DCQ is empty.  There is early execution on
  // the main thread, before it gets added to the threads list, which
  // is where this is called.  That execution may enqueue dirty cards.

  // If we are creating the thread during a marking cycle, we should
  // set the active field of the SATB queue to true.  That involves
  // copying the global is_active value to this thread's queue, which
  // is done without any direct synchronization here.
  //
  // The activation and deactivation of the SATB queues occurs at the
  // beginning / end of a marking cycle, and is done during
  // safepoints.  This function is called just before a thread is
  // added to its corresponding threads list (for Java or non-Java
  // threads, respectively).
  //
  // For Java threads, that's done while holding the Threads_lock,
  // which ensures we're not at a safepoint, so reading the global
  // is_active state is synchronized against update.
  assert(!thread->is_Java_thread() || !SafepointSynchronize::is_at_safepoint(),
         "Should not be at a safepoint");
  // For non-Java threads, thread creation (and list addition) may,
  // and indeed usually does, occur during a safepoint.  But such
  // creation isn't concurrent with updating the global SATB active
  // state.
  bool is_satb_active = _satb_mark_queue_set.is_active();
  G1ThreadLocalData::satb_mark_queue(thread).set_active(is_satb_active);
}

void G1BarrierSet::on_thread_detach(Thread* thread) {
  // Flush any deferred card marks.
  CardTableBarrierSet::on_thread_detach(thread);
  G1ThreadLocalData::satb_mark_queue(thread).flush();
  G1ThreadLocalData::dirty_card_queue(thread).flush();
}

BufferNode::Allocator& G1BarrierSet::satb_mark_queue_buffer_allocator() {
  return _satb_mark_queue_buffer_allocator;
}

BufferNode::Allocator& G1BarrierSet::dirty_card_queue_buffer_allocator() {
  return _dirty_card_queue_buffer_allocator;
}
