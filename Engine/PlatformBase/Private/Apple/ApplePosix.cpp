#include "../Posix/PosixPlatform.h"

#include <Platform/Fatal.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

// Apple(macOS) POSIX hook이다. iOS도 같은 API를 쓰지만 iOS backend는 아직 없다.
// 주의: macOS 실기기에서 아직 빌드·실행하지 않았다.
namespace rocks::platform::detail {

int64 PosixModifiedTicks(const struct stat& state) noexcept {
    // 0001-01-01부터 1970-01-01까지는 719162일이다.
    constexpr int64 Offset = 719162ll * 864000000000ll;
    return static_cast<int64>(state.st_mtimespec.tv_sec) * 10000000ll +
           state.st_mtimespec.tv_nsec / 100 + Offset;
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

} // namespace rocks::platform::detail
