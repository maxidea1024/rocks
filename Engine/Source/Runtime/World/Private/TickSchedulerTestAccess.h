#pragma once

#include <Base/Types.h>
#include <World/TickScheduler.h>

// 시험이 Reaches의 방문 번호(markEpoch_)를 한 바퀴 끝 근처로 옮기고 선행 목록 길이를 보는 모듈 내부
// 접근점이다(2^32번
// 의존을 추가하지 않고 번호가 0으로 돌 때 방문 표시를 비우는 경로를 시험한다). 공개 API에는 이
// 번호를 바꾸는 길을 두지 않으므로 Tests/ 소스만 include한다(정책 검사 test-access-include).
namespace rocks::detail {

struct TickSchedulerTestAccess {
    static void SetMarkEpoch(TickScheduler& scheduler, uint32 epoch) noexcept {
        scheduler.markEpoch_ = epoch;
    }

    static uint32 MarkEpoch(const TickScheduler& scheduler) noexcept {
        return scheduler.markEpoch_;
    }

    // 등록이 들고 있는 선행 handle 수다(해제된 선행이 쌓이지 않는지 본다). 없는 등록은 0이다.
    static usize PrerequisiteCount(const TickScheduler& scheduler, TickHandle handle) noexcept {
        const TickScheduler::Entry* entry = scheduler.entries_.Find(handle.slot);
        return entry == nullptr ? 0 : entry->prerequisites.Size();
    }
};

} // namespace rocks::detail
