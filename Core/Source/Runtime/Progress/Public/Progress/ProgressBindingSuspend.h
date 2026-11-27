#pragma once

#include <Progress/ProgressTypes.h>

namespace rocks {

// 이 스레드의 현재 작업을 잠시 "없음"으로 덮는다(Docs/Design/ProgressReporting 6.1·6.5절). 바인딩된
// 스레드가 병렬 본문을 직접 실행할 때 조각 안의 라이브러리 구간이 Primary 진행을 흐리지 않게 한다.
// 그 사이 연 구간은 아무 일도 하지 않는다(진행은 계수로 보고한다).
//
// 사용 예:
//   ProgressBindingSuspend suspend;
//   auto done = ParallelFor(scheduler, scratch, count, body);
//
// 규칙: ProgressBinding과 같다(같은 스레드, 생성의 역순, 그 위의 구간은 먼저 닫음). 겹침 깊이가
// MaxProgressBindingSuspendDepth를 넘으면 Fatal이다.
class ROCKS_PROGRESS_API ProgressBindingSuspend {
public:
    ProgressBindingSuspend() noexcept;
    ~ProgressBindingSuspend() noexcept;
    ProgressBindingSuspend(const ProgressBindingSuspend&) = delete;
    ProgressBindingSuspend& operator=(const ProgressBindingSuspend&) = delete;

private:
    uint32 depth_ = 0;
};

} // namespace rocks
