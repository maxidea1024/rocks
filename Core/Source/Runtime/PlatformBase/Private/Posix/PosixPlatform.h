#pragma once

#include <Base/Types.h>

#include <pthread.h>
#include <sys/stat.h>

// POSIX 공통 backend(Private/Posix)가 OS별로 의미가 다른 부분만 호출하는 hook이다.
// Apple(macOS)은 Private/Apple/ApplePosix.cpp, Android는 Private/Android/AndroidPosix.cpp가
// 구현한다. 공통 코드는 이 선언 밖의 OS 전용 API를 직접 쓰지 않는다.
namespace rocks::platform::detail {

// stat의 수정 시각을 0001-01-01 UTC 기준 100ns tick으로 바꾼다(Apple st_mtimespec, Linux st_mtim).
int64 PosixModifiedTicks(const struct stat& state) noexcept;

// to가 있으면 EEXIST로 실패하는 rename이다. 성공 0, 실패 -1과 errno.
int PosixRenameNoReplace(const char* from, const char* to) noexcept;

// 완료된 쓰기를 저장 장치까지 내린다(Apple F_FULLFSYNC, 그 밖 fsync). 성공 0, 실패 -1과 errno.
int PosixFullSync(int descriptor) noexcept;

// 현재 스레드의 OS 식별 값이다(Apple pthread_threadid_np, Android gettid).
uint64 PosixThreadId() noexcept;

// 조건 변수를 시스템 시각 변경에 영향받지 않는 시한 대기가 가능하도록 초기화한다. 실패는 Fatal.
void PosixInitializeCondition(pthread_cond_t* condition) noexcept;

// timeoutMs 동안 기다린다. pthread 오류 코드(0, ETIMEDOUT 등)를 그대로 반환한다.
int PosixTimedWait(pthread_cond_t* condition, pthread_mutex_t* mutex, uint32 timeoutMs) noexcept;

// decommit한 페이지를 OS에 돌려주는 madvise 값이다(Apple MADV_FREE, Linux 계열 MADV_DONTNEED).
int PosixDecommitAdvice() noexcept;

} // namespace rocks::platform::detail
