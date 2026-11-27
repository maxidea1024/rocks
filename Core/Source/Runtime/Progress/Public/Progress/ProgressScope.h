#pragma once

#include <Progress/ProgressCounter.h>

namespace rocks {

// 바인딩된 작업 아래의 중첩 가중 구간이다(Docs/Design/ProgressReporting 6.2절, UE FScopedSlowTask
// 모델). 호출자는 자기 일만 나누고 바깥 비율을 모른다: 안쪽 구간은 바깥의 현재 구간 하나를 채운다.
//
// 사용 예:
//   ProgressScope scope(3.0, buildMeshText);   // 문구는 미리 만든 Text
//   scope.EnterStep(1.0, weldVerticesText);
//   Weld();
//   scope.EnterStep(2.0);                      // 문구 없음 — 범위 기본 문구
//   BuildLods();                               // 안에서 다시 구간을 열면 이 2.0이 그 범위 전체
//
// 계약:
// - 이 스레드에 바인딩이 없거나 ProgressBindingSuspend 아래면 아무 일도 하지 않는다(모듈 경계 호출
//   한 번과 thread-local 읽기 한 번, 할당 없음). Secondary 바인딩 아래면 취소 조회만 한다.
// - 총량 0은 1로 보고 범위 전체가 한 구간이다. EnterStep이 앞 구간을 끝내고, 소멸은 남은 양을
//   채운다(범위 끝 값을 발행).
// - 작업량이 음수·NaN·무한이면 0으로 보고, 남은 양을 넘는 EnterStep은 잘라 낸다(hub 켜짐마다 처음
//   한 번 Warning). 깊이 MaxProgressScopeDepth를 넘는 구간은 아무 일도 하지 않는다(처음 한 번
//   Warning).
// - 문구 없는 EnterStep·Tick은 할당·잠금이 없다(원자 쓰기와 seqlock). 문구가 있으면 Text::Clone을
//   하고, 실패하면 문구만 버린다(처음 한 번 Warning).
// - 스택 객체로만 쓴다: 같은 스레드에서 연 역순으로 닫고, 바인딩보다 먼저 닫는다.
class ROCKS_PROGRESS_API ProgressScope {
public:
    explicit ProgressScope(double totalWork) noexcept;
    ProgressScope(double totalWork, const Text& defaultMessage) noexcept;
    ~ProgressScope() noexcept;
    ProgressScope(const ProgressScope&) = delete;
    ProgressScope& operator=(const ProgressScope&) = delete;

    // 앞 구간을 끝내고 work만큼의 새 구간에 들어간다.
    void EnterStep(double work = 1.0) noexcept;
    void EnterStep(double work, const Text& message) noexcept;

    // 진행 없이 살아 있음을 알린다(UI 스레드면 펌프 기회). 긴 외부 호출 사이사이 부른다.
    void Tick() noexcept;

    // 이 구간이 진행·문구를 발행하는지다(Primary 바인딩 아래의 활성 구간). 문구 Text를 만드는
    // 비용(할당)을 보고가 쓰일 때만 내려는 호출자가 본다.
    bool IsReporting() const noexcept {
        return frame_ != 0 && primary_;
    }

    bool IsCancellationRequested() const noexcept;
    Result<void> CheckCancelled() const noexcept;

    // 현재 구간을 채우는 계수를 만든다. 바인딩이 없거나 Secondary면 빈 계수다. 오류: 작업에 살아
    // 있는 구간 계수가 이미 있으면 AlreadyExists.
    Result<ProgressCounter> MakeCounter(uint64 total) noexcept;

private:
    void Open(double totalWork, const Text* defaultMessage) noexcept;
    void Enter(double work, const Text* message) noexcept;
    void Publish() noexcept;

    detail::ProgressTaskRecord* record_ = nullptr;
    ProgressScope* parent_ = nullptr;
    // 바인딩 스택 칸 번호 + 1이다(0이면 활성이 아님 — 구간이 아무 일도 하지 않는다).
    uint32 frame_ = 0;
    // 그 바인딩 위에서의 구간 깊이다(0이 가장 바깥).
    uint32 depth_ = 0;
    bool primary_ = false;
    bool hasStepMessage_ = false;
    bool hasDefaultMessage_ = false;
    double total_ = 1.0;
    double completed_ = 0.0;
    double current_ = 0.0;
    // 이 범위가 작업 전체에서 차지하는 시작과 폭이다.
    double rangeBase_ = 0.0;
    double rangeWidth_ = 1.0;
    uint64 stepSerial_ = 0;
    // trace span(TraceCapture TraceSpanId의 두 값)이다.
    uint64 spanValue_ = 0;
    uint64 spanSerial_ = 0;
};

} // namespace rocks
