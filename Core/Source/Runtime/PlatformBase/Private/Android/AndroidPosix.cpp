#include "../Posix/PosixPlatform.h"

#include <Platform/Fatal.h>

#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

// Android(Linux 커널, bionic) POSIX hook이다.
namespace rocks::platform::detail {

int64 PosixModifiedTicks(const struct stat& state) noexcept {
    // 0001-01-01부터 1970-01-01까지는 719162일이다. 0001-01-01 이전·9999-12-31 이후(UE FDateTime
    // 범위 밖)는 알 수 없음(0)으로 둔다.
    constexpr int64 Offset = 719162ll * 864000000000ll;
    constexpr int64 MaxTicks = 3155378975999999999ll;
    const int64 seconds = static_cast<int64>(state.st_mtim.tv_sec);
    if (seconds < -Offset / 10000000ll || seconds > (MaxTicks - Offset) / 10000000ll) {
        return 0;
    }
    return seconds * 10000000ll + state.st_mtim.tv_nsec / 100 + Offset;
}

// renameat2(RENAME_NOREPLACE)는 대상이 있으면 원자적으로 실패한다. bionic의 renameat2 wrapper는 API
// 30부터라 system call을 직접 부른다. 커널이나 파일 시스템이 지원하지 않으면(ENOSYS·EINVAL) 검사 후
// rename으로 대신하며, 이 경우 검사와 rename 사이에 생긴 대상은 덮어쓸 수 있다.
int PosixRenameNoReplace(const char* from, const char* to) noexcept {
    const long result = syscall(SYS_renameat2, AT_FDCWD, from, AT_FDCWD, to, RENAME_NOREPLACE);
    if (result == 0) {
        return 0;
    }
    if (errno != ENOSYS && errno != EINVAL) {
        return -1;
    }
    struct stat existing{};
    if (lstat(to, &existing) == 0) {
        errno = EEXIST;
        return -1;
    }
    if (errno != ENOENT) {
        return -1;
    }
    return rename(from, to);
}

int PosixFullSync(int descriptor) noexcept {
    return fsync(descriptor);
}

uint64 PosixThreadId() noexcept {
    return static_cast<uint64>(gettid());
}

// CLOCK_MONOTONIC의 ns를 그대로 tick으로 쓴다(주파수 10^9). 초를 ns로 바꾸기 전에 상한을 검사해
// wrap된 값을 반환하지 않는다.
uint64 PosixMonotonicTicks() noexcept {
    timespec time{};
    if (clock_gettime(CLOCK_MONOTONIC, &time) != 0 || time.tv_sec < 0) {
        Fatal("clock_gettime failed");
    }

    constexpr uint64 NanosecondsPerSecond = 1000000000;
    const uint64 seconds = static_cast<uint64>(time.tv_sec);
    if (seconds >
        (NumericLimits<uint64>::Max() - (NanosecondsPerSecond - 1)) / NanosecondsPerSecond) {
        Fatal("Monotonic clock overflow");
    }

    return seconds * NanosecondsPerSecond + static_cast<uint64>(time.tv_nsec);
}

uint64 PosixMonotonicTickFrequency() noexcept {
    return 1000000000;
}

// CLOCK_MONOTONIC 기준 조건 변수다. 시스템 시각 변경이 시한 대기에 영향을 주지 않는다.
void PosixInitializeCondition(pthread_cond_t* condition) noexcept {
    pthread_condattr_t attributes;
    if (pthread_condattr_init(&attributes) != 0) {
        Fatal("pthread_condattr_init failed");
    }
    if (pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC) != 0) {
        Fatal("pthread_condattr_setclock failed");
    }
    const int result = pthread_cond_init(condition, &attributes);
    if (pthread_condattr_destroy(&attributes) != 0 || result != 0) {
        Fatal("pthread_cond_init failed");
    }
}

int PosixTimedWait(pthread_cond_t* condition, pthread_mutex_t* mutex, uint32 timeoutMs) noexcept {
    timespec deadline{};
    if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0) {
        Fatal("clock_gettime failed");
    }
    deadline.tv_sec += static_cast<time_t>(timeoutMs / 1000);
    deadline.tv_nsec += static_cast<long>(timeoutMs % 1000) * 1000000;
    if (deadline.tv_nsec >= 1000000000) {
        deadline.tv_nsec -= 1000000000;
        ++deadline.tv_sec;
    }
    return pthread_cond_timedwait(condition, mutex, &deadline);
}

// Linux의 MADV_FREE는 4.5 이상 커널에서만 지원되므로 모든 Android 커널에서 동작하는 MADV_DONTNEED를
// 쓴다. PROT_NONE으로 막은 뒤이므로 즉시 회수되어도 의미가 같다.
int PosixDecommitAdvice() noexcept {
    return MADV_DONTNEED;
}

// Android 공개 NDK 라이브러리(libvulkan.so 등)는 링커 namespace가 이름으로 찾아야 한다. 앱
// namespace는 /system 절대 경로 열기를 막을 수 있으므로(Android 7 이후) 이름 그대로 넘긴다.
const char* PosixSystemLibraryDirectory() noexcept {
    return "";
}

} // namespace rocks::platform::detail
