#pragma once

#include <Core/Memory/Allocator.h>
#include <Progress/ProgressSnapshot.h>
#include <Progress/ProgressTaskHandle.h>

namespace rocks {

// 진행 중계다(Docs/Design/ProgressReporting 7장, UE FFeedbackContext·알림 factory 대응). 프로세스에
// 하나이고 저장소는 Progress cpp 안에 있다. 엔진·도구 시작이 Initialize, 종료가 Shutdown을 부른다.
// 보고하는 쪽(작업·구간·계수)은 받는 쪽 코드를 부르지 않는다. 받는 쪽은 자기 빈도로 Snapshot을
// 당긴다(UI 펌프 hook은 UI 스레드 자신의 호출이다).
//
// 사용 예(명령 하나):
//   ProgressHub& hub = ProgressHub::Get();
//   (void)hub.Initialize(allocator, {});
//   auto title = Text::Invariant(allocator, "Cooking packages");
//   ProgressTaskDesc desc;
//   desc.title = &title.Value();
//   desc.cancel = ProgressCancel::UserCancellable;
//   auto task = hub.Begin(desc);
//   ...
//   hub.Shutdown();
//
// 스레드: 모든 함수를 아무 스레드에서나 부를 수 있다(SetUiPump·ClearUiPump는 UI 스레드). 상태는 hub
// 잠금 하나가 지키고, 구간·계수의 진행 보고는 잠금 없이 원자 연산으로 한다.
class ROCKS_PROGRESS_API ProgressHub {
public:
    static ProgressHub& Get() noexcept;

    // hub를 켠다. allocator는 작업 기록·사건 고리·hub 취소 source를 할당하고, 마지막 작업 기록이
    // 놓일 때까지(Shutdown 뒤 남은 handle 포함) 살아 있어야 하며 다른 스레드의 해제를 지원해야
    // 한다. Shutdown 뒤 다시 켤 수 있다(그 전 handle은 계속 ShuttingDown이다). 오류: 이미 켜졌으면
    // AlreadyExists, 설정이 범위 밖이면 InvalidArgument, 할당 실패는 그 오류.
    Result<void> Initialize(Allocator& allocator, const ProgressHubSettings& settings) noexcept;

    // 살아 있는 작업을 Abandoned로 닫고(Warning 로그) 사건 고리를 비운 뒤 hub를 닫는다. 이후
    // Begin은 ShuttingDown이다. 켜지지 않았으면 아무 일도 하지 않는다.
    void Shutdown() noexcept;

    // 작업을 시작한다. 오류(5.2절): 켜기 전 NotReady, 닫힌 뒤 ShuttingDown, 설명이 잘못되면
    // InvalidArgument, 발급된 적 없는 부모 NotFound, 끝난 부모 StaleHandle, 살아 있는 작업 수 초과
    // CapacityExceeded, Clone·할당 실패는 그 오류.
    Result<ProgressTaskHandle> Begin(const ProgressTaskDesc& desc) noexcept;

    // 작업 하나의 취소를 요청한다(하위 작업은 연결 취소로 함께 취소된다). 오류:
    // NotReady·ShuttingDown, 발급된 적 없는 id NotFound, 끝난 작업 StaleHandle, 취소 불가 작업
    // Unsupported.
    Result<void> RequestCancel(ProgressTaskId id) noexcept;

    // 취소 가능한 최상위 작업을 모두 취소한다(Ctrl-C·부모 cancel 줄). 잠금을 쓰므로 신호 처리기에서
    // 부르지 않는다.
    void RequestCancelAllTopLevel() noexcept;

    // 살아 있는 작업 트리와 cursor 뒤의 사건을 out에 담는다(7.2·7.3절). cursor는 소비자마다 따로
    // 두는 불투명 값이고 0은 "지금부터"다(처음 호출은 사건 없이 살아 있는 트리만 받는다). 보이는
    // 비율 D는 이 호출이 hub 잠금 안에서 합성해 작업마다 고정한다. 오류: NotReady·ShuttingDown,
    // Clone·할당 실패는 그 오류(out은 비운다).
    Result<void> Snapshot(ProgressSnapshot& out, uint64& cursor) noexcept;

    // 호출한 스레드를 UI 스레드로 등록하고 펌프를 건다(6.3절). 같은 스레드가 다시 부르면 바꾼다.
    // 오류: pump가 null이면 InvalidArgument, NotReady·ShuttingDown, 다른 스레드가 이미 등록했으면
    // AlreadyExists.
    Result<void> SetUiPump(const ProgressUiPump& pump) noexcept;

    // UI 펌프를 푼다. UI 스레드에서 부른다(다른 스레드면 아무 일도 하지 않는다).
    void ClearUiPump() noexcept;

    // 이번 Initialize 뒤의 진단 계수다(켜지지 않았으면 마지막 켜짐의 값).
    ProgressHubStats Stats() const noexcept;

private:
    friend class ProgressTestAccess;

    ProgressHub() noexcept = default;

    // 살아 있는 작업을 닫고 저장소를 놓는다(hub 잠금 안).
    void ShutdownLocked() noexcept;

    // 닫힌 hub를 "켜기 전"으로 되돌린다(시험 접근점 전용 — 켜져 있으면 아무 일도 하지 않는다).
    void ReturnToUninitialized() noexcept;
};

} // namespace rocks
