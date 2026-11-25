#pragma once

#include <Base/Compiler.h>
#include <Base/Types.h>

namespace rocks::platform {
class WaiterSignal;
} // namespace rocks::platform

namespace rocks::platform::detail {

// 동기화 시한 시험의 seam이다(Docs/Design/Synchronization 14.2절). 시험은 단조 시계와
// WaiterSignal의 시한 대기를 함께 바꿔 실제 시간 없이 "시한 전 깨움"과 "시한 뒤 시간 초과"를
// 만든다. 설치는 SyncTestAccess.h로만 한다. Release에는 seam이 없다.
using SyncClockFunction = uint64 (*)() noexcept;
using SyncTimedWaitFunction = bool (*)(WaiterSignal& signal, uint32 timeoutMs) noexcept;

// 동기화 시한 계산이 쓰는 단조 시계(ns)다. 시험 시계가 있으면 그것, 없으면 MonotonicNanoseconds다.
ROCKS_PLATFORMBASE_API uint64 SyncMonotonicNanoseconds() noexcept;

// 시험의 ParkingLot 훅이다(14.2절, D30). onParked는 enqueue와 beforeWait가 끝난 뒤 잠들기 직전,
// onUnparked는 깬 직후다. 그래서 onParked가 불린 뒤의 깨우기는 반드시 그 대기자를 찾는다. 훅은
// WaiterSignal 기반 gate만 쓴다(ParkingLot 계열은 금지).
using SyncParkFunction = void (*)(const void* address) noexcept;
// 단계 지점 훅이다(14.3절, ROCKS_SYNC_STEP). 스레드마다 설치한다.
using SyncStepFunction = void (*)(const char* name) noexcept;

#if !ROCKS_BUILD_RELEASE
ROCKS_PLATFORMBASE_API void SetSyncTestParkHooks(SyncParkFunction onParked,
                                                 SyncParkFunction onUnparked) noexcept;
SyncParkFunction SyncTestOnParked() noexcept;
SyncParkFunction SyncTestOnUnparked() noexcept;
// 이 스레드의 단계 훅을 바꾼다(nullptr이면 끈다).
ROCKS_PLATFORMBASE_API void SetThisThreadSyncStep(SyncStepFunction step) noexcept;
// 이 스레드에 단계 훅이 있으면 부른다.
void SyncStep(const char* name) noexcept;
// ParkingLot 영구 풀의 free list에 있는 레코드 수다(시험이 레코드 반환을 확인한다, D31).
ROCKS_PLATFORMBASE_API uint32 ParkingLotFreeRecordCount() noexcept;
#endif

#if !ROCKS_BUILD_RELEASE
ROCKS_PLATFORMBASE_API void SetSyncTestClock(SyncClockFunction clock) noexcept;
ROCKS_PLATFORMBASE_API void SetSyncTestTimedWait(SyncTimedWaitFunction wait) noexcept;
// 설치된 가짜 시한 대기다(없으면 nullptr).
SyncTimedWaitFunction SyncTestTimedWait() noexcept;
#endif

} // namespace rocks::platform::detail

// PlatformBase 동기화 구현(.cpp)의 단계 지점이다(14.3절, D24). 느린 경로(spin 뒤·대기·깨우기)에만
// 둔다. Debug·Development에서 이 스레드에 단계 훅이 있으면 부르고, Release에서는 비어 있다.
#if !ROCKS_BUILD_RELEASE
#define ROCKS_SYNC_STEP(name) ::rocks::platform::detail::SyncStep(name)
#else
#define ROCKS_SYNC_STEP(name) static_cast<void>(0)
#endif
