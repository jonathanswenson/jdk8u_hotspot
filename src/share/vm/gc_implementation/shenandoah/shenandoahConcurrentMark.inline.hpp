/*
 * Copyright (c) 2015, Red Hat, Inc. and/or its affiliates.
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

#ifndef SHARE_VM_GC_SHENANDOAH_SHENANDOAHCONCURRENTMARK_INLINE_HPP
#define SHARE_VM_GC_SHENANDOAH_SHENANDOAHCONCURRENTMARK_INLINE_HPP

#include "gc_implementation/shenandoah/brooksPointer.hpp"
#include "gc_implementation/shenandoah/shenandoahBarrierSet.inline.hpp"
#include "gc_implementation/shenandoah/shenandoahConcurrentMark.hpp"
#include "gc_implementation/shenandoah/shenandoahTaskqueue.inline.hpp"
#include "memory/iterator.inline.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/prefetch.inline.hpp"

template <class T, bool CL>
void ShenandoahMarkObjsClosure<T, CL>::do_task(SCMTask* task) {
  oop obj = task->obj();

  assert(obj != NULL, "expect non-null object");

  assert(oopDesc::unsafe_equals(obj, ShenandoahBarrierSet::resolve_oop_static_not_null(obj)), "expect forwarded obj in queue");

#ifdef ASSERT
  if (! oopDesc::bs()->is_safe(obj)) {
    tty->print_cr("trying to mark obj: "PTR_FORMAT" (%s) in dirty region: ", p2i((HeapWord*) obj), BOOL_TO_STR(_heap->is_marked_next(obj)));
    //      _heap->heap_region_containing(obj)->print();
    //      _heap->print_heap_regions();
  }
#endif
  assert(_heap->cancelled_concgc()
         || oopDesc::bs()->is_safe(obj),
         "we don't want to mark objects in from-space");
  assert(_heap->is_in(obj), "referenced objects must be in the heap. No?");
  assert(_heap->is_marked_next(obj), "only marked objects on task queue");

  if (task->is_not_chunked()) {
    if (CL) count_liveness(obj);
    if (!obj->is_objArray()) {
      // Case 1: Normal oop, process as usual.
      obj->oop_iterate(&_mark_refs);
    } else {
      // Case 2: Array instance and no chunk is set. Must be the first time
      // we visit it.
      do_chunked_array_start(obj);
    }
  } else {
    // Case 3: Array chunk, has sensible chunk id. Process it.
    do_chunked_array(obj, task->chunk(), task->pow());
  }
}

template <class T, bool CL>
inline void ShenandoahMarkObjsClosure<T, CL>::count_liveness(oop obj) {
  uint region_idx = _heap->heap_region_index_containing(obj);
  jushort cur = _live_data[region_idx];
  int size = obj->size() + BrooksPointer::word_size();
  int max = (1 << (sizeof(jushort) * 8)) - 1;
  if (size >= max) {
    // too big, add to region data directly
    _heap->regions()->get_fast(region_idx)->increase_live_data_words(size);
  } else {
    int new_val = cur + size;
    if (new_val >= max) {
      // overflow, flush to region data
      _heap->regions()->get_fast(region_idx)->increase_live_data_words(new_val);
      _live_data[region_idx] = 0;
    } else {
      // still good, remember in locals
      _live_data[region_idx] = (jushort) new_val;
    }
  }
}

template <class T, bool CL>
inline void ShenandoahMarkObjsClosure<T, CL>::do_chunked_array_start(oop obj) {
  assert(obj->is_objArray(), "expect object array");
  objArrayOop array = objArrayOop(obj);
  int len = array->length();

  if (len <= (int) ObjArrayMarkingStride*2) {
    // A few slices only, process directly
    array->oop_iterate_range(&_mark_refs, 0, len);
  } else {
    int bits = log2_long(len);
    // Compensate for non-power-of-two arrays, cover the array in excess:
    if (len != (1 << bits)) bits++;

    // Only allow full chunks on the queue. This frees do_chunked_array() from checking from/to
    // boundaries against array->length(), touching the array header on every chunk.
    //
    // To do this, we cut the prefix in full-sized chunks, and submit them on the queue.
    // If the array is not divided in chunk sizes, then there would be an irregular tail,
    // which we will process separately.

    int last_idx = 0;

    int chunk = 1;
    int pow = bits;

    // Handle overflow
    if (pow >= 31) {
      assert (pow == 31, "sanity");
      pow--;
      chunk = 2;
      last_idx = (1 << pow);
      bool pushed = _queue->push(SCMTask(array, 1, pow));
      assert(pushed, "overflow queue should always succeed pushing");
    }

    // Split out tasks, as suggested in ObjArrayChunkedTask docs. Record the last
    // successful right boundary to figure out the irregular tail.
    while ((1 << pow) > (int)ObjArrayMarkingStride &&
           (chunk*2 < SCMTask::chunk_size)) {
      pow--;
      int left_chunk = chunk*2 - 1;
      int right_chunk = chunk*2;
      int left_chunk_end = left_chunk * (1 << pow);
      if (left_chunk_end < len) {
        bool pushed = _queue->push(SCMTask(array, left_chunk, pow));
        assert(pushed, "overflow queue should always succeed pushing");
        chunk = right_chunk;
        last_idx = left_chunk_end;
      } else {
        chunk = left_chunk;
      }
    }

    // Process the irregular tail, if present
    int from = last_idx;
    if (from < len) {
      array->oop_iterate_range(&_mark_refs, from, len);
    }
  }
}

template <class T, bool CL>
inline void ShenandoahMarkObjsClosure<T, CL>::do_chunked_array(oop obj, int chunk, int pow) {
  assert(obj->is_objArray(), "expect object array");
  objArrayOop array = objArrayOop(obj);

  assert (ObjArrayMarkingStride > 0, "sanity");

  // Split out tasks, as suggested in ObjArrayChunkedTask docs. Avoid pushing tasks that
  // are known to start beyond the array.
  while ((1 << pow) > (int)ObjArrayMarkingStride && (chunk*2 < SCMTask::chunk_size)) {
    pow--;
    chunk *= 2;
    bool pushed = _queue->push(SCMTask(array, chunk - 1, pow));
    assert(pushed, "overflow queue should always succeed pushing");
  }

  int chunk_size = 1 << pow;

  int from = (chunk - 1) * chunk_size;
  int to = chunk * chunk_size;

#ifdef ASSERT
  int len = array->length();
  assert (0 <= from && from < len, err_msg("from is sane: %d/%d", from, len));
  assert (0 < to && to <= len, err_msg("to is sane: %d/%d", to, len));
#endif

  array->oop_iterate_range(&_mark_refs, from, to);
}

inline bool ShenandoahConcurrentMark::try_queue(SCMObjToScanQueue* q, SCMTask &task) {
  return (q->pop_buffer(task) ||
          q->pop_local(task) ||
          q->pop_overflow(task));
}

class ShenandoahSATBBufferClosure : public SATBBufferClosure {
private:
  SCMObjToScanQueue* _queue;
  ShenandoahHeap* _heap;
public:
  ShenandoahSATBBufferClosure(SCMObjToScanQueue* q) :
    _queue(q), _heap(ShenandoahHeap::heap())
  {
  }

  void do_buffer(void** buffer, size_t size) {
    for (size_t i = 0; i < size; ++i) {
      oop* p = (oop*) &buffer[i];
      ShenandoahConcurrentMark::mark_through_ref<oop, RESOLVE>(p, _heap, _queue);
    }
  }
};

inline bool ShenandoahConcurrentMark::try_draining_satb_buffer(SCMObjToScanQueue *q, SCMTask &task) {
  ShenandoahSATBBufferClosure cl(q);
  SATBMarkQueueSet& satb_mq_set = JavaThread::satb_mark_queue_set();
  bool had_refs = satb_mq_set.apply_closure_to_completed_buffer(&cl);
  return had_refs && try_queue(q, task);
}

template<class T, UpdateRefsMode UPDATE_REFS>
inline void ShenandoahConcurrentMark::mark_through_ref(T *p, ShenandoahHeap* heap, SCMObjToScanQueue* q) {
  T o = oopDesc::load_heap_oop(p);
  if (! oopDesc::is_null(o)) {
    oop obj = oopDesc::decode_heap_oop_not_null(o);
    switch (UPDATE_REFS) {
    case NONE:
      break;
    case RESOLVE:
      obj = ShenandoahBarrierSet::resolve_oop_static_not_null(obj);
      break;
    case SIMPLE:
      // We piggy-back reference updating to the marking tasks.
      obj = heap->update_oop_ref_not_null(p, obj);
      break;
    case CONCURRENT:
      obj = heap->maybe_update_oop_ref_not_null(p, obj);
      break;
    default:
      ShouldNotReachHere();
    }
    assert(oopDesc::unsafe_equals(obj, ShenandoahBarrierSet::resolve_oop_static(obj)), "need to-space object here");

    // Note: Only when concurrently updating references can obj become NULL here.
    // It happens when a mutator thread beats us by writing another value. In that
    // case we don't need to do anything else.
    if (UPDATE_REFS != CONCURRENT || !oopDesc::is_null(obj)) {
      assert(!oopDesc::is_null(obj), "Must not be null here");
      assert(heap->is_in(obj), err_msg("We shouldn't be calling this on objects not in the heap: " PTR_FORMAT, p2i(obj)));
      assert(oopDesc::bs()->is_safe(obj), "Only mark objects in from-space");
      if (heap->mark_next(obj)) {
        log_develop_trace(gc, marking)("Marked obj: " PTR_FORMAT, p2i((HeapWord*) obj));

        bool pushed = q->push(SCMTask(obj));
        assert(pushed, "overflow queue should always succeed pushing");
      } else {
        log_develop_trace(gc, marking)("Failed to mark obj (already marked): " PTR_FORMAT, p2i((HeapWord*) obj));
        assert(heap->is_marked_next(obj), "Consistency: should be marked.");
      }
    }
  }
}

#endif // SHARE_VM_GC_SHENANDOAH_SHENANDOAHCONCURRENTMARK_INLINE_HPP
