#pragma once

#include <Platform/Clock.h>
#include <Platform/Sync.h>

namespace rocks::platform::detail {

// spurious wake마다 원래 timeout을 재시작하지 않는다. 절대 wall clock 대신 elapsed monotonic을
// 쓴다.
class WaitDeadline {
public:
    explicit WaitDeadline(uint32 timeoutMs) noexcept
        : started_(MonotonicNanoseconds()), timeoutMs_(timeoutMs) {
    }

    uint32 RemainingMilliseconds() const noexcept {
        if (timeoutMs_ == InfiniteTimeout) {
            return InfiniteTimeout;
        }
        constexpr uint64 nanosecondsPerMillisecond = 1000000;
        const uint64 duration = static_cast<uint64>(timeoutMs_) * nanosecondsPerMillisecond;
        const uint64 elapsed = MonotonicNanoseconds() - started_;
        if (elapsed >= duration) {
            return 0;
        }
        const uint64 remaining = duration - elapsed;
        // 양의 1ms 미만 잔량을 0으로 절삭하여 조기 timeout을 만들지 않는다.
        return static_cast<uint32>(remaining / nanosecondsPerMillisecond +
                                   (remaining % nanosecondsPerMillisecond != 0));
    }

private:
    uint64 started_;
    uint32 timeoutMs_;
};

} // namespace rocks::platform::detail
