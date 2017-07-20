/*
 * Copyright (c) 2016, Red Hat, Inc. and/or its affiliates.
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
#include "gc_implementation/shenandoah/shenandoahCollectionSet.hpp"
#include "gc_implementation/shenandoah/shenandoahCollectionSet.inline.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.inline.hpp"
#include "gc_implementation/shenandoah/shenandoahHeapRegion.hpp"
#include "gc_implementation/shenandoah/shenandoahHeapRegionSet.hpp"
#include "runtime/atomic.hpp"
#include "utilities/copy.hpp"

ShenandoahCollectionSet::ShenandoahCollectionSet(ShenandoahHeap* heap, HeapWord* heap_base) :
        _garbage(0), _live_data(0), _heap(heap), _region_count(0),
        _map_size(heap->max_regions()), _current_index(0) {
  // Use 1-byte data type
  STATIC_ASSERT(sizeof(jbyte) == 1);

  _cset_map = NEW_C_HEAP_ARRAY(jbyte, _map_size, mtGC);
  // Bias cset map's base address for fast test if an oop is in cset
  _biased_cset_map = _cset_map - ((uintx)heap_base >> ShenandoahHeapRegion::region_size_shift());

  // Initialize cset map
  Copy::zero_to_bytes(_cset_map, _map_size);
}

void ShenandoahCollectionSet::add_region(ShenandoahHeapRegion* r) {
  assert(SafepointSynchronize::is_at_safepoint(), "Must be at a safepoint");
  assert(Thread::current()->is_VM_thread(), "Must be VMThread");
  assert(!is_in(r), "Already in collection set");
  _cset_map[r->region_number()] = 1;
  _region_count ++;
  _garbage += r->garbage();
  _live_data += r->get_live_data_bytes();
}

void ShenandoahCollectionSet::remove_region(ShenandoahHeapRegion* r) {
  assert(SafepointSynchronize::is_at_safepoint(), "Must be at a safepoint");
  assert(Thread::current()->is_VM_thread(), "Must be VMThread");
  assert(is_in(r), "Not in collection set");
  _cset_map[r->region_number()] = 0;
  _region_count --;
}

void ShenandoahCollectionSet::clear() {
  assert(SafepointSynchronize::is_at_safepoint(), "Must be at a safepoint");
  Copy::zero_to_bytes(_cset_map, _map_size);

  _garbage = 0;
  _live_data = 0;

  _region_count = 0;
  _current_index = 0;
}

ShenandoahHeapRegion* ShenandoahCollectionSet::claim_next() {
  size_t num_regions = _heap->num_regions();
  if (_current_index >= (jint)num_regions) {
    return NULL;
  }

  jint saved_current = _current_index;
  size_t index = (size_t)saved_current;

  while(index < num_regions) {
    if (is_in(index)) {
      jint cur = Atomic::cmpxchg((jint)(index + 1), &_current_index, saved_current);
      assert(cur >= (jint)saved_current, "Must move forward");
      if (cur == saved_current) {
        assert(is_in(index), "Invariant");
        return _heap->regions()->get(index);
      } else {
        index = (size_t)cur;
        saved_current = cur;
      }
    } else {
      index ++;
    }
  }
  return NULL;
}


ShenandoahHeapRegion* ShenandoahCollectionSet::next() {
  assert(SafepointSynchronize::is_at_safepoint(), "Must be at a safepoint");
  assert(Thread::current()->is_VM_thread(), "Must be VMThread");
  size_t num_regions = _heap->num_regions();
  for (size_t index = (size_t)_current_index; index < num_regions; index ++) {
    if (is_in(index)) {
      _current_index = (jint)(index + 1);
      return _heap->regions()->get(index);
    }
  }

  return NULL;
}


void ShenandoahCollectionSet::print(outputStream* out) const {
  out->print_cr("Collection Set : " SIZE_FORMAT "", count());

  debug_only(size_t regions = 0;)
  for (size_t index = 0; index < _heap->num_regions(); index ++) {
    if (is_in(index)) {
      _heap->regions()->get(index)->print_on(out);
      debug_only(regions ++;)
    }
  }
  assert(regions == count(), "Must match");
}
