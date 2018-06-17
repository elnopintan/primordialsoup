// Copyright (c) 2016, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "vm/heap.h"

#include "vm/isolate.h"  // TODO: Move the thread pool.
#include "vm/lockers.h"
#include "vm/lookup_cache.h"
#include "vm/os.h"
#include "vm/thread_pool.h"

namespace psoup {

class HeapPage {
 public:
  static HeapPage* Allocate(intptr_t size) {
    VirtualMemory memory = VirtualMemory::Allocate(size,
                                                   VirtualMemory::kReadWrite,
                                                   "primordialsoup-heap");
    HeapPage* page = reinterpret_cast<HeapPage*>(memory.base());
    page->memory_ = memory;
    page->object_end_ = page->object_start();
    return page;
  }

  void Free() { memory_.Free(); }

  uword TryAllocate(intptr_t size) {
    ASSERT(Utils::IsAligned(size, kObjectAlignment));
    uword result = object_end_;
    intptr_t remaining = memory_.limit() - object_end_;
    if (remaining < size) {
      return 0;
    }
    ASSERT((result & kObjectAlignmentMask) == kOldObjectAlignmentOffset);
    object_end_ += size;
    return result;
  }

  uword size() const { return memory_.size(); }
  uword limit() const { return memory_.limit(); }
  uword object_start() const {
    return reinterpret_cast<uword>(this) + AllocationSize(sizeof(HeapPage));
  }
  uword object_end() const { return object_end_; }
  void set_object_end(uword value) { object_end_ = value; }

  size_t Size() const { return object_end() - object_start(); }

  HeapPage* next() const { return next_; }
  void set_next(HeapPage* next) { next_ = next; }

 private:
  HeapPage* next_;
  VirtualMemory memory_;
  uword object_end_;
};

class ConcurrentMarkTask : public ThreadPool::Task {
 public:
  explicit ConcurrentMarkTask(Heap* heap) : heap_(heap) {}

  virtual void Run() { heap_->ConcurrentMark(); }

 private:
  Heap* const heap_;

  DISALLOW_COPY_AND_ASSIGN(ConcurrentMarkTask);
};

Heap::Heap() :
    top_(0),
    end_(0),
    survivor_end_(0),
    to_(),
    from_(),
    next_semispace_capacity_(kInitialSemispaceCapacity),
    pages_(NULL),
    freelist_(),
    old_size_(0),
    old_capacity_(0),
    old_limit_(0),
    remembered_set_(NULL),
    remembered_set_size_(0),
    remembered_set_capacity_(0),
    class_table_(NULL),
    class_table_size_(0),
    class_table_capacity_(0),
    class_table_free_(0),
    object_store_(NULL),
    current_activation_(NULL),
    handles_(),
    handles_size_(0),
#if RECYCLE_ACTIVATIONS
    recycle_list_(NULL),
#endif
#if LOOKUP_CACHE
    lookup_cache_(NULL),
#endif
    ephemeron_list_(NULL),
    weak_list_(NULL) {
  to_.Allocate(kInitialSemispaceCapacity);
  from_.Allocate(kInitialSemispaceCapacity);
  top_ = to_.object_start();
  end_ = to_.limit();

  remembered_set_capacity_ = 1024;
  remembered_set_ = new HeapObject*[remembered_set_capacity_];

  // Class table.
  class_table_capacity_ = 1024;
  class_table_ = new Object*[class_table_capacity_];
#if defined(DEBUG)
  for (intptr_t i = 0; i < kFirstRegularObjectCid; i++) {
    class_table_[i] = reinterpret_cast<Object*>(kUninitializedWord);
  }
  for (intptr_t i = kFirstRegularObjectCid; i < class_table_capacity_; i++) {
    class_table_[i] = reinterpret_cast<Object*>(kUnallocatedWord);
  }
#endif
  class_table_size_ = kFirstRegularObjectCid;

  mutator_mark_block_ = new MarkBlock();
  collector_mark_block_ = new MarkBlock();
  shared_mark_block_ = NULL;

  marking_monitor_ = new Monitor();
  marking_state_ = kComplete;
  concurrent_marked_ = 0;
}

static void DeleteMarkBlockList(MarkBlock* block) {
  while (block != NULL) {
    MarkBlock* next = block->next();
    block->set_next(NULL);
    block->Reset();
    delete block;
    block = next;
  }
}

Heap::~Heap() {
  WaitForMarker(); // TODO: Interrupt the marker.
  DeleteMarkBlockList(mutator_mark_block_);
  DeleteMarkBlockList(shared_mark_block_);
  DeleteMarkBlockList(collector_mark_block_);
  delete marking_monitor_;

  to_.Free();
  from_.Free();
  delete[] remembered_set_;
  delete[] class_table_;
}

uword Heap::AllocateNew(intptr_t size) {
  ASSERT(size < kLargeAllocation);
  uword addr = TryAllocateNew(size);
  if (addr == 0) {
    Scavenge(kNewSpace);
    addr = TryAllocateNew(size);
    if (addr == 0) {
      return AllocateOldSmall(size, kControlGrowth);
    }
  }
#if defined(DEBUG)
  memset(reinterpret_cast<void*>(addr), kUninitializedByte, size);
#endif
  return addr;
}

uword Heap::AllocateTenure(intptr_t size) {
  ASSERT(size < kLargeAllocation);
  uword result = freelist_.TryAllocate(size);
  if (result != 0) {
    old_size_ += size;
  } else {
    result = AllocateOldSmall(size, kForceGrowth);
    ASSERT(result != 0);
  }
  PushTenureStack(result);
  return result;
}

uword Heap::AllocateOldSmall(intptr_t size, GrowthPolicy growth) {
  ASSERT(size < kLargeAllocation);
  uword addr = freelist_.TryAllocate(size);
  if (addr == 0) {
    HeapPage* page = AllocatePage(kPageSize, growth);
    addr = page->TryAllocate(size);
    intptr_t remaining = page->limit() - page->object_end();
    if (remaining > 0) {
      freelist_.EnqueueRange(page->object_end(), remaining);
      page->set_object_end(page->limit());
    }
  }
  if (addr == 0) {
    FATAL1("Failed to allocate %" Pd " bytes\n", size);
  }
  old_size_ += size;
#if defined(DEBUG)
  memset(reinterpret_cast<void*>(addr), kUninitializedByte, size);
#endif
  return addr;
}

uword Heap::AllocateOldLarge(intptr_t size, GrowthPolicy growth) {
  ASSERT(size >= kLargeAllocation);
  HeapPage* page = AllocatePage(size + AllocationSize(sizeof(HeapPage)),
                                growth);
  uword addr = page->TryAllocate(size);
  if (addr == 0) {
    FATAL1("Failed to allocate %" Pd " bytes\n", size);
  }
  old_size_ += size;
#if defined(DEBUG)
  memset(reinterpret_cast<void*>(addr), kUninitializedByte, size);
#endif
  return addr;
}

uword Heap::AllocateSnapshotSmall(intptr_t size) {
  ASSERT(size < kLargeAllocation);
  HeapPage* page = pages_;
  if (page == NULL) {
    page = AllocatePage(kPageSize, kForceGrowth);
  }
  uword addr = page->TryAllocate(size);
  if (addr == 0) {
    intptr_t remaining = page->limit() - page->object_end();
    if (remaining > 0) {
      freelist_.EnqueueRange(page->object_end(), remaining);
      page->set_object_end(page->limit());
    }

    page = AllocatePage(kPageSize, kForceGrowth);
    addr = page->TryAllocate(size);
  }
  if (addr == 0) {
    FATAL1("Failed to allocate %" Pd " bytes\n", size);
  }
  old_size_ += size;
#if defined(DEBUG)
  memset(reinterpret_cast<void*>(addr), kUninitializedByte, size);
#endif
  return addr;
}

uword Heap::AllocateSnapshotLarge(intptr_t size) {
  ASSERT(size >= kLargeAllocation);
  uword addr;
  HeapPage* page = HeapPage::Allocate(size + AllocationSize(sizeof(HeapPage)));
  old_capacity_ += page->size();
  // Keep the current page since it likely still has free space.
  if (pages_ == NULL) {
    pages_ = page;
  } else {
    page->set_next(pages_->next());
    pages_->set_next(page);
  }
  addr = page->TryAllocate(size);
  if (addr == 0) {
    FATAL1("Failed to allocate %" Pd " bytes\n", size);
  }
  old_size_ += size;
#if defined(DEBUG)
  memset(reinterpret_cast<void*>(addr), kUninitializedByte, size);
#endif
  return addr;
}

HeapPage* Heap::AllocatePage(intptr_t page_size, GrowthPolicy growth) {
  if (growth == kControlGrowth) {
    if ((old_size_ + page_size) > old_limit_) {
      MarkSweep(kOldSpace);
    } else {
      EvaluateConcurrentMarking();
    }
  }
  HeapPage* page = HeapPage::Allocate(page_size);
  old_capacity_ += page->size();
  page->set_next(pages_);
  pages_ = page;
  return page;
}

void Heap::GrowRememberedSet() {
  // TODO(rmacnak): Investigate a limit to trigger GC instead of letting this
  // grow in an unbounded way.
  remembered_set_capacity_ += (remembered_set_capacity_ >> 1);
  if (TRACE_GROWTH) {
    OS::PrintErr("Growing remembered set to %" Pd "\n",
                 remembered_set_capacity_);
  }
  HeapObject** old_remembered_set = remembered_set_;
  remembered_set_ = new HeapObject*[remembered_set_capacity_];
  for (intptr_t i = 0; i < remembered_set_size_; i++) {
    remembered_set_[i] = old_remembered_set[i];
  }
  delete[] old_remembered_set;
}

void Heap::ShrinkRememberedSet() {
  intptr_t preferred_capacity =
      Utils::RoundUp(remembered_set_size_ + (remembered_set_size_ >> 1) + 1,
                     KB);

  if (remembered_set_capacity_ <= preferred_capacity) {
    return;
  }
  remembered_set_capacity_ = preferred_capacity;
  if (TRACE_GROWTH) {
    OS::PrintErr("Shrinking remembered set to %" Pd "\n",
                 remembered_set_capacity_);
  }
  HeapObject** old_remembered_set = remembered_set_;
  remembered_set_ = new HeapObject*[remembered_set_capacity_];
  for (intptr_t i = 0; i < remembered_set_size_; i++) {
    remembered_set_[i] = old_remembered_set[i];
  }
  delete[] old_remembered_set;
}

void Heap::Scavenge(Reason reason) {
#if REPORT_GC
  int64_t start = OS::CurrentMonotonicNanos();
  size_t new_before = top_ - to_.object_start();
#endif
  size_t old_before = old_size_;

  FlipSpaces();

#if defined(DEBUG)
  to_.ReadWrite();
#endif

  // We treat all references as strong during a scavenge. WeakArray::next_ and
  // Ephemeron::next_ may be in use by the concurrent marker.
  ScavengeRoots();
  uword scan = to_.object_start();
  while (scan < top_ || end_ < to_.limit()) {
    scan = ScavengeToSpace(scan);
    ProcessTenureStack();
  }

#if defined(DEBUG)
  from_.MarkUnallocated();
  from_.NoAccess();
#endif

  ClearCaches();

  survivor_end_ = top_;

  size_t new_after = top_ - to_.object_start();
  size_t old_after = old_size_;
  size_t tenured = old_after - old_before;
  size_t survived = new_after + tenured;

  if (survived > (to_.size() / 3)) {
    next_semispace_capacity_ = to_.size() * 2;
    if (next_semispace_capacity_ > kMaxSemispaceCapacity) {
      next_semispace_capacity_ = kMaxSemispaceCapacity;
    }
  }

#if REPORT_GC
  size_t freed = (new_before + old_before) - (new_after + old_after);
  int64_t stop = OS::CurrentMonotonicNanos();
  int64_t time = stop - start;
  OS::PrintErr("Scavenge (%s, %" Pd "kB new, "
               "%" Pd "kB tenured, %" Pd "kB freed, %" Pd64 " us)\n",
               ReasonToCString(reason), new_after / KB, tenured / KB,
               freed / KB, time / kNanosecondsPerMicrosecond);
#endif

  ASSERT(reason == kNewSpace ||
         reason == kClassTable ||
         reason == kPrimitive ||
         reason == kSnapshotTest);
  // kClassTable and kPrimitive will follow up with a MarkSweep anyway, so don't
  // perform an extra one for tenure.
  if (reason == kNewSpace) {
    if (old_size_ > old_limit_) {
      MarkSweep(kTenure);
    } else {
      EvaluateConcurrentMarking();
    }
  }
}

void Heap::FlipSpaces() {
  Semispace temp = to_;
  to_ = from_;
  from_ = temp;

  ASSERT(next_semispace_capacity_ <= kMaxSemispaceCapacity);
  if (to_.size() < next_semispace_capacity_) {
    if (TRACE_GROWTH && (from_.size() < next_semispace_capacity_)) {
      OS::PrintErr("Growing new space to %" Pd "MB\n",
                   next_semispace_capacity_ / MB);
    }
    to_.Free();
    to_.Allocate(next_semispace_capacity_);
  }

  ASSERT(to_.size() >= from_.size());

  top_ = to_.object_start();
  end_ = to_.limit();
  ASSERT((top_ & kObjectAlignmentMask) == kNewObjectAlignmentOffset);
}

static void ForwardClass(Heap* heap, HeapObject* object) {
  ASSERT(object->IsHeapObject());
  Behavior* old_class = heap->ClassAt(object->cid());
  if (old_class->IsForwardingCorpse()) {
    Behavior* new_class = static_cast<Behavior*>(
        reinterpret_cast<ForwardingCorpse*>(old_class)->target());
    ASSERT(!new_class->IsForwardingCorpse());
    new_class->AssertCouldBeBehavior();
    if (new_class->id() == heap->object_store()->nil_obj()) {
      ASSERT(old_class->id()->IsSmallInteger());
      new_class->set_id(old_class->id());
    }
    object->set_cid(new_class->id()->value());
  }
}

static void ForwardPointer(Object** ptr) {
  Object* old_target = *ptr;
  if (old_target->IsForwardingCorpse()) {
    Object* new_target = static_cast<ForwardingCorpse*>(old_target)->target();
    ASSERT(!new_target->IsForwardingCorpse());
    *ptr = new_target;
  }
}

void Heap::ScavengeRoots() {
  // Process the remembered set first so we can visit and reset in one pass.
  intptr_t saved_remembered_set_size = remembered_set_size_;
  remembered_set_size_ = 0;

  for (intptr_t i = 0; i < saved_remembered_set_size; i++) {
    HeapObject* obj = remembered_set_[i];
    if (obj == NULL) {
      continue;
    }
    ASSERT(obj->IsOldObject());
    ASSERT(obj->is_remembered());
    obj->set_is_remembered(false);
    ScavengeOldObject(obj);
  }

  ScavengePointer(reinterpret_cast<Object**>(&object_store_));
  ScavengePointer(reinterpret_cast<Object**>(&current_activation_));

  for (intptr_t i = 0; i < handles_size_; i++) {
    ScavengePointer(handles_[i]);
  }

  for (intptr_t i = kFirstLegalCid; i < class_table_size_; i++) {
    ScavengePointer(&class_table_[i]);
  }
}

uword Heap::ScavengeToSpace(uword scan) {
  while (scan < top_) {
    HeapObject* obj = HeapObject::FromAddr(scan);
    Object** from;
    Object** to;
    obj->Pointers(&from, &to);
    for (Object** ptr = from; ptr <= to; ptr++) {
      ScavengePointer(ptr);
    }
    scan += obj->HeapSize();
  }
  return scan;
}

void Heap::PushTenureStack(uword addr) {
  ASSERT(end_ > top_);
  ASSERT(end_ <= to_.limit());
  end_ -= sizeof(addr);
  *reinterpret_cast<uword*>(end_) = addr;
}

uword Heap::PopTenureStack() {
  ASSERT(end_ > top_);
  ASSERT(end_ <= to_.limit());
  uword addr = *reinterpret_cast<uword*>(end_);
  end_ += sizeof(uword);
  return addr;
}

bool Heap::IsTenureStackEmpty() {
  ASSERT(end_ > top_);
  ASSERT(end_ <= to_.limit());
  return end_ == to_.limit();
}

void Heap::ProcessTenureStack() {
  while (!IsTenureStackEmpty()) {
    HeapObject* obj = HeapObject::FromAddr(PopTenureStack());
    ScavengeOldObject(obj);

    // See ForwardPointer.
    if (marking_state_ != kComplete) {
      ASSERT(obj->is_marked());
      AddToMarkStack(obj);
    }
  }
}

static bool IsForwarded(HeapObject* obj) {
  uword header = *reinterpret_cast<uword*>(obj->Addr());
  return header & (1 << kMarkBit);
}

static HeapObject* ForwardingTarget(HeapObject* obj) {
  ASSERT(IsForwarded(obj));
  uword header = *reinterpret_cast<uword*>(obj->Addr());
  // Mark bit and tag bit are conveniently in the same place.
  ASSERT((header & kSmiTagMask) == kHeapObjectTag);
  return reinterpret_cast<HeapObject*>(header);
}

static void SetForwarded(HeapObject* old_obj, HeapObject* new_obj) {
  ASSERT(old_obj->IsNewObject());
  ASSERT(!IsForwarded(old_obj));
  uword header = reinterpret_cast<uword>(new_obj);
  // Mark bit and tag bit are conveniently in the same place.
  ASSERT((header & kSmiTagMask) == kHeapObjectTag);
  *reinterpret_cast<uword*>(old_obj->Addr()) = header;
}

void Heap::ScavengePointer(Object** ptr) {
  HeapObject* old_target = static_cast<HeapObject*>(*ptr);
  if (old_target->IsImmediateOrOldObject()) {
    return;
  }

  DEBUG_ASSERT(InFromSpace(old_target));

  HeapObject* new_target;
  if (IsForwarded(old_target)) {
    new_target = ForwardingTarget(old_target);
  } else {
    // Target is now known to be reachable. Move it to to-space.
    intptr_t size = old_target->HeapSize();

    uword new_target_addr;
    if (old_target->Addr() < survivor_end_) {
      new_target_addr = AllocateTenure(size);
    } else {
      new_target_addr = TryAllocateNew(size);
    }

    ASSERT(new_target_addr != 0);
    memcpy(reinterpret_cast<void*>(new_target_addr),
           reinterpret_cast<void*>(old_target->Addr()),
           size);
    new_target = HeapObject::FromAddr(new_target_addr);

    if (new_target->IsOldObject() && (marking_state_ != kComplete)) {
      // Setting the forwarding pointer below will make this tenured object
      // visible to the concurrent marker, but we haven't visited its slots yet.
      // We mark the object here so the concurrent marker won't add it to the
      // mark stack and visit its slots. We'll push it to the mark stack after
      // forwarding its slots in ProcessTenureStack.
      new_target->set_is_marked(true);  // TODO: order_relaxed
    }

    SetForwarded(old_target, new_target);
  }

  DEBUG_ASSERT(new_target->IsOldObject() || InToSpace(new_target));

  *ptr = new_target;
}

void Heap::ScavengeOldObject(HeapObject* obj) {
  Object** from;
  Object** to;
  obj->Pointers(&from, &to);
  for (Object** ptr = from; ptr <= to; ptr++) {
    ScavengePointer(ptr);
    if ((*ptr)->IsNewObject() && !obj->is_remembered()) {
      AddToRememberedSet(obj);
    }
  }
}

void Heap::EvaluateConcurrentMarking() {
  MarkingState state;
  {
    MonitorLocker ml(marking_monitor_);
    state = marking_state_;
  }

  if (state == kAwaitingFinalization) {
#if TRACE_CONCURRENT_MARK
    OS::PrintErr("Finalizing marking\n");
#endif
    MarkSweep(kFinalize);
  } else if ((state == kComplete) && (old_size_ + to_.size()/2 > old_limit_)) {
#if TRACE_CONCURRENT_MARK
    OS::PrintErr("Begin concurrent marking\n");
#endif
    bool visit_weak = false;
    MarkRoots(visit_weak);
    marking_state_ = kMarking;
    HeapObject::is_marking = true;  // Enable write barrier.
    Isolate::thread_pool()->Run(new ConcurrentMarkTask(this));
  }
}

void Heap::ConcurrentMark() {
  ASSERT(marking_state_ == kMarking);
#if TRACE_CONCURRENT_MARK
  int64_t start = OS::CurrentMonotonicNanos();
#endif

  size_t marked = 0;
  while (!collector_mark_block_->IsEmpty() ||
         collector_mark_block_->next() != NULL) {
    marked += ProcessMarkStack();
    MarkEphemeronList();
  }

#if TRACE_CONCURRENT_MARK
  int64_t stop = OS::CurrentMonotonicNanos();
  OS::PrintErr("CON marked %" Pd " kB in %" Pd64 "us\n", marked / KB,
               (stop - start) / kNanosecondsPerMicrosecond);
#endif

  concurrent_marked_ = marked;

  MonitorLocker ml(marking_monitor_);
  marking_state_ = kAwaitingFinalization;
  ml.Notify();
}

void Heap::WaitForMarker() {
  MonitorLocker ml(marking_monitor_);
  while (marking_state_ == kMarking) {
    ml.Wait();
  }
}

void Heap::CollectAll(Reason reason) {
  // TODO: Instead interrupt the marker, abandon the mark stack, and reset the
  // mark bits.
  MarkSweep(kFinalize);

  survivor_end_ = end_;  // Tenure everything.
  Scavenge(reason);
  MarkSweep(reason);
}

void Heap::MarkSweep(Reason reason) {
  int64_t start = OS::CurrentMonotonicNanos();
#if REPORT_GC
  size_t size_before = old_size_;
#endif

  WaitForMarker();

  // Move all marking blocks over to the collector's linked list.
  MutatorReleaseBlock();
  collector_mark_block_->set_next(shared_mark_block_);
  shared_mark_block_ = NULL;

  // Explicitly mark nil since we skip the barrier for various initializing
  // stores.
  MarkObject(object_store()->nil_obj());

  // Strong references.
  MarkRoots(true);
  size_t stw_marked = 0;
  while (!collector_mark_block_->IsEmpty() ||
         collector_mark_block_->next() != NULL) {
    stw_marked += ProcessMarkStack();
    MarkEphemeronList();
  }

  ASSERT(collector_mark_block_ != NULL);
  ASSERT(collector_mark_block_->IsEmpty());
  ASSERT(mutator_mark_block_ != NULL);
  ASSERT(mutator_mark_block_->IsEmpty());
  ASSERT(shared_mark_block_.load() == NULL);

  marking_state_ = kComplete;
  HeapObject::is_marking = false;  // Disable write barrier.

  ASSERT((stw_marked + concurrent_marked_) <= old_size_);
  old_size_ = stw_marked + concurrent_marked_;
  ASSERT(old_size_ <= old_capacity_);
  concurrent_marked_ = 0;

#if TRACE_CONCURRENT_MARK
  int64_t mid = OS::CurrentMonotonicNanos();
  OS::PrintErr("STW marked %" Pd " kB in %" Pd64 "us\n", stw_marked / KB,
               (mid - start) / kNanosecondsPerMicrosecond);
#endif

  // Weak references.
  MournEphemeronList();
  MournWeakListMarkSweep();
  MournClassTableMarkSweep();
  MournRememberedSet();

  ClearCaches();

  Sweep();  // TODO: Concurrent sweep.

  SetOldAllocationLimit();

#if REPORT_GC
  size_t size_after = old_size_;
  int64_t stop = OS::CurrentMonotonicNanos();
  int64_t time = stop - start;
  OS::PrintErr("Mark-sweep "
               "(%s, %" Pd "kB old, %" Pd "kB freed, %" Pd64 " us)\n",
               ReasonToCString(reason), size_after / KB,
               (size_before - size_after) / KB,
               time / kNanosecondsPerMicrosecond);
#endif
}

void Heap::MarkRoots(bool visit_weak) {
  MarkObject(object_store_);
  MarkObject(current_activation_);

  for (intptr_t i = 0; i < handles_size_; i++) {
    MarkObject(*handles_[i]);
  }

  // TODO(rmacnak): Investigate marking through new space.
  uword scan = to_.object_start();
  while (scan < top_) {
    HeapObject* obj = HeapObject::FromAddr(scan);
    if (!obj->IsForwardingCorpse() &&
        !obj->IsFreeListElement()) {
      intptr_t cid = obj->cid();
      ASSERT(cid != kIllegalCid);
      ASSERT(cid != kForwardingCorpseCid);
      ASSERT(cid != kFreeListElementCid);

      MarkObject(ClassAt(cid));

      if (cid == kWeakArrayCid) {
        if (visit_weak) {
          AddToWeakList(static_cast<WeakArray*>(obj));
        }
      } else if (cid == kEphemeronCid) {
        if (visit_weak) {
          AddToEphemeronList(static_cast<Ephemeron*>(obj));
        }
      } else {
        Object** from;
        Object** to;
        obj->Pointers(&from, &to);
        for (Object** ptr = from; ptr <= to; ptr++) {
          MarkObject(*ptr);
        }
      }
    }
    scan += obj->HeapSize();
  }
}

void Heap::MarkObject(Object* obj) {
  if (obj->IsImmediateOrNewObject()) return;

  HeapObject* heap_obj = static_cast<HeapObject*>(obj);
  if (!heap_obj->TryAcquireMarkBit()) return;

  if (collector_mark_block_->Push(heap_obj)) {
    return;
  }
  MarkBlock* new_block = new MarkBlock();
  new_block->set_next(collector_mark_block_);
  collector_mark_block_ = new_block;
  bool success = collector_mark_block_->Push(heap_obj);
  ASSERT(success);
}

size_t Heap::ProcessMarkStack() {
  size_t marked = 0;
 again:
  while (!collector_mark_block_->IsEmpty()) {
    HeapObject* obj = collector_mark_block_->Pop();
    ASSERT(obj->IsOldObject());
    ASSERT(obj->is_marked());

    intptr_t cid = obj->cid();
    ASSERT(cid != kIllegalCid);
    ASSERT(cid != kForwardingCorpseCid);
    ASSERT(cid != kFreeListElementCid);

    marked += obj->HeapSize();

    MarkObject(ClassAt(cid));

    if (cid == kWeakArrayCid) {
      AddToWeakList(static_cast<WeakArray*>(obj));
    } else if (cid == kEphemeronCid) {
      AddToEphemeronList(static_cast<Ephemeron*>(obj));
    } else {
      Object** from;
      Object** to;
      obj->Pointers(&from, &to);
      for (Object** ptr = from; ptr <= to; ptr++) {
        Object* target = *ptr;
        MarkObject(target);
      }
    }
  }

  MarkBlock* next = collector_mark_block_->next();
  if (next != NULL) {
    collector_mark_block_->set_next(NULL);
    delete collector_mark_block_;
    collector_mark_block_ = next;
    goto again;
  }
  next = shared_mark_block_.exchange(NULL, std::memory_order_acquire);
  if (next != NULL) {
    collector_mark_block_->set_next(NULL);
    delete collector_mark_block_;
    collector_mark_block_ = next;
    goto again;
  }

  return marked;
}

void Heap::Sweep() {
  freelist_.Reset();

  HeapPage* prev = NULL;
  HeapPage* page = pages_;
  while (page != NULL) {
    bool in_use = SweepPage(page);
    if (in_use) {
      prev = page;
      page = page->next();
    } else {
      HeapPage* next = page->next();
      if (prev == NULL) {
        pages_ = next;
      } else {
        prev->set_next(next);
      }
      old_capacity_ -= page->size();
      page->Free();
      page = next;
    }
  }
}

bool Heap::SweepPage(HeapPage* page) {
  uword scan = page->object_start();
  uword end = page->object_end();
  while (scan < end) {
    HeapObject* obj = HeapObject::FromAddr(scan);
    if (obj->is_marked()) {
      obj->set_is_marked(false);
      scan += obj->HeapSize();
    } else {
      uword free_scan = scan + obj->HeapSize();
      while (free_scan < end) {
        HeapObject* next = HeapObject::FromAddr(free_scan);
        if (next->is_marked()) break;
        free_scan += next->HeapSize();
      }

      if ((scan == page->object_start()) && (free_scan == end)) {
        // Note that HeapPage::Of relies on us releasing large pages instead of
        // adding them to the freelist.
        return false;  // Not in use.
      }

      freelist_.EnqueueRange(scan, free_scan - scan);
      scan = free_scan;
    }
  }
  return true;  // In use.
}

void Heap::SetOldAllocationLimit() {
  old_limit_ = old_size_ + old_size_ / 2;
  if (old_limit_ < old_size_ + 2 * kPageSize) {
    old_limit_ = old_size_ + 2 * kPageSize;
  }
  if (TRACE_GROWTH) {
    OS::PrintErr("Old %" Pd "kB size, %" Pd "kB capacity, %" Pd "kB limit\n",
                 old_size_ / KB, old_capacity_ / KB, old_limit_ / KB);
  }
}

void Heap::AddToEphemeronList(Ephemeron* survivor) {
  DEBUG_ASSERT(survivor->IsOldObject() || InToSpace(survivor));
  survivor->set_next(ephemeron_list_);
  ephemeron_list_ = survivor;
}

static bool IsMarkSweepSurvivor(Object* obj) {
  return obj->IsImmediateOrNewObject() ||
      static_cast<HeapObject*>(obj)->is_marked();
}

void Heap::MarkEphemeronList() {
  Ephemeron* survivor = ephemeron_list_;
  ephemeron_list_ = NULL;

  while (survivor != NULL) {
    ASSERT(survivor->IsEphemeron());
    Ephemeron* next = survivor->next();
    survivor->set_next(NULL);

    if (IsMarkSweepSurvivor(survivor->key())) {
      // TODO(rmacnak): These scavenges potentially add to the ephemeron list
      // that we are in the middle of traversing. Add tests for ephemerons
      // only reachable from another ephemeron.
      MarkObject(survivor->key());
      MarkObject(survivor->value());
      MarkObject(survivor->finalizer());
    } else {
      // Fate of the key is not yet known; add the ephemeron back to the list.
      survivor->set_next(ephemeron_list_);
      ephemeron_list_ = survivor;
    }

    survivor = next;
  }
}

void Heap::MournEphemeronList() {
  Object* nil = object_store()->nil_obj();
  Ephemeron* survivor = ephemeron_list_;
  ephemeron_list_ = NULL;

  while (survivor != NULL) {
    ASSERT(survivor->IsEphemeron());

    DEBUG_ASSERT(survivor->key()->IsOldObject() ||
                 InFromSpace(static_cast<HeapObject*>(survivor->key())));
    survivor->set_key(nil, kNoBarrier);
    survivor->set_value(nil, kNoBarrier);
    // TODO(rmacnak): Put the finalizer on a queue for the event loop
    // to process.
    survivor->set_finalizer(nil, kNoBarrier);

    Ephemeron* next = survivor->next();
    survivor->set_next(NULL);
    survivor = next;
  }
}

void Heap::AddToWeakList(WeakArray* survivor) {
  DEBUG_ASSERT(survivor->IsOldObject() || InToSpace(survivor));
  survivor->set_next(weak_list_);
  weak_list_ = survivor;
}

void Heap::MournWeakListMarkSweep() {
  WeakArray* survivor = weak_list_;
  weak_list_ = NULL;
  while (survivor != NULL) {
    ASSERT(survivor->IsWeakArray());

    Object** from;
    Object** to;
    survivor->Pointers(&from, &to);
    for (Object** ptr = from; ptr <= to; ptr++) {
      MournWeakPointerMarkSweep(ptr);
      if (survivor->IsOldObject() &&
          (*ptr)->IsNewObject() &&
          !survivor->is_remembered()) {
        UNREACHABLE();
        AddToRememberedSet(survivor);
      }
    }

    WeakArray* next = survivor->next();
    survivor->set_next(NULL);
    survivor = next;
  }
  ASSERT(weak_list_ == NULL);
}

void Heap::MournWeakPointerMarkSweep(Object** ptr) {
  Object* target = *ptr;

  if (IsMarkSweepSurvivor(target)) {
    // Target is still alive.
    return;
  }

  ASSERT(IsMarkSweepSurvivor(object_store()->nil_obj()));
  *ptr = object_store()->nil_obj();
}

void Heap::MournClassTableMarkSweep() {
  for (intptr_t i = kFirstLegalCid; i < class_table_size_; i++) {
    Object** ptr = &class_table_[i];

    Object* target = *ptr;

    if (IsMarkSweepSurvivor(target)) {
      continue;
    }

    *ptr = SmallInteger::New(class_table_free_);
    class_table_free_ = i;
  }
}

void Heap::MournRememberedSet() {
  for (intptr_t i = 0; i < remembered_set_size_; i++) {
    HeapObject* obj = remembered_set_[i];
    if (obj == NULL) {
      continue;
    }
    ASSERT(obj->IsOldObject());
    ASSERT(obj->is_remembered());
    if (!obj->is_marked()) {
      remembered_set_[i] = NULL;
    }
  }
}

bool Heap::BecomeForward(Array* old, Array* neu) {
  if (old->Size() != neu->Size()) {
    return false;
  }

  intptr_t length = old->Size();
  if (TRACE_BECOME) {
    OS::PrintErr("become(%" Pd ")\n", length);
  }

  for (intptr_t i = 0; i < length; i++) {
    Object* forwarder = old->element(i);
    Object* forwardee = neu->element(i);
    if (forwarder->IsImmediateObject() ||
        forwardee->IsImmediateObject()) {
      return false;
    }
  }

  // TODO: It should be safe to let the marker continue if we apply the write
  // barrier during forwarding.
  WaitForMarker();
  if (marking_state_ == kAwaitingFinalization) {
    MarkSweep(kFinalize);
  }

  for (intptr_t i = 0; i < length; i++) {
    HeapObject* forwarder = static_cast<HeapObject*>(old->element(i));
    HeapObject* forwardee = static_cast<HeapObject*>(neu->element(i));

    ASSERT(!forwarder->IsForwardingCorpse());
    ASSERT(!forwardee->IsForwardingCorpse());

    forwardee->set_header_hash(forwarder->header_hash());

    intptr_t heap_size = forwarder->HeapSize();

    HeapObject::Initialize(forwarder->Addr(), kForwardingCorpseCid, heap_size);
    ASSERT(forwarder->IsForwardingCorpse());
    ForwardingCorpse* corpse = static_cast<ForwardingCorpse*>(forwarder);
    if (forwarder->heap_size() == 0) {
      corpse->set_overflow_size(heap_size);
    }
    ASSERT(forwarder->HeapSize() == heap_size);

    corpse->set_target(forwardee);
  }

  ForwardRoots();
  ForwardHeap();  // Using old class table.
  ForwardClassTable();

  ClearCaches();

  return true;
}

void Heap::ForwardRoots() {
  ForwardPointer(reinterpret_cast<Object**>(&object_store_));
  ForwardPointer(reinterpret_cast<Object**>(&current_activation_));

  for (intptr_t i = 0; i < handles_size_; i++) {
    ForwardPointer(handles_[i]);
  }
}

void Heap::ForwardHeap() {
  uword scan = to_.object_start();
  while (scan < top_) {
    HeapObject* obj = HeapObject::FromAddr(scan);
    if (obj->cid() >= kFirstLegalCid) {
      ForwardClass(this, obj);
      Object** from;
      Object** to;
      obj->Pointers(&from, &to);
      for (Object** ptr = from; ptr <= to; ptr++) {
        ForwardPointer(ptr);
      }
    }
    scan += obj->HeapSize();
  }

  remembered_set_size_ = 0;
  for (HeapPage* page = pages_; page != NULL; page = page->next()) {
    uword scan = page->object_start();
    while (scan < page->object_end()) {
      HeapObject* obj = HeapObject::FromAddr(scan);
      if (obj->cid() >= kFirstLegalCid) {
        ForwardClass(this, obj);
        obj->set_is_remembered(false);
        Object** from;
        Object** to;
        obj->Pointers(&from, &to);
        for (Object** ptr = from; ptr <= to; ptr++) {
          ForwardPointer(ptr);
          if ((*ptr)->IsNewObject() && !obj->is_remembered()) {
            AddToRememberedSet(obj);
          }
        }
      }
      scan += obj->HeapSize();
    }
  }
}

void Heap::ForwardClassTable() {
  Object* nil = object_store()->nil_obj();

  for (intptr_t i = kFirstLegalCid; i < class_table_size_; i++) {
    Behavior* old_class = static_cast<Behavior*>(class_table_[i]);
    if (!old_class->IsForwardingCorpse()) {
      continue;
    }

    Behavior* new_class = static_cast<Behavior*>(
        reinterpret_cast<ForwardingCorpse*>(old_class)->target());
    ASSERT(!new_class->IsForwardingCorpse());

    ASSERT(old_class->id()->IsSmallInteger());
    ASSERT(new_class->id()->IsSmallInteger() ||
           new_class->id() == nil);
    if (old_class->id() == new_class->id()) {
      class_table_[i] = new_class;
    } else {
      // new_class is not registered or registered with a different cid.
      // Instances of old_class (if any) have already had their headers updated
      // to the new cid, so release the old_class's cid.
      class_table_[i] = SmallInteger::New(class_table_free_);
      class_table_free_ = i;
    }
  }
}

void Heap::ClearCaches() {
#if LOOKUP_CACHE
  lookup_cache_->Clear();
#endif
#if RECYCLE_ACTIVATIONS
  recycle_list_ = NULL;
#endif
}

intptr_t Heap::AllocateClassId() {
  intptr_t cid;
  if (class_table_free_ != 0) {
    cid = class_table_free_;
    class_table_free_ =
        static_cast<SmallInteger*>(class_table_[cid])->value();
  } else if (class_table_size_ == class_table_capacity_) {
    if (TRACE_GROWTH) {
      OS::PrintErr("Scavenging to free class table entries\n");
    }
    CollectAll(kClassTable);
    if (class_table_free_ != 0) {
      cid = class_table_free_;
      class_table_free_ =
          static_cast<SmallInteger*>(class_table_[cid])->value();
    } else {
      FATAL("Class table growth unimplemented");
      cid = -1;
    }
  } else {
    cid = class_table_size_;
    class_table_size_++;
  }
#if defined(DEBUG)
  class_table_[cid] = reinterpret_cast<Object*>(kUninitializedWord);
#endif
  return cid;
}

intptr_t Heap::CountInstances(intptr_t cid) {
  intptr_t instances = 0;
  uword scan = to_.object_start();
  while (scan < top_) {
    HeapObject* obj = HeapObject::FromAddr(scan);
    if (obj->cid() == cid) {
      instances++;
    }
    scan += obj->HeapSize();
  }
  for (HeapPage* page = pages_; page != NULL; page = page->next()) {
    uword scan = page->object_start();
    while (scan < page->object_end()) {
      HeapObject* obj = HeapObject::FromAddr(scan);
      if (obj->cid() == cid) {
        instances++;
      }
      scan += obj->HeapSize();
    }
  }
  return instances;
}

intptr_t Heap::CollectInstances(intptr_t cid, Array* array) {
  intptr_t instances = 0;
  uword scan = to_.object_start();
  while (scan < top_) {
    HeapObject* obj = HeapObject::FromAddr(scan);
    if (obj->cid() == cid) {
      array->set_element(instances, obj);
      instances++;
    }
    scan += obj->HeapSize();
  }
  for (HeapPage* page = pages_; page != NULL; page = page->next()) {
    uword scan = page->object_start();
    while (scan < page->object_end()) {
      HeapObject* obj = HeapObject::FromAddr(scan);
      if (obj->cid() == cid) {
        array->set_element(instances, obj);
        instances++;
      }
      scan += obj->HeapSize();
    }
  }
  return instances;
}

static void PrintStringError(String* string) {
  const char* cstr = reinterpret_cast<const char*>(string->element_addr(0));
  OS::PrintErr("%.*s", static_cast<int>(string->Size()), cstr);
}

void Heap::PrintStack() {
  Activation* act = activation();
  while (act != object_store()->nil_obj()) {
    OS::PrintErr("  ");

    Activation* home = act;
    while (home->closure() != object_store()->nil_obj()) {
      ASSERT(home->closure()->IsClosure());
      OS::PrintErr("[] in ");
      home = home->closure()->defining_activation();
    }

    AbstractMixin* receiver_mixin = home->receiver()->Klass(this)->mixin();
    String* receiver_mixin_name = receiver_mixin->name();
    if (receiver_mixin_name->IsString()) {
      PrintStringError(receiver_mixin_name);
    } else {
      receiver_mixin_name =
          reinterpret_cast<AbstractMixin*>(receiver_mixin_name)->name();
      ASSERT(receiver_mixin_name->IsString());
      PrintStringError(receiver_mixin_name);
      OS::PrintErr(" class");
    }

    AbstractMixin* method_mixin = home->method()->mixin();
    if (receiver_mixin != method_mixin) {
      String* method_mixin_name = method_mixin->name();
      OS::PrintErr("(");
      if (method_mixin_name->IsString()) {
        PrintStringError(method_mixin_name);
      } else {
        method_mixin_name =
            reinterpret_cast<AbstractMixin*>(method_mixin_name)->name();
        ASSERT(method_mixin_name->IsString());
        PrintStringError(method_mixin_name);
        OS::PrintErr(" class");
      }
      OS::PrintErr(")");
    }

    String* method_name = home->method()->selector();
    OS::PrintErr(" ");
    PrintStringError(method_name);
    OS::PrintErr("\n");

    act = act->sender();
  }
}

uword FreeList::TryAllocate(intptr_t size) {
  intptr_t index = IndexForSize(size);
  while (index < kSizeClasses) {
    if (free_lists_[index] != NULL) {
      FreeListElement* element = Dequeue(index);
      SplitAndRequeue(element, size);
      return reinterpret_cast<uword>(element) - kHeapObjectTag;
    }
    index++;
  }

  FreeListElement* prev = NULL;
  FreeListElement* element = free_lists_[kSizeClasses];
  while (element != NULL) {
    if (element->HeapSize() >= size) {
      if (prev == NULL) {
        free_lists_[kSizeClasses] = element->next();
      } else {
        prev->set_next(element->next());
      }
      SplitAndRequeue(element, size);
      return element->Addr();
    }
    element = element->next();
  }

  return 0;
}

void FreeList::SplitAndRequeue(FreeListElement* element, intptr_t size) {
  ASSERT(size > 0);
  ASSERT((size & kObjectAlignmentMask) == 0);
  ASSERT(element->HeapSize() >= size);
  intptr_t remaining_size = element->HeapSize() - size;
  ASSERT(remaining_size >= 0);
  if (remaining_size > 0) {
    uword remaining_addr = element->Addr() + size;
    EnqueueRange(remaining_addr, remaining_size);
  }
}

FreeListElement* FreeList::Dequeue(intptr_t index) {
  FreeListElement* element = free_lists_[index];
  if (element == NULL) {
    return NULL;
  }
  ASSERT((element->next() == NULL) || element->next()->IsFreeListElement());
  free_lists_[index] = element->next();
  return element;
}

void FreeList::Enqueue(FreeListElement* element) {
  ASSERT(element->IsFreeListElement());
  intptr_t index = IndexForSize(element->HeapSize());
  element->set_next(free_lists_[index]);
  free_lists_[index] = element;
}

void FreeList::EnqueueRange(uword addr, intptr_t size) {
#if defined(DEBUG)
  memset(reinterpret_cast<void*>(addr), kUnallocatedByte, size);
#endif
  ASSERT(size >= kObjectAlignment);
  ASSERT((addr & kObjectAlignmentMask) == 0);
  ASSERT((size & kObjectAlignmentMask) == 0);
  HeapObject* object = HeapObject::Initialize(addr, kFreeListElementCid, size);
  FreeListElement* element = static_cast<FreeListElement*>(object);
  if (element->heap_size() == 0) {
    ASSERT(size > kObjectAlignment);
    element->set_overflow_size(size);
  }
  ASSERT(object->HeapSize() == size);
  ASSERT(element->HeapSize() == size);
  Enqueue(element);
}

}  // namespace psoup
