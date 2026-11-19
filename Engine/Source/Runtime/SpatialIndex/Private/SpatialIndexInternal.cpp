#include "SpatialIndexInternal.h"

#include <Platform/Atomics.h>

namespace rocks::spatial {

uint64 NextMutationEpoch() noexcept {
    static uint64 counter = 0;
    return platform::AtomicFetchAdd<platform::MemoryOrder::Relaxed>(&counter, uint64{1}) + 1;
}

} // namespace rocks::spatial
