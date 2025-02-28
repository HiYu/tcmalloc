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

#include "tcmalloc/internal/percpu_tcmalloc.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/kernel-page-flags.h>
#include <stddef.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#if defined(__linux__)
#include <linux/param.h>
#else
#include <sys/param.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <new>
#include <optional>
#include <thread>  // NOLINT(build/c++11)
#include <utility>
#include <vector>

#include "benchmark/benchmark.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/algorithm/container.h"
#include "absl/base/call_once.h"
#include "absl/base/internal/sysinfo.h"
#include "absl/base/thread_annotations.h"
#include "absl/container/fixed_array.h"
#include "absl/container/flat_hash_set.h"
#include "absl/random/random.h"
#include "absl/random/seed_sequences.h"
#include "absl/strings/string_view.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "absl/types/span.h"
#include "tcmalloc/internal/affinity.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/page_size.h"
#include "tcmalloc/internal/sysinfo.h"
#include "tcmalloc/internal/util.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/testing/testutil.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace subtle {
namespace percpu {
namespace {

using testing::Each;
using testing::UnorderedElementsAreArray;

constexpr size_t kStressSlabs = 4;
constexpr size_t kStressCapacity = 4;

constexpr size_t kShift = 18;
typedef class TcmallocSlab<kStressSlabs> TcmallocSlab;

TcmallocSlab::Slabs* AllocSlabs(
    absl::FunctionRef<void*(size_t, std::align_val_t)> alloc,
    size_t raw_shift) {
  Shift shift = ToShiftType(raw_shift);
  const size_t slabs_size = GetSlabsAllocSize(shift, NumCPUs());
  return static_cast<TcmallocSlab::Slabs*>(
      alloc(slabs_size, kPhysicalPageAlign));
}

void InitSlab(TcmallocSlab& slab,
              absl::FunctionRef<void*(size_t, std::align_val_t)> alloc,
              absl::FunctionRef<size_t(size_t)> capacity, size_t raw_shift) {
  TcmallocSlab::Slabs* slabs = AllocSlabs(alloc, raw_shift);
  slab.Init(slabs, capacity, ToShiftType(raw_shift));
}

class TcmallocSlabTest : public testing::Test {
 public:
  TcmallocSlabTest() {
// Ignore false-positive warning in GCC. For more information, see:
// https://gcc.gnu.org/bugzilla/show_bug.cgi?id=96003
#pragma GCC diagnostic ignored "-Wnonnull"
    InitSlab(
        slab_,
        [&](size_t size, std::align_val_t align) {
          return ByteCountingMalloc(size, align);
        },
        [](size_t) { return kCapacity; }, kShift);

    for (int i = 0; i < kCapacity; ++i) {
      object_ptrs_[i] = &objects_[i];
    }
  }

  ~TcmallocSlabTest() override { slab_.Destroy(sized_aligned_delete); }

  template <int result>
  static int ExpectOverflow(int cpu, size_t size_class, void* item, void* arg) {
    auto& test_slab = *static_cast<TcmallocSlabTest*>(arg);
    EXPECT_EQ(cpu, test_slab.current_cpu_);
    EXPECT_EQ(size_class, test_slab.current_size_class_);
    EXPECT_FALSE(test_slab.overflow_called_);
    test_slab.overflow_called_ = true;
    return result;
  }

  template <size_t result_object>
  static void* ExpectUnderflow(int cpu, size_t size_class, void* arg) {
    auto& test_slab = *static_cast<TcmallocSlabTest*>(arg);
    EXPECT_EQ(cpu, test_slab.current_cpu_);
    EXPECT_EQ(size_class, test_slab.current_size_class_);
    EXPECT_LT(result_object, kCapacity);
    EXPECT_FALSE(test_slab.underflow_called_);
    test_slab.underflow_called_ = true;
    return &test_slab.objects_[result_object];
  }

  template <int result>
  bool PushExpectOverflow(TcmallocSlab* slab, size_t size_class, void* item) {
    bool res = slab->Push(size_class, item, ExpectOverflow<result>, this);
    EXPECT_TRUE(overflow_called_);
    overflow_called_ = false;
    return res;
  }

  template <size_t result_object>
  void* PopExpectUnderflow(TcmallocSlab* slab, size_t size_class) {
    void* res = slab->Pop(size_class, ExpectUnderflow<result_object>, this);
    EXPECT_TRUE(underflow_called_);
    underflow_called_ = false;
    return res;
  }

  void* ByteCountingMalloc(size_t size, std::align_val_t alignment) {
    void* ptr = ::operator new(size, alignment);
    // Emulate obtaining memory as if we got it from mmap (zero'd).
    memset(ptr, 0, size);
    if (static_cast<size_t>(alignment) >= GetPageSize()) {
      madvise(ptr, size, MADV_DONTNEED);
    }
    metadata_bytes_ += size;
    return ptr;
  }

  TcmallocSlab slab_;

  static constexpr size_t kCapacity = 10;
  char objects_[kCapacity];
  void* object_ptrs_[kCapacity];
  int current_cpu_;
  size_t current_size_class_;
  bool overflow_called_ = false;
  bool underflow_called_ = false;
  size_t metadata_bytes_ = 0;
};

int ExpectNoOverflow(int cpu, size_t size_class, void* item, void* arg) {
  CHECK_CONDITION(false && "overflow is not expected");
  return 0;
}

void* ExpectNoUnderflow(int cpu, size_t size_class, void* arg) {
  CHECK_CONDITION(false && "underflow is not expected");
  return nullptr;
}

TEST_F(TcmallocSlabTest, Metadata) {
  PerCPUMetadataState r = slab_.MetadataMemoryUsage();

  ASSERT_GT(metadata_bytes_, 0);
  EXPECT_EQ(r.virtual_size, metadata_bytes_);
  EXPECT_EQ(r.resident_size, 0);

  if (!IsFast()) {
    GTEST_SKIP() << "Need fast percpu. Skipping.";
    return;
  }

  // Initialize a core.  Verify that the increased RSS is proportional to a
  // core.
  slab_.InitCpu(0, [](size_t size_class) { return kCapacity; });

  r = slab_.MetadataMemoryUsage();
  // We may fault a whole hugepage, so round up the expected per-core share to
  // a full hugepage.
  size_t expected = r.virtual_size / NumCPUs();
  expected = (expected + kHugePageSize - 1) & ~(kHugePageSize - 1);

  // A single core may be less than the full slab for that core, since we do
  // not touch every page within the slab.
  EXPECT_GE(expected, r.resident_size);
  // We expect to have touched at least one page, so resident size should be a
  // non-zero number of bytes.
  EXPECT_GT(r.resident_size, 0);

  // Read stats from the slab.  This will fault additional memory.
  for (int cpu = 0, n = NumCPUs(); cpu < n; ++cpu) {
    // To inhibit optimization, verify the values are sensible.
    for (int size_class = 0; size_class < kStressSlabs; ++size_class) {
      EXPECT_EQ(0, slab_.Length(cpu, size_class));
      EXPECT_EQ(0, slab_.Capacity(cpu, size_class));
    }
  }

  PerCPUMetadataState post_stats = slab_.MetadataMemoryUsage();
  EXPECT_LE(post_stats.resident_size, metadata_bytes_);
  EXPECT_GT(post_stats.resident_size, r.resident_size);
}

TEST_F(TcmallocSlabTest, Unit) {
  if (MallocExtension::PerCpuCachesActive()) {
    // This test unregisters rseq temporarily, as to decrease flakiness.
    GTEST_SKIP() << "per-CPU TCMalloc is incompatible with unregistering rseq";
  }

  if (!IsFast()) {
    GTEST_SKIP() << "Need fast percpu. Skipping.";
    return;
  }

  // Decide if we should expect a push or pop to be the first action on the CPU
  // slab to trigger initialization.
  absl::FixedArray<bool, 0> initialized(NumCPUs(), false);

  for (auto cpu : AllowedCpus()) {
    SCOPED_TRACE(cpu);

    // Temporarily fake being on the given CPU.
    ScopedFakeCpuId fake_cpu_id(cpu);
    current_cpu_ = cpu;

    for (size_t size_class = 0; size_class < kStressSlabs; ++size_class) {
      SCOPED_TRACE(size_class);
      current_size_class_ = size_class;

      // Check new slab state.
      ASSERT_EQ(slab_.Length(cpu, size_class), 0);
      ASSERT_EQ(slab_.Capacity(cpu, size_class), 0);

      struct Policy {
        using pointer_type = void*;
        static void* to_pointer(void* p, size_t size_class) { return p; }
      };
      if (!initialized[cpu]) {
#pragma GCC diagnostic ignored "-Wnonnull"
        void* ptr = slab_.Pop<Policy>(
            size_class,
            +[](int cpu, size_t size_class, void* arg) {
              static_cast<TcmallocSlab*>(arg)->InitCpu(
                  cpu, [](size_t size_class) { return kCapacity; });

              return arg;
            },
            &slab_);

        ASSERT_TRUE(ptr == &slab_);
        initialized[cpu] = true;
      }

      // Test overflow/underflow handlers.
      ASSERT_EQ(PopExpectUnderflow<5>(&slab_, size_class), &objects_[5]);
      ASSERT_FALSE(PushExpectOverflow<-1>(&slab_, size_class, &objects_[0]));
      ASSERT_FALSE(PushExpectOverflow<-2>(&slab_, size_class, &objects_[0]));
      ASSERT_TRUE(PushExpectOverflow<0>(&slab_, size_class, &objects_[0]));

      // Grow capacity to kCapacity / 2.
      const auto max_capacity = [](uint8_t shift) { return kCapacity; };
      ASSERT_EQ(slab_.Grow(cpu, size_class, kCapacity / 2, max_capacity),
                kCapacity / 2);
      ASSERT_EQ(slab_.Length(cpu, size_class), 0);
      ASSERT_EQ(slab_.Capacity(cpu, size_class), kCapacity / 2);
      ASSERT_EQ(PopExpectUnderflow<5>(&slab_, size_class), &objects_[5]);
      ASSERT_TRUE(
          slab_.Push(size_class, &objects_[0], ExpectNoOverflow, nullptr));
      ASSERT_EQ(slab_.Length(cpu, size_class), 1);
      ASSERT_EQ(slab_.Capacity(cpu, size_class), kCapacity / 2);
      ASSERT_EQ(slab_.Pop(size_class, ExpectNoUnderflow, nullptr),
                &objects_[0]);
      ASSERT_EQ(slab_.Length(cpu, size_class), 0);
      for (size_t i = 0; i < kCapacity / 2; ++i) {
        ASSERT_TRUE(
            slab_.Push(size_class, &objects_[i], ExpectNoOverflow, nullptr));
        ASSERT_EQ(slab_.Length(cpu, size_class), i + 1);
      }
      ASSERT_FALSE(PushExpectOverflow<-1>(&slab_, size_class, &objects_[0]));
      for (size_t i = kCapacity / 2; i > 0; --i) {
        ASSERT_EQ(slab_.Pop(size_class, ExpectNoUnderflow, nullptr),
                  &objects_[i - 1]);
        ASSERT_EQ(slab_.Length(cpu, size_class), i - 1);
      }
      // Ensure that Shink don't underflow capacity.
      ASSERT_EQ(slab_.Shrink(cpu, size_class, kCapacity), kCapacity / 2);
      ASSERT_EQ(slab_.Capacity(cpu, size_class), 0);

      // Grow capacity to kCapacity.
      ASSERT_EQ(slab_.Grow(cpu, size_class, kCapacity / 2, max_capacity),
                kCapacity / 2);
      // Ensure that grow don't overflow max capacity.
      ASSERT_EQ(slab_.Grow(cpu, size_class, kCapacity, max_capacity),
                kCapacity / 2);
      ASSERT_EQ(slab_.Capacity(cpu, size_class), kCapacity);
      for (size_t i = 0; i < kCapacity; ++i) {
        ASSERT_TRUE(
            slab_.Push(size_class, &objects_[i], ExpectNoOverflow, nullptr));
        ASSERT_EQ(slab_.Length(cpu, size_class), i + 1);
      }
      ASSERT_FALSE(PushExpectOverflow<-1>(&slab_, size_class, &objects_[0]));
      for (size_t i = kCapacity; i > 0; --i) {
        ASSERT_EQ(slab_.Pop(size_class, ExpectNoUnderflow, nullptr),
                  &objects_[i - 1]);
        ASSERT_EQ(slab_.Length(cpu, size_class), i - 1);
      }

      // Ensure that we can't shrink below length.
      ASSERT_TRUE(
          slab_.Push(size_class, &objects_[0], ExpectNoOverflow, nullptr));
      ASSERT_TRUE(
          slab_.Push(size_class, &objects_[1], ExpectNoOverflow, nullptr));
      ASSERT_EQ(slab_.Shrink(cpu, size_class, kCapacity), kCapacity - 2);
      ASSERT_EQ(slab_.Capacity(cpu, size_class), 2);

      // Test Drain.
      ASSERT_EQ(slab_.Grow(cpu, size_class, 2, max_capacity), 2);

      slab_.Drain(
          cpu, [this, size_class, cpu](int cpu_arg, size_t size_class_arg,
                                       void** batch, size_t size, size_t cap) {
            ASSERT_EQ(cpu, cpu_arg);
            if (size_class == size_class_arg) {
              ASSERT_EQ(size, 2);
              ASSERT_EQ(cap, 4);
              ASSERT_EQ(batch[0], &objects_[0]);
              ASSERT_EQ(batch[1], &objects_[1]);
            } else {
              ASSERT_EQ(size, 0);
              ASSERT_EQ(cap, 0);
            }
          });
      ASSERT_EQ(slab_.Length(cpu, size_class), 0);
      ASSERT_EQ(slab_.Capacity(cpu, size_class), 0);

      // Test PushBatch/PopBatch.
      void* batch[kCapacity + 1];
      for (size_t i = 0; i < kCapacity; ++i) {
        batch[i] = &objects_[i];
      }
      void* slabs_result[kCapacity + 1];
      ASSERT_EQ(slab_.PopBatch(size_class, batch, kCapacity), 0);
      ASSERT_EQ(slab_.PushBatch(size_class, batch, kCapacity), 0);
      ASSERT_EQ(slab_.Grow(cpu, size_class, kCapacity / 2, max_capacity),
                kCapacity / 2);
      ASSERT_EQ(slab_.PopBatch(size_class, batch, kCapacity), 0);
      // Push a batch of size i into empty slab.
      for (size_t i = 1; i < kCapacity; ++i) {
        const size_t expect = std::min(i, kCapacity / 2);
        ASSERT_EQ(slab_.PushBatch(size_class, batch, i), expect);
        ASSERT_EQ(slab_.Length(cpu, size_class), expect);
        for (size_t j = 0; j < expect; ++j) {
          slabs_result[j] = slab_.Pop(size_class, ExpectNoUnderflow, nullptr);
        }
        ASSERT_THAT(
            std::vector<void*>(&slabs_result[0], &slabs_result[expect]),
            UnorderedElementsAreArray(&object_ptrs_[i - expect], expect));
        ASSERT_EQ(PopExpectUnderflow<5>(&slab_, size_class), &objects_[5]);
      }
      // Push a batch of size i into non-empty slab.
      for (size_t i = 1; i < kCapacity / 2; ++i) {
        const size_t expect = std::min(i, kCapacity / 2 - i);
        ASSERT_EQ(slab_.PushBatch(size_class, batch, i), i);
        ASSERT_EQ(slab_.PushBatch(size_class, batch, i), expect);
        ASSERT_EQ(slab_.Length(cpu, size_class), i + expect);
        // Because slabs are LIFO fill in this array from the end.
        for (int j = i + expect - 1; j >= 0; --j) {
          slabs_result[j] = slab_.Pop(size_class, ExpectNoUnderflow, nullptr);
        }
        ASSERT_THAT(std::vector<void*>(&slabs_result[0], &slabs_result[i]),
                    UnorderedElementsAreArray(&object_ptrs_[0], i));
        ASSERT_THAT(
            std::vector<void*>(&slabs_result[i], &slabs_result[i + expect]),
            UnorderedElementsAreArray(&object_ptrs_[i - expect], expect));
        ASSERT_EQ(PopExpectUnderflow<5>(&slab_, size_class), &objects_[5]);
      }
      for (size_t i = 0; i < kCapacity + 1; ++i) {
        batch[i] = nullptr;
      }
      // Pop all elements in a single batch.
      for (size_t i = 1; i < kCapacity / 2; ++i) {
        for (size_t j = 0; j < i; ++j) {
          ASSERT_TRUE(
              slab_.Push(size_class, &objects_[j], ExpectNoOverflow, nullptr));
        }
        ASSERT_EQ(slab_.PopBatch(size_class, batch, i), i);
        ASSERT_EQ(slab_.Length(cpu, size_class), 0);
        ASSERT_EQ(PopExpectUnderflow<5>(&slab_, size_class), &objects_[5]);

        ASSERT_THAT(absl::MakeSpan(&batch[0], i),
                    UnorderedElementsAreArray(&object_ptrs_[0], i));
        ASSERT_THAT(absl::MakeSpan(&batch[i], kCapacity - i), Each(nullptr));
        for (size_t j = 0; j < kCapacity + 1; ++j) {
          batch[j] = nullptr;
        }
      }
      // Pop half of elements in a single batch.
      for (size_t i = 1; i < kCapacity / 2; ++i) {
        for (size_t j = 0; j < i; ++j) {
          ASSERT_TRUE(
              slab_.Push(size_class, &objects_[j], ExpectNoOverflow, nullptr));
        }
        size_t want = std::max<size_t>(1, i / 2);
        ASSERT_EQ(slab_.PopBatch(size_class, batch, want), want);
        ASSERT_EQ(slab_.Length(cpu, size_class), i - want);

        for (size_t j = 0; j < i - want; ++j) {
          ASSERT_EQ(slab_.Pop(size_class, ExpectNoUnderflow, nullptr),
                    static_cast<void*>(&objects_[i - want - j - 1]));
        }

        ASSERT_EQ(PopExpectUnderflow<5>(&slab_, size_class), &objects_[5]);

        ASSERT_GE(i, want);
        ASSERT_THAT(absl::MakeSpan(&batch[0], want),
                    UnorderedElementsAreArray(&object_ptrs_[i - want], want));
        ASSERT_THAT(absl::MakeSpan(&batch[want], kCapacity - want),
                    Each(nullptr));
        for (size_t j = 0; j < kCapacity + 1; ++j) {
          batch[j] = nullptr;
        }
      }
      // Pop 2x elements in a single batch.
      for (size_t i = 1; i < kCapacity / 2; ++i) {
        for (size_t j = 0; j < i; ++j) {
          ASSERT_TRUE(
              slab_.Push(size_class, &objects_[j], ExpectNoOverflow, nullptr));
        }
        ASSERT_EQ(slab_.PopBatch(size_class, batch, i * 2), i);
        ASSERT_EQ(slab_.Length(cpu, size_class), 0);
        ASSERT_EQ(PopExpectUnderflow<5>(&slab_, size_class), &objects_[5]);

        ASSERT_THAT(absl::MakeSpan(&batch[0], i),
                    UnorderedElementsAreArray(&object_ptrs_[0], i));
        ASSERT_THAT(absl::MakeSpan(&batch[i], kCapacity - i), Each(nullptr));
        for (size_t j = 0; j < kCapacity + 1; ++j) {
          batch[j] = nullptr;
        }
      }
      ASSERT_EQ(slab_.Shrink(cpu, size_class, kCapacity / 2), kCapacity / 2);
    }
  }
}

TEST_F(TcmallocSlabTest, SimulatedMadviseFailure) {
  if (!IsFast()) {
    GTEST_SKIP() << "Need fast percpu. Skipping.";
    return;
  }

  // Initialize a core.
  slab_.InitCpu(0, [](size_t size_class) { return kCapacity; });

  auto trigger_resize = [&](size_t shift) {
    // We are deliberately simulating madvise failing, so ignore the return
    // value.
    auto alloc = [&](size_t size, std::align_val_t alignment) {
      return ByteCountingMalloc(size, alignment);
    };
    TcmallocSlab::Slabs* slabs = AllocSlabs(alloc, shift);
    (void)slab_.ResizeSlabs(
        subtle::percpu::ToShiftType(shift), slabs, alloc,
        [](size_t) { return kCapacity / 2; }, [](int cpu) { return cpu == 0; },
        [&](int cpu, size_t size_class, void** batch, size_t size, size_t cap) {
          EXPECT_EQ(size, 0);
        });
  };

  // We need to switch from one size (kShift) to another (kShift - 1) and back.
  trigger_resize(kShift - 1);
  trigger_resize(kShift);
}

size_t get_capacity(size_t size_class) {
  return size_class < kStressSlabs ? kStressCapacity : 0;
}

struct Context {
  TcmallocSlab* slab;
  std::vector<std::vector<void*>>* blocks;
  absl::Span<absl::Mutex> mutexes;
  std::atomic<size_t>* capacity;
  std::atomic<bool>* stop;
  absl::Span<absl::once_flag> init;
  absl::Span<std::atomic<bool>> has_init;
};

void InitCpuOnce(Context& ctx, int cpu) {
  absl::base_internal::LowLevelCallOnce(&ctx.init[cpu], [&]() {
    absl::MutexLock lock(&ctx.mutexes[cpu]);
    ctx.slab->InitCpu(cpu, get_capacity);
    ctx.has_init[cpu].store(true, std::memory_order_relaxed);
  });
}

// TODO(b/213923453): move to an environment style of test, as in
// FakeTransferCacheEnvironment.
void StressThread(size_t thread_id, Context& ctx) {
  EXPECT_TRUE(IsFast());

  std::vector<void*>& block = (*ctx.blocks)[thread_id];

  struct Handler {
    static int Overflow(int cpu, size_t size_class, void* item, void* arg) {
      EXPECT_GE(cpu, 0);
      EXPECT_LT(cpu, NumCPUs());
      EXPECT_LT(size_class, kStressSlabs);
      EXPECT_NE(item, nullptr);
      Context& ctx = *static_cast<Context*>(arg);
      InitCpuOnce(ctx, cpu);
      return -1;
    }

    static void* Underflow(int cpu, size_t size_class, void* arg) {
      EXPECT_GE(cpu, 0);
      EXPECT_LT(cpu, NumCPUs());
      EXPECT_LT(size_class, kStressSlabs);
      Context& ctx = *static_cast<Context*>(arg);
      InitCpuOnce(ctx, cpu);
      // Return arg as a sentinel that we reached underflow.
      return arg;
    }
  };

  const int num_cpus = NumCPUs();
  absl::BitGen rnd(absl::SeedSeq({thread_id}));
  while (!*ctx.stop) {
    size_t size_class = absl::Uniform<int32_t>(rnd, 0, kStressSlabs);
    const int what = absl::Uniform<int32_t>(rnd, 0, 101);
    if (what < 10) {
      if (!block.empty()) {
        if (ctx.slab->Push(size_class, block.back(), &Handler::Overflow,
                           &ctx)) {
          block.pop_back();
        }
      }
    } else if (what < 20) {
      void* item = ctx.slab->Pop(size_class, &Handler::Underflow, &ctx);
      // The test Handler::Underflow returns arg (&ctx) when run.  This is not a
      // valid item and should not be pushed to block, but it allows us to test
      // that we never return a null item which could be indicative of a bug in
      // lazy InitCpu initialization (b/148973091, b/147974701).
      EXPECT_NE(item, nullptr);
      if (item != &ctx) {
        block.push_back(item);
      }
    } else if (what < 30) {
      if (!block.empty()) {
        void* batch[kStressCapacity];
        size_t n = absl::Uniform<int32_t>(
                       rnd, 0, std::min(block.size(), kStressCapacity)) +
                   1;
        for (size_t i = 0; i < n; ++i) {
          batch[i] = block.back();
          block.pop_back();
        }
        size_t pushed = ctx.slab->PushBatch(size_class, batch, n);
        EXPECT_LE(pushed, n);
        for (size_t i = 0; i < n - pushed; ++i) {
          block.push_back(batch[i]);
        }
      }
    } else if (what < 40) {
      void* batch[kStressCapacity];
      size_t n = absl::Uniform<int32_t>(rnd, 0, kStressCapacity) + 1;
      size_t popped = ctx.slab->PopBatch(size_class, batch, n);
      EXPECT_LE(popped, n);
      for (size_t i = 0; i < popped; ++i) {
        block.push_back(batch[i]);
      }
    } else if (what < 50) {
      size_t n = absl::Uniform<int32_t>(rnd, 0, kStressCapacity) + 1;
      for (;;) {
        size_t c = ctx.capacity->load();
        n = std::min(n, c);
        if (n == 0) {
          break;
        }
        if (ctx.capacity->compare_exchange_weak(c, c - n)) {
          break;
        }
      }
      if (n != 0) {
        const int cpu = ctx.slab->GetCurrentVirtualCpuUnsafe();
        // Grow mutates the header array and must be operating on an initialized
        // core.
        InitCpuOnce(ctx, cpu);

        size_t res = ctx.slab->Grow(
            cpu, size_class, n, [](uint8_t shift) { return kStressCapacity; });
        EXPECT_LE(res, n);
        ctx.capacity->fetch_add(n - res);
      }
    } else if (what < 60) {
      const int cpu = ctx.slab->GetCurrentVirtualCpuUnsafe();
      // Shrink mutates the header array and must be operating on an initialized
      // core.
      InitCpuOnce(ctx, cpu);

      size_t n = ctx.slab->Shrink(
          cpu, size_class, absl::Uniform<int32_t>(rnd, 0, kStressCapacity) + 1);
      ctx.capacity->fetch_add(n);
    } else if (what < 70) {
      size_t len = ctx.slab->Length(absl::Uniform<int32_t>(rnd, 0, num_cpus),
                                    size_class);
      EXPECT_LE(len, kStressCapacity);
    } else if (what < 80) {
      size_t cap = ctx.slab->Capacity(absl::Uniform<int32_t>(rnd, 0, num_cpus),
                                      size_class);
      EXPECT_LE(cap, kStressCapacity);
    } else if (what < 90) {
      int cpu = absl::Uniform<int32_t>(rnd, 0, num_cpus);

      // ShrinkOtherCache mutates the header array and must be operating on an
      // initialized core.
      InitCpuOnce(ctx, cpu);

      absl::MutexLock lock(&ctx.mutexes[cpu]);
      size_t to_shrink = absl::Uniform<int32_t>(rnd, 0, kStressCapacity) + 1;
      size_t total_shrunk = ctx.slab->ShrinkOtherCache(
          cpu, size_class, to_shrink,
          [&block](size_t size_class, void** batch, size_t n) {
            EXPECT_LT(size_class, kStressSlabs);
            EXPECT_LE(n, kStressCapacity);
            for (size_t i = 0; i < n; ++i) {
              EXPECT_NE(batch[i], nullptr);
              block.push_back(batch[i]);
            }
          });
      EXPECT_LE(total_shrunk, to_shrink);
      EXPECT_LE(0, total_shrunk);
      ctx.capacity->fetch_add(total_shrunk);
    } else if (what < 100) {
      size_t to_grow = absl::Uniform<int32_t>(rnd, 0, kStressCapacity) + 1;
      for (;;) {
        size_t c = ctx.capacity->load();
        to_grow = std::min(to_grow, c);
        if (to_grow == 0) {
          break;
        }
        if (ctx.capacity->compare_exchange_weak(c, c - to_grow)) {
          break;
        }
      }
      if (to_grow != 0) {
        int cpu = absl::Uniform<int32_t>(rnd, 0, num_cpus);

        // GrowOtherCache mutates the header array and must be operating on an
        // initialized core.
        InitCpuOnce(ctx, cpu);

        absl::MutexLock lock(&ctx.mutexes[cpu]);
        size_t grown = ctx.slab->GrowOtherCache(
            cpu, size_class, to_grow,
            [](uint8_t shift) { return kStressCapacity; });
        EXPECT_LE(grown, to_grow);
        EXPECT_GE(grown, 0);
        ctx.capacity->fetch_add(to_grow - grown);
      }
    } else {
      int cpu = absl::Uniform<int32_t>(rnd, 0, num_cpus);
      // Flip coin on whether to unregister rseq on this thread.
      const bool unregister = absl::Bernoulli(rnd, 0.5);

      // Drain mutates the header array and must be operating on an initialized
      // core.
      InitCpuOnce(ctx, cpu);

      {
        absl::MutexLock lock(&ctx.mutexes[cpu]);
        std::optional<ScopedUnregisterRseq> scoped_rseq;
        if (unregister) {
          scoped_rseq.emplace();
          ASSERT(!IsFastNoInit());
        }

        ctx.slab->Drain(
            cpu, [&block, &ctx, cpu](int cpu_arg, size_t size_class,
                                     void** batch, size_t size, size_t cap) {
              EXPECT_EQ(cpu, cpu_arg);
              EXPECT_LT(size_class, kStressSlabs);
              EXPECT_LE(size, kStressCapacity);
              EXPECT_LE(cap, kStressCapacity);
              for (size_t i = 0; i < size; ++i) {
                EXPECT_NE(batch[i], nullptr);
                block.push_back(batch[i]);
              }
              ctx.capacity->fetch_add(cap);
            });
      }

      // Verify we re-registered with rseq as required.
      ASSERT(IsFastNoInit());
    }
  }
}

void* allocator(size_t bytes, std::align_val_t alignment) {
  void* ptr = ::operator new(bytes, alignment);
  memset(ptr, 0, bytes);
  return ptr;
}

constexpr size_t kResizeInitialShift = 14;
constexpr size_t kResizeMaxShift = 18;

void ResizeSlabsThread(Context& ctx, TcmallocSlab::DrainHandler drain_handler,
                       absl::Span<std::pair<void*, size_t>> old_slabs_span)
    ABSL_NO_THREAD_SAFETY_ANALYSIS {
  absl::BitGen rnd;
  const size_t num_cpus = NumCPUs();
  size_t shift = kResizeInitialShift;
  size_t old_slabs_idx = 0;
  for (int i = 0; i < 10; ++i) {
    if (shift == kResizeInitialShift) {
      ++shift;
    } else if (shift == kResizeMaxShift) {
      --shift;
    } else {
      const bool grow = absl::Bernoulli(rnd, 0.5);
      if (grow) {
        ++shift;
      } else {
        --shift;
      }
    }
    for (size_t cpu = 0; cpu < num_cpus; ++cpu) ctx.mutexes[cpu].Lock();
    TcmallocSlab::Slabs* slabs = AllocSlabs(allocator, shift);
    const auto [old_slabs, old_slabs_size] = ctx.slab->ResizeSlabs(
        ToShiftType(shift), slabs, allocator, get_capacity,
        [&](size_t cpu) {
          return ctx.has_init[cpu].load(std::memory_order_relaxed);
        },
        drain_handler);
    for (size_t cpu = 0; cpu < num_cpus; ++cpu) ctx.mutexes[cpu].Unlock();
    ASSERT_NE(old_slabs, nullptr);
    // We sometimes don't madvise away the old slabs in order to simulate
    // madvise failing.
    const bool simulate_madvise_failure = absl::Bernoulli(rnd, 0.1);
    if (!simulate_madvise_failure) {
      // It's important that we do this here in order to uncover any potential
      // correctness issues due to madvising away the old slabs.
      // TODO(b/214241843): we should be able to just do one MADV_DONTNEED once
      // the kernel enables huge zero pages.
      madvise(old_slabs, old_slabs_size, MADV_NOHUGEPAGE);
      madvise(old_slabs, old_slabs_size, MADV_DONTNEED);

      // Verify that old_slabs is now non-resident.
      const int fd = signal_safe_open("/proc/self/pageflags", O_RDONLY);
      if (fd < 0) continue;

      // /proc/self/pageflags is an array. Each entry is a bitvector of size 64.
      // To index the array, divide the virtual address by the pagesize. The
      // 64b word has bit fields set.
      const uintptr_t start_addr = reinterpret_cast<uintptr_t>(old_slabs);
      constexpr size_t kPhysicalPageSize = EXEC_PAGESIZE;
      for (uintptr_t addr = start_addr; addr < start_addr + old_slabs_size;
           addr += kPhysicalPageSize) {
        ASSERT_EQ(addr % kPhysicalPageSize, 0);
        // Offset in /proc/self/pageflags.
        const off64_t offset = addr / kPhysicalPageSize * 8;
        uint64_t entry = 0;
// Ignore false-positive warning in GCC.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wattribute-warning"
#endif
        const int64_t bytes_read = pread(fd, &entry, sizeof(entry), offset);
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
        ASSERT_EQ(bytes_read, sizeof(entry));
        constexpr uint64_t kExpectedBits =
            (uint64_t{1} << KPF_ZERO_PAGE) | (uint64_t{1} << KPF_NOPAGE);
        ASSERT_NE(entry & kExpectedBits, 0)
            << entry << " " << addr << " " << start_addr;
      }
      signal_safe_close(fd);
    }

    // Delete the old slab from 100 iterations ago.
    if (old_slabs_span[old_slabs_idx].first != nullptr) {
      sized_aligned_delete(old_slabs_span[old_slabs_idx].first,
                           old_slabs_span[old_slabs_idx].second,
                           std::align_val_t{EXEC_PAGESIZE});
    }
    old_slabs_span[old_slabs_idx] = {old_slabs, old_slabs_size};
    if (++old_slabs_idx == old_slabs_span.size()) old_slabs_idx = 0;
  }
}

class StressThreadTest : public testing::TestWithParam<bool> {
 protected:
  bool Resize() const { return GetParam(); }
};

TEST_P(StressThreadTest, Stress) {
  // The test creates 2 * NumCPUs() threads each executing all possible
  // operations on TcmallocSlab. Depending on the test param, we may grow the
  // slabs a few times while stress threads are running. After that we verify
  // that no objects lost/duplicated and that total capacity is preserved.

  if (!IsFast()) {
    GTEST_SKIP() << "Need fast percpu. Skipping.";
    return;
  }

  TcmallocSlab slab;
  size_t shift = Resize() ? kResizeInitialShift : kShift;
  InitSlab(slab, allocator, get_capacity, shift);
  std::vector<std::thread> threads;
  const size_t num_cpus = NumCPUs();
  const size_t n_stress_threads = 2 * num_cpus;
  const size_t n_threads = n_stress_threads + Resize();

  // once_flag's protect InitCpu on a CPU.
  std::vector<absl::once_flag> init(num_cpus);
  // Tracks whether init has occurred on a CPU for use in ResizeSlabs.
  std::vector<std::atomic<bool>> has_init(num_cpus);

  // Mutexes protect Drain operation on a CPU.
  std::vector<absl::Mutex> mutexes(num_cpus);
  // Give each thread an initial set of local objects.
  std::vector<std::vector<void*>> blocks(n_stress_threads);
  for (size_t i = 0; i < blocks.size(); ++i) {
    for (size_t j = 0; j < kStressCapacity; ++j) {
      blocks[i].push_back(reinterpret_cast<void*>(i * kStressCapacity + j + 1));
    }
  }
  std::atomic<bool> stop(false);
  // Total capacity shared between all size classes and all CPUs.
  const size_t kTotalCapacity = blocks.size() * kStressCapacity * 3 / 4;
  std::atomic<size_t> capacity(kTotalCapacity);
  Context ctx = {&slab,
                 &blocks,
                 absl::MakeSpan(mutexes),
                 &capacity,
                 &stop,
                 absl::MakeSpan(init),
                 absl::MakeSpan(has_init)};
  // Create threads and let them work for 5 seconds while we may or not also be
  // resizing the slab.
  threads.reserve(n_threads);
  for (size_t t = 0; t < n_stress_threads; ++t) {
    threads.push_back(std::thread(StressThread, t, std::ref(ctx)));
  }
  // Collect objects and capacity from all slabs in Drain in ResizeSlabs.
  absl::flat_hash_set<void*> objects;
  const auto drain_handler = [&objects, &ctx](int cpu, size_t size_class,
                                              void** batch, size_t size,
                                              size_t cap) {
    for (size_t i = 0; i < size; ++i) {
      objects.insert(batch[i]);
    }
    ctx.capacity->fetch_add(cap);
  };
  // Keep track of old slabs so we can free the memory. We technically could
  // have a sleeping StressThread access any of the old slabs, but it's very
  // inefficient to keep all the old slabs around so we just keep 100.
  std::array<std::pair<void*, size_t>, 100> old_slabs_arr{};
  if (Resize()) {
    threads.push_back(std::thread(ResizeSlabsThread, std::ref(ctx),
                                  std::ref(drain_handler),
                                  absl::MakeSpan(old_slabs_arr)));
  }
  absl::SleepFor(absl::Seconds(5));
  stop = true;
  for (auto& t : threads) {
    t.join();
  }
  for (int cpu = 0; cpu < num_cpus; ++cpu) {
    slab.Drain(cpu, drain_handler);
    for (size_t size_class = 0; size_class < kStressSlabs; ++size_class) {
      EXPECT_EQ(slab.Length(cpu, size_class), 0);
      EXPECT_EQ(slab.Capacity(cpu, size_class), 0);
    }
  }
  for (const auto& b : blocks) {
    for (auto o : b) {
      objects.insert(o);
    }
  }
  EXPECT_EQ(objects.size(), blocks.size() * kStressCapacity);
  EXPECT_EQ(capacity.load(), kTotalCapacity);
  void* deleted_slabs = slab.Destroy(sized_aligned_delete);
  for (const auto& [old_slabs, old_slabs_size] : old_slabs_arr) {
    if (old_slabs == nullptr || old_slabs == deleted_slabs) continue;
    sized_aligned_delete(old_slabs, old_slabs_size,
                         std::align_val_t{EXEC_PAGESIZE});
  }
}
INSTANTIATE_TEST_SUITE_P(GrowOrNot, StressThreadTest, ::testing::Bool());

TEST(TcmallocSlab, SMP) {
  // For the other tests here to be meaningful, we need multiple cores.
  ASSERT_GT(NumCPUs(), 1);
}

#if ABSL_INTERNAL_HAVE_ELF_SYMBOLIZE
int FilterElfHeader(struct dl_phdr_info* info, size_t size, void* data) {
  *reinterpret_cast<uintptr_t*>(data) =
      reinterpret_cast<uintptr_t>(info->dlpi_addr);
  // No further iteration wanted.
  return 1;
}
#endif

TEST(TcmallocSlab, CriticalSectionMetadata) {
// We cannot inhibit --gc-sections, except on GCC or Clang 9-or-newer.
#if defined(__clang_major__) && __clang_major__ < 9
  GTEST_SKIP() << "--gc-sections cannot be inhibited on this compiler.";
#endif

#if !TCMALLOC_INTERNAL_PERCPU_USE_RSEQ
  GTEST_SKIP() << "rseq is not enabled in this build.";
#endif

  // We expect that restartable sequence critical sections (rseq_cs) are in the
  // __rseq_cs section (by convention, not hard requirement).  Additionally, for
  // each entry in that section, there should be a pointer to it in
  // __rseq_cs_ptr_array.
#if ABSL_INTERNAL_HAVE_ELF_SYMBOLIZE
  uintptr_t relocation = 0;
  dl_iterate_phdr(FilterElfHeader, &relocation);

  int fd = tcmalloc_internal::signal_safe_open("/proc/self/exe", O_RDONLY);
  ASSERT_NE(fd, -1);

  const kernel_rseq_cs* cs_start = nullptr;
  const kernel_rseq_cs* cs_end = nullptr;

  const kernel_rseq_cs** cs_array_start = nullptr;
  const kernel_rseq_cs** cs_array_end = nullptr;

  absl::debugging_internal::ForEachSection(
      fd, [&](const absl::string_view name, const ElfW(Shdr) & hdr) {
        uintptr_t start = relocation + reinterpret_cast<uintptr_t>(hdr.sh_addr);
        uintptr_t end =
            relocation + reinterpret_cast<uintptr_t>(hdr.sh_addr + hdr.sh_size);

        if (name == "__rseq_cs") {
          EXPECT_EQ(cs_start, nullptr);
          EXPECT_EQ(start % alignof(kernel_rseq_cs), 0);
          EXPECT_EQ(end % alignof(kernel_rseq_cs), 0);
          EXPECT_LT(start, end) << "__rseq_cs must not be empty";

          cs_start = reinterpret_cast<const kernel_rseq_cs*>(start);
          cs_end = reinterpret_cast<const kernel_rseq_cs*>(end);
        } else if (name == "__rseq_cs_ptr_array") {
          EXPECT_EQ(cs_array_start, nullptr);
          EXPECT_EQ(start % alignof(kernel_rseq_cs*), 0);
          EXPECT_EQ(end % alignof(kernel_rseq_cs*), 0);
          EXPECT_LT(start, end) << "__rseq_cs_ptr_array must not be empty";

          cs_array_start = reinterpret_cast<const kernel_rseq_cs**>(start);
          cs_array_end = reinterpret_cast<const kernel_rseq_cs**>(end);
        }

        return true;
      });

  close(fd);

  // The length of the array in multiples of rseq_cs should be the same as the
  // length of the array of pointers.
  ASSERT_EQ(cs_end - cs_start, cs_array_end - cs_array_start);

  // The array should not be empty.
  ASSERT_NE(cs_start, nullptr);

  absl::flat_hash_set<const kernel_rseq_cs*> cs_pointers;
  for (auto* ptr = cs_start; ptr != cs_end; ++ptr) {
    cs_pointers.insert(ptr);
  }

  absl::flat_hash_set<const kernel_rseq_cs*> cs_array_pointers;
  for (auto** ptr = cs_array_start; ptr != cs_array_end; ++ptr) {
    // __rseq_cs_ptr_array should have no duplicates.
    EXPECT_TRUE(cs_array_pointers.insert(*ptr).second);
  }

  EXPECT_THAT(cs_pointers, ::testing::ContainerEq(cs_array_pointers));
#endif
}

void BM_PushPop(benchmark::State& state) {
  CHECK_CONDITION(IsFast());
  constexpr int kCpu = 0;
  constexpr size_t kSizeClass = 0;
  // Fake being on the given CPU. This allows Grow to succeed for
  // kCpu/kSizeClass, and then we Push/Pop repeatedly on kCpu/kSizeClass.
  // Note that no other thread has access to `slab` so we don't need to worry
  // about races.
  ScopedFakeCpuId fake_cpu_id(kCpu);
  constexpr int kBatchSize = 32;
  TcmallocSlab slab;

#pragma GCC diagnostic ignored "-Wnonnull"
  const auto get_capacity = [](size_t size_class) -> size_t {
    return kBatchSize;
  };
  InitSlab(slab, allocator, get_capacity, kShift);
  for (int cpu = 0, n = NumCPUs(); cpu < n; ++cpu) {
    slab.InitCpu(cpu, get_capacity);
  }

  CHECK_CONDITION(slab.Grow(kCpu, kSizeClass, kBatchSize, [](uint8_t shift) {
    return kBatchSize;
  }) == kBatchSize);
  void* batch[kBatchSize];
  for (int i = 0; i < kBatchSize; i++) {
    batch[i] = &batch[i];
  }
  for (auto _ : state) {
    for (size_t x = 0; x < kBatchSize; x++) {
      CHECK_CONDITION(
          slab.Push(kSizeClass, batch[x], ExpectNoOverflow, nullptr));
    }
    for (size_t x = 0; x < kBatchSize; x++) {
      CHECK_CONDITION(slab.Pop(kSizeClass, ExpectNoUnderflow, nullptr) ==
                      batch[kBatchSize - x - 1]);
    }
  }
}
BENCHMARK(BM_PushPop);

void BM_PushPopBatch(benchmark::State& state) {
  CHECK_CONDITION(IsFast());
  constexpr int kCpu = 0;
  constexpr size_t kSizeClass = 0;
  // Fake being on the given CPU. This allows Grow to succeed for
  // kCpu/kSizeClass, and then we Push/PopBatch repeatedly on kCpu/kSizeClass.
  // Note that no other thread has access to `slab` so we don't need to worry
  // about races.
  constexpr int kBatchSize = 32;
  TcmallocSlab slab;
  const auto get_capacity = [](size_t size_class) -> size_t {
    return kBatchSize;
  };
  InitSlab(slab, allocator, get_capacity, kShift);
  for (int cpu = 0, n = NumCPUs(); cpu < n; ++cpu) {
    slab.InitCpu(cpu, get_capacity);
  }
  CHECK_CONDITION(slab.Grow(kCpu, kSizeClass, kBatchSize, [](uint8_t shift) {
    return kBatchSize;
  }) == kBatchSize);
  void* batch[kBatchSize];
  for (int i = 0; i < kBatchSize; i++) {
    batch[i] = &batch[i];
  }
  for (auto _ : state) {
    CHECK_CONDITION(slab.PushBatch(kSizeClass, batch, kBatchSize) ==
                    kBatchSize);
    CHECK_CONDITION(slab.PopBatch(kSizeClass, batch, kBatchSize) == kBatchSize);
  }
}
BENCHMARK(BM_PushPopBatch);

}  // namespace
}  // namespace percpu
}  // namespace subtle
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
