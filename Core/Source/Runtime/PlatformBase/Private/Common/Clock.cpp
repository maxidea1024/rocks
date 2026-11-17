#include <Platform/Clock.h>
#include <Platform/Fatal.h>

namespace rocks::platform {

namespace {

constexpr uint64 NanosecondsPerSecond = 1000000000;

// 나머지 × 10^9가 uint64에 들어가는 가장 큰 주파수다. 실제 원천(QPC 10MHz 안팎, Apple 24MHz·1GHz,
// Linux 1GHz)은 이보다 훨씬 작다.
constexpr uint64 MaxConvertibleFrequency = NumericLimits<uint64>::Max() / NanosecondsPerSecond;

} // namespace

// tick을 초와 나머지로 나눠 정수로 바꾼다. tick × 10^9의 중간 곱 overflow를 피하고, 결과는 정확한
// 소수 버림이라 MonotonicTicks와의 관계(Clock.h)를 오차 없이 지킨다.
uint64 MonotonicNanoseconds() noexcept {
    const uint64 ticks = MonotonicTicks();
    const uint64 frequency = MonotonicTickFrequency();
    if (frequency == 0 || frequency > MaxConvertibleFrequency) {
        Fatal("Monotonic tick frequency is out of range");
    }

    const uint64 seconds = ticks / frequency;
    const uint64 remainder = ticks % frequency;
    if (seconds >
        (NumericLimits<uint64>::Max() - (NanosecondsPerSecond - 1)) / NanosecondsPerSecond) {
        Fatal("Monotonic clock overflow");
    }

    return seconds * NanosecondsPerSecond + remainder * NanosecondsPerSecond / frequency;
}

} // namespace rocks::platform
