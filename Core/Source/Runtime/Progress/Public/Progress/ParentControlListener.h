#pragma once

#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <Platform/File.h>
#include <Progress/ProgressTypes.h>

namespace rocks {

// 부모 제어 입력을 읽는다. 데이터가 없으면 timeoutMilliseconds 뒤 NoProgress다(Platform
// ReadStandardInput timeout 계약).
using ParentControlReader = platform::FileTransfer (*)(void* context, void* buffer, usize capacity,
                                                       uint32 timeoutMilliseconds) noexcept;

struct ParentControlListenerDesc {
    // null이면 표준 입력(platform::ReadStandardInput timeout)이다. 시험은 줄을 넣는다.
    ParentControlReader read = nullptr;
    void* context = nullptr;
};

// 자식 명령의 부모 제어 수신이다(Docs/Design/ProgressReporting 10.2절). --parent-control로 실행한
// 명령만 켠다. 전용 스레드가 표준 입력 줄을 읽고:
// - "cancel" → ProgressHub::RequestCancelAllTopLevel. 명령은 정상 경로로 Cancelled를 내고 종료 코드
//   ProgressCancelledExitCode(130)로 끝난다.
// - 입력 끝(EOF, 부모가 사라짐) → 같은 취소, 그 뒤 읽기를 멈춘다(셰이더 워커의 "입력 EOF면 정리하고
//   끝남"과 같다).
// - 모르는 줄 → Warning 한 번 뒤 무시.
// 표준 입력이 콘솔이면(사람이 손으로 띄움) Warning 한 번 뒤 수신을 끈다(취소하지 않는다).
// --parent-control 없이 실행한 명령은 표준 입력을 보지 않는다(CI가 stdin을 닫고 띄워도 취소되지
// 않는다).
//
// 사용 예:
//   ParentControlListener control;
//   if (options.parentControl) { (void)control.Start(allocator); }
//   ...                              // 작업은 token으로 취소를 본다
//   control.Stop();
//
// 스레드: Start·Stop·소멸은 한 스레드에서 한다. Stop은 읽기 대기(ChildControlPollMilliseconds) 한
// 번 안에 끝난다.
class ROCKS_PROGRESS_API ParentControlListener {
public:
    ParentControlListener() noexcept;
    ~ParentControlListener() noexcept;
    ParentControlListener(const ParentControlListener&) = delete;
    ParentControlListener& operator=(const ParentControlListener&) = delete;

    // 수신 스레드를 시작한다. allocator는 스레드 상태를 할당하고 Stop까지 살아 있어야 한다.
    // 오류: 이미 돌고 있으면 AlreadyExists, 스레드를 만들지 못하면 OutOfMemory.
    Result<void> Start(Allocator& allocator, const ParentControlListenerDesc& desc = {}) noexcept;

    // 수신 스레드를 멈추고 회수한다. 돌고 있지 않으면 아무 일도 하지 않는다.
    void Stop() noexcept;

    // 받은 취소 요청 수다(cancel 줄·EOF — 시험·진단). Stop 뒤에 읽는다.
    uint32 CancelRequests() const noexcept {
        return cancelRequests_;
    }

    // 모르는 줄 수다(시험·진단). Stop 뒤에 읽는다.
    uint32 UnknownLines() const noexcept {
        return unknownLines_;
    }

private:
    struct State;

    static void Run(void* context) noexcept;
    void HandleLine(StringView line) noexcept;
    void Cancel() noexcept;

    ParentControlListenerDesc desc_;
    Allocator* allocator_ = nullptr;
    State* state_ = nullptr;
    uint32 cancelRequests_ = 0;
    uint32 unknownLines_ = 0;
};

} // namespace rocks
