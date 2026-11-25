#pragma once

#include <Platform/Sync/WaiterSignal.h>

#include "SyncSeams.h"

// 동기화 시험 통로다(Docs/Design/Synchronization 14.2절). 제품 코드는 쓰지 않는다 — Tests/ 소스만
// include한다(정책 검사 test-access-include). 시험은
// <PlatformBase/Private/Common/Sync/SyncTestAccess.h>로 쓴다. Release에는 seam이 없으므로 이 헤더도
// Release 시험에서는 쓰지 않는다.
namespace rocks::platform::detail {

#if !ROCKS_BUILD_RELEASE
struct SyncTestAccess {
    // 동기화 시한 계산의 단조 시계를 바꾼다(nullptr이면 실제 시계). 시험 끝에 Clear한다.
    static void InstallClock(SyncClockFunction clock) noexcept {
        SetSyncTestClock(clock);
    }

    // WaiterSignal::WaitFor의 유한 시한 대기를 바꾼다(nullptr이면 OS 대기). 무한·0 대기는 바꾸지
    // 않는다. 가짜 대기는 보통 대상 신호를 무한으로 기다리고(Wait), 시험이 "시간이 지났다"를 정하면
    // 표시를 남긴 뒤 그 신호를 Notify해 깨운다. 깬 가짜 대기는 표시를 보고 Reset한 뒤 false(시간
    // 초과)를 돌려준다. 그래서 실제 시간 없이 시한 전 깨움과 시한 뒤 시간 초과를 정한 순서로
    // 만든다.
    static void InstallTimedWait(SyncTimedWaitFunction wait) noexcept {
        SetSyncTestTimedWait(wait);
    }

    // ParkingLot 시험 훅을 바꾼다(nullptr이면 끈다). onParked는 대기자가 큐에 들어가고 beforeWait를
    // 마친 뒤 잠들기 직전에 불린다. 시험은 여기서 gate를 열어 "그 스레드가 이제 잔다"를 안다.
    static void InstallParkHooks(SyncParkFunction onParked, SyncParkFunction onUnparked) noexcept {
        SetSyncTestParkHooks(onParked, onUnparked);
    }

    // 이 스레드의 단계 훅(ROCKS_SYNC_STEP)을 바꾼다. 스레드 지역이라 그 스레드에서 부른다.
    static void InstallThisThreadStep(SyncStepFunction step) noexcept {
        SetThisThreadSyncStep(step);
    }

    // ParkingLot 영구 풀의 free list 레코드 수다.
    static uint32 FreeRecordCount() noexcept {
        return ParkingLotFreeRecordCount();
    }

    static void Clear() noexcept {
        SetSyncTestClock(nullptr);
        SetSyncTestTimedWait(nullptr);
        SetSyncTestParkHooks(nullptr, nullptr);
    }
};
#endif

} // namespace rocks::platform::detail
