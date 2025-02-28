// Copyright 2021 The TCMalloc Authors
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

#ifndef TCMALLOC_INTERNAL_CACHE_TOPOLOGY_H_
#define TCMALLOC_INTERNAL_CACHE_TOPOLOGY_H_

#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/util.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

// Build a mapping from cpuid to the index of the L3 cache used by that cpu.
// Returns the number of caches detected.
int BuildCpuToL3CacheMap(uint8_t l3_cache_index[CPU_SETSIZE]);

// Helper function exposed to permit testing it.
int BuildCpuToL3CacheMap_FindFirstNumberInBuf(absl::string_view current);

class CacheTopology {
 public:
  constexpr CacheTopology() = default;

  void Init() { shard_count_ = BuildCpuToL3CacheMap(l3_cache_index_); }

  unsigned shard_count() const { return shard_count_; }

  unsigned GetL3FromCpuId(int cpu) const {
    ASSERT(cpu >= 0);
    ASSERT(cpu < ABSL_ARRAYSIZE(l3_cache_index_));
    return l3_cache_index_[cpu];
  }

 private:
  unsigned shard_count_ = 0;
  // Mapping from cpu to the L3 cache used.
  uint8_t l3_cache_index_[CPU_SETSIZE] = {0};
};

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_INTERNAL_CACHE_TOPOLOGY_H_
