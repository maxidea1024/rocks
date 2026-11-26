#include <Platform/Atomics.h>
#include <Platform/Thread.h>

namespace rocks::platform::detail {

void YieldAfterSpin() noexcept {
    YieldThread();
}

} // namespace rocks::platform::detail
