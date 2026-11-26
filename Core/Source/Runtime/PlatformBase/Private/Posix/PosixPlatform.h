#pragma once

#include <Base/Types.h>
#include <Platform/Thread.h>

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

// 현재 스레드 스택의 주소 범위 [low, high)를 쓴다(Apple pthread_get_stackaddr_np가 주는 높은 끝과
// pthread_get_stacksize_np, Android pthread_getattr_np·pthread_attr_getstack). 실패는 Fatal.
void PosixCurrentThreadStack(uptr& low, uptr& high) noexcept;

// 단조 시계의 원시 tick이다(Apple mach_absolute_time, Android CLOCK_MONOTONIC의 ns). 실패는 Fatal.
uint64 PosixMonotonicTicks() noexcept;

// PosixMonotonicTicks의 초당 tick 수다(Apple timebase에서 구한 값, Android 10^9). 실패는 Fatal.
uint64 PosixMonotonicTickFrequency() noexcept;

// 조건 변수를 시스템 시각 변경에 영향받지 않는 시한 대기가 가능하도록 초기화한다. 실패는 Fatal.
void PosixInitializeCondition(pthread_cond_t* condition) noexcept;

// timeoutMs 동안 기다린다. pthread 오류 코드(0, ETIMEDOUT 등)를 그대로 반환한다.
int PosixTimedWait(pthread_cond_t* condition, pthread_mutex_t* mutex, uint32 timeoutMs) noexcept;

// 현재 스레드에 이름을 붙인다(Apple은 자기 스레드만 가능, Linux 계열은 15 byte로 자른다). name은
// Thread::Start가 검사한 비어 있지 않은 UTF-8이다. 실패는 무시한다(이름은 힌트).
void PosixSetCurrentThreadName(const char* name) noexcept;

// 현재 스레드의 우선순위를 바꾼다(Apple SCHED_OTHER 우선순위, Android nice). 실패는 무시한다.
void PosixSetCurrentThreadPriority(ThreadPriority priority) noexcept;

// 현재 스레드의 실제 우선순위를 PosixSetCurrentThreadPriority와 같은 눈금의 가장 가까운 단계로
// 읽는다. 조회 실패는 false다.
bool PosixGetCurrentThreadPriority(ThreadPriority& priority) noexcept;

// decommit한 페이지를 OS에 돌려주는 madvise 값이다(Apple MADV_FREE, Linux 계열 MADV_DONTNEED).
int PosixDecommitAdvice() noexcept;

// 접근을 열어 둔 예약(VirtualMemoryAccess::Open)의 commit·decommit이다. 보호는 바꾸지 않는다(UE 5.8
// FPlatformVirtualMemoryBlock과 같다).
// - PosixOpenCommit: Apple은 madvise(MADV_FREE_REUSE)로 다시 쓴다고 알리고(통계용 힌트 — 실패는
//   무시한다), Android는 할 일이 없다.
// - PosixOpenDecommit: 페이지를 OS에 돌려준다(Apple MADV_FREE_REUSABLE, Android MADV_DONTNEED).
//   성공하면 0, 실패하면 -1과 errno다. 실패하면 범위는 그대로다.
void PosixOpenCommit(void* address, usize bytes) noexcept;
int PosixOpenDecommit(void* address, usize bytes) noexcept;

// 이 프로세스가 쓸 수 있는 논리 프로세서 수다(1 이상 — Android는 sched_getaffinity의 affinity 수,
// Apple은 affinity API가 없어 sysconf의 online 수). 알 수 없으면 1이다.
uint32 PosixLogicalProcessorCount() noexcept;

// OpenSystem이 이름 앞에 붙일 시스템 라이브러리 디렉터리다(끝 '/' 포함). 빈 문자열이면 이름 그대로
// dlopen에 넘겨 링커의 시스템 검색 경로에서 찾는다(Apple "/usr/lib/", Android "").
const char* PosixSystemLibraryDirectory() noexcept;

} // namespace rocks::platform::detail
