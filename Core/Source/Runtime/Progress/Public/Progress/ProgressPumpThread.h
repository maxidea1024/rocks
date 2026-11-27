#pragma once

#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Progress/ProgressTypes.h>

namespace rocks {

struct ProgressPumpThreadDesc {
    // 간격마다 부르는 펌프다(필수 — 보통 ConsoleProgressReporter·ProgressJsonlWriter의 Pump를
    // 감싼다).
    void (*pump)(void* context, uint64 nowNanoseconds) noexcept = nullptr;
    void* context = nullptr;
    // 종료 요청(Ctrl-C) 확인이다(선택 — 명령 쪽 glue가 Launch IsTerminationRequested를 감싼다.
    // Progress 모듈은 Launch에 의존하지 않는다). 처음 true를 보면 RequestCancelAllTopLevel을 한 번
    // 부른다.
    bool (*terminationRequested)(void* context) noexcept = nullptr;
    void* terminationContext = nullptr;
    // 펌프 간격(밀리초)이다. 1 이상.
    uint32 intervalMilliseconds = ChildControlPollMilliseconds;
};

// 한 호출로 오래 막는 명령의 펌프 주인이다(Docs/Design/ProgressReporting 9.1절). 전용 스레드가
// 간격마다 Tick을 부른다: 펌프를 부르고 종료 요청을 확인한다. Stop은 스레드를 깨워 마지막 Tick을
// 한 번 더 한 뒤 회수한다(끝 사건 줄이 빠지지 않게).
//
// 사용 예:
//   ProgressPumpThreadDesc desc;
//   desc.pump = [](void* context, uint64 now) noexcept {
//       (void)static_cast<ConsoleProgressReporter*>(context)->Pump(now);
//   };
//   desc.context = &console;
//   desc.terminationRequested = [](void*) noexcept { return launch::IsTerminationRequested(); };
//   ProgressPumpThread pump;
//   if (auto started = pump.Start(allocator, desc); !started) { ... }
//   CookEverything();   // 오래 막는 호출
//   pump.Stop();
//
// 스레드: Start·Stop·소멸은 한 스레드(만든 스레드)에서 한다. Tick은 스레드가 부른다(시험은 스레드
// 없이 직접 불러 순서를 정한다). 소멸은 Stop을 부른다.
class ROCKS_PROGRESS_API ProgressPumpThread {
public:
    ProgressPumpThread() noexcept;
    ~ProgressPumpThread() noexcept;
    ProgressPumpThread(const ProgressPumpThread&) = delete;
    ProgressPumpThread& operator=(const ProgressPumpThread&) = delete;

    // 스레드를 시작한다. allocator는 스레드 상태를 할당하고 Stop까지 살아 있어야 한다. 오류: pump가
    // null이거나 간격이 0이면 InvalidArgument, 이미 돌고 있으면 AlreadyExists, 스레드를 만들지
    // 못하면 OutOfMemory.
    Result<void> Start(Allocator& allocator, const ProgressPumpThreadDesc& desc) noexcept;

    // 스레드를 멈추고 회수한다(마지막 Tick 한 번). 돌고 있지 않으면 아무 일도 하지 않는다.
    void Stop() noexcept;

    // 펌프 한 번과 종료 요청 확인이다(스레드 본문 — 시험은 직접 부른다).
    void Tick(uint64 nowNanoseconds) noexcept;

    // RequestCancelAllTopLevel을 부른 횟수다(0 또는 1 — 시험·진단). 스레드가 쓰므로 Stop 뒤에
    // 읽는다.
    uint32 CancelRequests() const noexcept {
        return cancelRequests_;
    }

    // Start 없이 Tick만 쓰는 쪽(시험)이 desc를 넘긴다.
    void Configure(const ProgressPumpThreadDesc& desc) noexcept {
        desc_ = desc;
    }

private:
    struct State;

    static void Run(void* context) noexcept;

    ProgressPumpThreadDesc desc_;
    Allocator* allocator_ = nullptr;
    State* state_ = nullptr;
    uint32 cancelRequests_ = 0;
};

} // namespace rocks
