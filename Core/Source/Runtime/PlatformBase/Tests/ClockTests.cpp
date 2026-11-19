#include <TestSupport/Test.h>
#include <Platform/Clock.h>

using namespace rocks;
namespace clock_tests_file {

using namespace rocks::platform;

namespace {

// Clock.h가 약속한 tick → ns 관계(초·나머지로 나눈 소수 버림)를 시험 쪽에서 따로 계산한다.
uint64 TicksToNanoseconds(uint64 ticks, uint64 frequency) {
    constexpr uint64 NanosecondsPerSecond = 1000000000;
    return ticks / frequency * NanosecondsPerSecond +
           ticks % frequency * NanosecondsPerSecond / frequency;
}

} // namespace

TEST(PlatformMonotonicTickFrequencyIsPositiveAndStable) {
    const uint64 frequency = MonotonicTickFrequency();
    CHECK(frequency != 0);
    for (int attempt = 0; attempt < 16; ++attempt) {
        CHECK(MonotonicTickFrequency() == frequency);
    }
}

TEST(PlatformMonotonicTicksDoNotDecrease) {
    uint64 previous = MonotonicTicks();
    for (int attempt = 0; attempt < 4096; ++attempt) {
        const uint64 current = MonotonicTicks();
        CHECK(current >= previous);
        previous = current;
    }
}

// MonotonicNanoseconds는 MonotonicTicks와 같은 원천이다. 두 tick 읽기 사이에서 읽은 ns는 두 tick을
// 같은 규칙으로 바꾼 값 사이에 있어야 한다. 원천이 다르면(예: 한쪽만 절전 시간을 포함하거나 기준
// 시점이 다르면) 이 구간을 벗어난다. 시간 문턱 없이 순서만 본다.
TEST(PlatformMonotonicNanosecondsShareTickSource) {
    const uint64 frequency = MonotonicTickFrequency();
    for (int attempt = 0; attempt < 1024; ++attempt) {
        const uint64 before = MonotonicTicks();
        const uint64 nanoseconds = MonotonicNanoseconds();
        const uint64 after = MonotonicTicks();
        CHECK(TicksToNanoseconds(before, frequency) <= nanoseconds);
        CHECK(nanoseconds <= TicksToNanoseconds(after, frequency));
    }
}

// 두 시계로 잰 같은 구간의 길이도 일치해야 한다(구간 끝을 tick 읽기로 감싼다).
TEST(PlatformMonotonicElapsedAgreesBetweenTicksAndNanoseconds) {
    const uint64 frequency = MonotonicTickFrequency();
    const uint64 outerStart = MonotonicTicks();
    const uint64 startNanoseconds = MonotonicNanoseconds();
    const uint64 innerStart = MonotonicTicks();
    uint64 spin = 0;
    for (int index = 0; index < 100000; ++index) {
        spin += static_cast<uint64>(index);
    }
    CHECK(spin != 0);
    const uint64 innerEnd = MonotonicTicks();
    const uint64 endNanoseconds = MonotonicNanoseconds();
    const uint64 outerEnd = MonotonicTicks();
    const uint64 elapsed = endNanoseconds - startNanoseconds;
    CHECK(elapsed <=
          TicksToNanoseconds(outerEnd, frequency) - TicksToNanoseconds(outerStart, frequency));
    CHECK(elapsed >=
          TicksToNanoseconds(innerEnd, frequency) - TicksToNanoseconds(innerStart, frequency));
}
} // namespace clock_tests_file
