#include "../Posix/PosixPlatform.h"

#include <Platform/Fatal.h>

#include <errno.h>
#include <fcntl.h>
#include <mach/mach_time.h>
#include <sched.h>
#include <stdio.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

// Apple(macOS) POSIX hook이다. iOS도 같은 API를 쓰지만 iOS backend는 아직 없다.
// 주의: macOS 실기기에서 아직 빌드·실행하지 않았다.
namespace rocks::platform::detail {

int64 PosixModifiedTicks(const struct stat& state) noexcept {
    // 0001-01-01부터 1970-01-01까지는 719162일이다. 0001-01-01 이전·9999-12-31 이후(UE FDateTime
    // 범위 밖)는 알 수 없음(0)으로 둔다.
    constexpr int64 Offset = 719162ll * 864000000000ll;
    constexpr int64 MaxTicks = 3155378975999999999ll;
    const int64 seconds = static_cast<int64>(state.st_mtimespec.tv_sec);
    if (seconds < -Offset / 10000000ll || seconds > (MaxTicks - Offset) / 10000000ll) {
        return 0;
    }
    return seconds * 10000000ll + state.st_mtimespec.tv_nsec / 100 + Offset;
}

int PosixRenameNoReplace(const char* from, const char* to) noexcept {
    return renamex_np(from, to, RENAME_EXCL);
}

// F_FULLFSYNC는 장치 캐시까지 내린다. 지원하지 않는 파일 시스템이면 fsync로 대신한다.
int PosixFullSync(int descriptor) noexcept {
    if (fcntl(descriptor, F_FULLFSYNC) == 0) {
        return 0;
    }
    if (errno != ENOTSUP && errno != EINVAL) {
        return -1;
    }
    return fsync(descriptor);
}

uint64 PosixThreadId() noexcept {
    uint64_t identifier = 0;
    if (pthread_threadid_np(nullptr, &identifier) != 0) {
        Fatal("pthread_threadid_np failed");
    }
    return static_cast<uint64>(identifier);
}

// pthread_get_stackaddr_np는 스택의 높은 끝(시작 주소)을 준다. 스택은 아래로 자란다.
void PosixCurrentThreadStack(uptr& low, uptr& high) noexcept {
    const pthread_t self = pthread_self();
    const auto top = reinterpret_cast<uptr>(pthread_get_stackaddr_np(self));
    const auto size = static_cast<uptr>(pthread_get_stacksize_np(self));
    if (top == 0 || size == 0 || size > top) {
        Fatal("pthread_get_stackaddr_np returned an invalid stack");
    }

    high = top;
    low = top - size;
}

// mach_absolute_time은 절전 중 멈춘다(UE FPlatformTime::Cycles64와 같은 원천). 절전을 포함하는
// mach_continuous_time은 쓰지 않는다.
uint64 PosixMonotonicTicks() noexcept {
    return static_cast<uint64>(mach_absolute_time());
}

// tick × numer / denom = ns이므로 초당 tick은 10^9 × denom / numer다. Apple Silicon(125/3 →
// 24MHz)과 Intel(1/1 → 1GHz)은 나누어떨어진다. 나누어떨어지지 않는 timebase는 반올림한다.
uint64 PosixMonotonicTickFrequency() noexcept {
    mach_timebase_info_data_t timebase{};
    if (mach_timebase_info(&timebase) != KERN_SUCCESS || timebase.numer == 0 ||
        timebase.denom == 0) {
        Fatal("mach_timebase_info failed");
    }

    constexpr uint64 NanosecondsPerSecond = 1000000000;
    const uint64 scaled = NanosecondsPerSecond * static_cast<uint64>(timebase.denom);
    const uint64 frequency =
        (scaled + static_cast<uint64>(timebase.numer) / 2) / static_cast<uint64>(timebase.numer);
    if (frequency == 0) {
        Fatal("mach_timebase_info frequency is out of range");
    }

    return frequency;
}

void PosixInitializeCondition(pthread_cond_t* condition) noexcept {
    if (pthread_cond_init(condition, nullptr) != 0) {
        Fatal("pthread_cond_init failed");
    }
}

// Apple의 relative variant는 CLOCK_REALTIME 변경의 영향을 받지 않는다.
int PosixTimedWait(pthread_cond_t* condition, pthread_mutex_t* mutex, uint32 timeoutMs) noexcept {
    const timespec relative{static_cast<time_t>(timeoutMs / 1000),
                            static_cast<long>(timeoutMs % 1000) * 1000000};
    return pthread_cond_timedwait_relative_np(condition, mutex, &relative);
}

int PosixDecommitAdvice() noexcept {
    return MADV_FREE;
}

// Apple의 pthread_setname_np는 현재 스레드에만 붙이며 한도는 63 byte다(MaxThreadNameBytes와 같다).
void PosixSetCurrentThreadName(const char* name) noexcept {
    static_cast<void>(pthread_setname_np(name));
}

// SCHED_OTHER의 최소·최대 사이를 다섯 단계로 나눈다(UE Apple 스레드도 SCHED_OTHER 대역 안의 값을
// 쓴다). 보통은 대역 가운데((최소 + 최대) / 2)로 고정한다. 부모에서 물려받은 값을 기준으로 삼으면
// 같은 단계가 만든 스레드마다 달라지기 때문이다. 정책은 늘 SCHED_OTHER로 둔다(실시간 정책은 쓰지
// 않는다).
void PosixSetCurrentThreadPriority(ThreadPriority priority) noexcept {
    const int minimum = sched_get_priority_min(SCHED_OTHER);
    const int maximum = sched_get_priority_max(SCHED_OTHER);
    if (minimum < 0 || maximum < minimum) {
        return;
    }

    const int normal = (minimum + maximum) / 2;
    int value = normal;
    switch (priority) {
    case ThreadPriority::Lowest:
        value = minimum;
        break;
    case ThreadPriority::BelowNormal:
        value = (minimum + normal) / 2;
        break;
    case ThreadPriority::Normal:
        return;
    case ThreadPriority::AboveNormal:
        value = (normal + maximum) / 2;
        break;
    case ThreadPriority::Highest:
        value = maximum;
        break;
    }

    sched_param parameters{};
    parameters.sched_priority = value;
    static_cast<void>(pthread_setschedparam(pthread_self(), SCHED_OTHER, &parameters));
}

// macOS 시스템 라이브러리는 dyld 공유 캐시에 있어 파일이 없어도 /usr/lib 절대 경로로 열린다. 이름만
// 넘기면 dyld가 DYLD_LIBRARY_PATH·현재 디렉터리 같은 대체 경로도 찾으므로 절대 경로로 연다.
const char* PosixSystemLibraryDirectory() noexcept {
    return "/usr/lib/";
}

} // namespace rocks::platform::detail
