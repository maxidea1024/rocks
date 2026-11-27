#pragma once

#include <Core/Result.h>
#include <Progress/ProgressTypes.h>

namespace rocks {

// 시작한 작업 하나의 소유 handle이다(Docs/Design/ProgressReporting 5.1·5.3절). ProgressHub::Begin이
// 만든다. 이동만 할 수 있고, 다른 스레드로 넘겨 거기서 Finish해도 된다(자동 바인딩이 없으므로
// 매달린 바인딩이 남지 않는다).
//
// 사용 예:
//   auto task = ProgressHub::Get().Begin(desc);
//   if (!task) { return Result<void>::Failure(task.Error()); }
//   ProgressBinding binding = ...Bind(task.Value().Id(), ProgressBindingRole::Primary);
//   { ProgressScope scope(3.0); ... }
//   (void)task.Value().Finish(ProgressOutcome::Succeeded);
//
// 수명: 작업 기록은 참조 계수다. handle·계수·바인딩이 기록을 붙잡으므로 hub Shutdown 뒤나 작업이
// 끝난 뒤의 보고는 아무 일도 하지 않는다. Finish 없이 소멸하면 Abandoned로 끝내고 Warning 로그를
// 남긴다.
class ROCKS_PROGRESS_API ProgressTaskHandle {
public:
    ProgressTaskHandle() noexcept = default;
    ProgressTaskHandle(ProgressTaskHandle&& other) noexcept;
    ProgressTaskHandle& operator=(ProgressTaskHandle&& other) noexcept;
    ProgressTaskHandle(const ProgressTaskHandle&) = delete;
    ProgressTaskHandle& operator=(const ProgressTaskHandle&) = delete;
    ~ProgressTaskHandle() noexcept;

    // 작업 id다(빈 handle이면 NoProgressTask).
    ProgressTaskId Id() const noexcept;

    // 작업의 취소 token이다. UserCancellable이면 hub source의 token(부모·외부 token에 연결), 아니면
    // 부모 작업 token 또는 외부 token 그대로, 둘 다 없으면 취소될 수 없는 기본 token이다.
    CancelToken Token() const noexcept;

    // 작업 문구를 바꾼다(구간 문구가 없을 때 보인다). 문구는 Clone한다.
    // 오류: 빈 handle은 InvalidArgument, hub가 닫혔으면 ShuttingDown, 이미 끝났으면 ScopeClosed,
    // Clone 실패는 그 오류.
    Result<void> SetMessage(const Text& message) noexcept;

    // 작업을 끝낸다(한 번). 살아 있는 하위 작업은 먼저 Abandoned로 닫힌다. Succeeded면 보이는
    // 비율이 1이 된다. 오류: SetMessage와 같다. detail Clone이 실패하면 사유 없이 끝내고 그 오류를
    // 돌려준다.
    Result<void> Finish(ProgressOutcome outcome) noexcept;
    Result<void> Finish(ProgressOutcome outcome, const Text& detail) noexcept;

    bool IsValid() const noexcept {
        return record_ != nullptr;
    }

private:
    friend class ProgressHub;
    friend class ProgressCounter;

    explicit ProgressTaskHandle(detail::ProgressTaskRecord* record) noexcept : record_(record) {
    }

    Result<void> FinishWith(ProgressOutcome outcome, const Text* detail) noexcept;
    void Release() noexcept;

    detail::ProgressTaskRecord* record_ = nullptr;
};

} // namespace rocks
