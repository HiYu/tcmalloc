// Copyright 2019 The TCMalloc Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "tcmalloc/static_vars.h"

#include <stddef.h>

#include <atomic>
#include <new>

#include "absl/base/attributes.h"
#include "absl/base/const_init.h"
#include "absl/base/internal/spinlock.h"
#include "absl/base/macros.h"
#include "tcmalloc/cpu_cache.h"
#include "tcmalloc/deallocation_profiler.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/mincore.h"
#include "tcmalloc/internal/numa.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/pagemap.h"
#include "tcmalloc/sampler.h"
#include "tcmalloc/sizemap.h"
#include "tcmalloc/thread_cache.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

// Cacheline-align our SizeMap and CpuCache.  They both have very hot arrays as
// their first member variables, and aligning them reduces the number of cache
// lines these arrays use.
//
// IF YOU ADD TO THIS LIST, ADD TO STATIC_VAR_SIZE TOO!
// LINT.IfChange(static_vars)
ABSL_CONST_INIT absl::base_internal::SpinLock pageheap_lock(
    absl::kConstInit, absl::base_internal::SCHEDULE_KERNEL_ONLY);
ABSL_CONST_INIT Arena Static::arena_;
ABSL_CONST_INIT SizeMap ABSL_CACHELINE_ALIGNED Static::sizemap_;
TCMALLOC_ATTRIBUTE_NO_DESTROY ABSL_CONST_INIT TransferCacheManager
    Static::transfer_cache_;
ABSL_CONST_INIT ShardedTransferCacheManager
    Static::sharded_transfer_cache_(nullptr, nullptr);
ABSL_CONST_INIT CpuCache ABSL_CACHELINE_ALIGNED Static::cpu_cache_;
ABSL_CONST_INIT SampledAllocationAllocator Static::sampledallocation_allocator_;
ABSL_CONST_INIT PageHeapAllocator<Span> Static::span_allocator_;
ABSL_CONST_INIT PageHeapAllocator<ThreadCache> Static::threadcache_allocator_;
ABSL_CONST_INIT ExplicitlyConstructed<SampledAllocationRecorder>
    Static::sampled_allocation_recorder_;
ABSL_CONST_INIT tcmalloc_internal::StatsCounter Static::sampled_objects_size_;
ABSL_CONST_INIT tcmalloc_internal::StatsCounter
    Static::sampled_internal_fragmentation_;
ABSL_CONST_INIT tcmalloc_internal::StatsCounter Static::total_sampled_count_;
ABSL_CONST_INIT AllocationSampleList Static::allocation_samples;
ABSL_CONST_INIT deallocationz::DeallocationProfilerList
    Static::deallocation_samples;
ABSL_CONST_INIT std::atomic<AllocHandle> Static::sampled_alloc_handle_generator{
    0};
ABSL_CONST_INIT PeakHeapTracker Static::peak_heap_tracker_;
ABSL_CONST_INIT PageHeapAllocator<StackTraceTable::LinkedSample>
    Static::linked_sample_allocator_;
ABSL_CONST_INIT std::atomic<bool> Static::inited_{false};
ABSL_CONST_INIT std::atomic<bool> Static::cpu_cache_active_{false};
ABSL_CONST_INIT Static::PageAllocatorStorage Static::page_allocator_;
ABSL_CONST_INIT PageMap Static::pagemap_;
ABSL_CONST_INIT GuardedPageAllocator Static::guardedpage_allocator_;
ABSL_CONST_INIT StackTraceFilter Static::stacktrace_filter_;
ABSL_CONST_INIT NumaTopology<kNumaPartitions, kNumBaseClasses>
    Static::numa_topology_;
ABSL_CONST_INIT CacheTopology Static::cache_topology_;
// LINT.ThenChange(:static_vars_size)

ABSL_CONST_INIT Static tc_globals;

size_t Static::metadata_bytes() {
  // This is ugly and doesn't nicely account for e.g. alignment losses
  // -- I'd like to put all the above in a struct and take that
  // struct's size.  But we can't due to linking issues.
  //
  // TODO(b/242550501):  Progress on constant initialization guarantees allow
  // state to be consolidated directly into an instance, rather than as a
  // collection of static variables.  Simplify this.
  // LINT.IfChange(static_vars_size)
  const size_t static_var_size =
      sizeof(pageheap_lock) + sizeof(arena_) + sizeof(sizemap_) +
      sizeof(sharded_transfer_cache_) + sizeof(transfer_cache_) +
      sizeof(cpu_cache_) + sizeof(sampledallocation_allocator_) +
      sizeof(span_allocator_) + +sizeof(threadcache_allocator_) +
      sizeof(sampled_allocation_recorder_) + sizeof(linked_sample_allocator_) +
      sizeof(inited_) + sizeof(cpu_cache_active_) + sizeof(page_allocator_) +
      sizeof(pagemap_) + sizeof(sampled_objects_size_) +
      sizeof(sampled_internal_fragmentation_) + sizeof(total_sampled_count_) +
      sizeof(allocation_samples) + sizeof(deallocation_samples) +
      sizeof(sampled_alloc_handle_generator) + sizeof(peak_heap_tracker_) +
      sizeof(guardedpage_allocator_) + sizeof(stacktrace_filter_) +
      sizeof(numa_topology_) + sizeof(cache_topology_);
  // LINT.ThenChange(:static_vars)

  const size_t allocated = arena().stats().bytes_allocated +
                           AddressRegionFactory::InternalBytesAllocated();
  return allocated + static_var_size;
}

size_t Static::pagemap_residence() {
  // Determine residence of the root node of the pagemap.
  size_t total = MInCore::residence(&pagemap_, sizeof(pagemap_));
  return total;
}

int ABSL_ATTRIBUTE_WEAK default_want_legacy_size_classes();

ABSL_ATTRIBUTE_COLD ABSL_ATTRIBUTE_NOINLINE void Static::SlowInitIfNecessary() {
  absl::base_internal::SpinLockHolder h(&pageheap_lock);

  // double-checked locking
  if (!inited_.load(std::memory_order_acquire)) {
    absl::Span<const SizeClassInfo> size_classes;

    if (IsExperimentActive(Experiment::TEST_ONLY_TCMALLOC_POW2_SIZECLASS)) {
      size_classes = kExperimentalPow2SizeClasses;
    } else if (default_want_legacy_size_classes != nullptr &&
               default_want_legacy_size_classes() > 0) {
      // TODO(b/242710633): remove this opt out.
      size_classes = kLegacySizeClasses;
    } else {
      size_classes = kSizeClasses;
    }

    CHECK_CONDITION(sizemap_.Init(size_classes));
    numa_topology_.Init();
    cache_topology_.Init();
    sampledallocation_allocator_.Init(&arena_);
    sampled_allocation_recorder_.Construct(&sampledallocation_allocator_);
    sampled_allocation_recorder().Init();
    peak_heap_tracker_.Init(&arena_);
    span_allocator_.Init(&arena_);
    span_allocator_.New();  // Reduce cache conflicts
    span_allocator_.New();  // Reduce cache conflicts
    linked_sample_allocator_.Init(&arena_);
    // Do a bit of sanitizing: make sure central_cache is aligned properly
    CHECK_CONDITION((sizeof(transfer_cache_) % ABSL_CACHELINE_SIZE) == 0);
    transfer_cache_.Init();
    // The constructor of the sharded transfer cache leaves it in a disabled
    // state.
    sharded_transfer_cache_.Init();
    new (page_allocator_.memory) PageAllocator;
    threadcache_allocator_.Init(&arena_);
    pagemap_.MapRootWithSmallPages();
    guardedpage_allocator_.Init(/*max_alloced_pages=*/64, /*total_pages=*/128);
    inited_.store(true, std::memory_order_release);
  }
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END
