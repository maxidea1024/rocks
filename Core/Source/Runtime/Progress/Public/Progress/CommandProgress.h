#pragma once

#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <Progress/ConsoleProgressReporter.h>
#include <Progress/ParentControlListener.h>
#include <Progress/ProgressJsonlWriter.h>

namespace rocks {

// 명령의 진행 출력 방식이다(Docs/Design/ProgressReporting 9.5절).
enum class CommandProgressMode : uint8 {
    // 사람이 읽는 줄(Log API — stderr)이다. 기본이다.
    Console,
    // 표준 출력의 JSONL v1이다(--progress=jsonl, 9.3절).
    Jsonl,
    // 진행 줄을 내지 않는다(--no-progress).
    Off
};

// 명령줄에서 읽은 진행 설정이다.
struct CommandProgressOptions {
    CommandProgressMode mode = CommandProgressMode::Console;
    // --parent-control: 표준 입력을 부모 제어 채널로 읽는다(10.2절).
    bool parentControl = false;
    // 모드 인자(--progress=·--no-progress)를 이미 받았는가다(ParseCommandProgressArgument가 둘째
    // 모드 인자를 거부하려고 쓴다).
    bool modeGiven = false;
};

// 인자 하나의 해석 결과다.
enum class CommandProgressArgument : uint8 {
    // 진행 인자가 아니다(명령이 자기 인자로 읽는다).
    NotProgress,
    // 진행 인자를 읽어 options에 넣었다.
    Consumed,
    // 진행 인자지만 잘못됐다(모르는 값·둘째 모드 인자 — 명령의 사용법 오류).
    Invalid
};

// 명령 인자 하나를 진행 인자로 읽는다: --progress=console·--progress=jsonl·--no-progress·
// --parent-control. 그 밖은 NotProgress다.
//
// 사용 예:
//   CommandProgressOptions progress;
//   for (const StringView argument : args) {
//       switch (ParseCommandProgressArgument(argument, progress)) {
//       case CommandProgressArgument::Consumed: continue;
//       case CommandProgressArgument::Invalid: return UsageError();
//       case CommandProgressArgument::NotProgress: break;
//       }
//       ...                                   // 명령 자기 인자
//   }
ROCKS_PROGRESS_API CommandProgressArgument
ParseCommandProgressArgument(StringView argument, CommandProgressOptions& options) noexcept;

// 시험·호스트가 바꾸는 출력·입력이다. 비운 칸은 기본(표준 출력·Log API·표준 입력)이다.
struct CommandProgressDesc {
    // JSONL 줄 출력이다(비면 표준 출력 — ProgressJsonlWriterDesc::write).
    ProgressLineWriter jsonlWrite = nullptr;
    void* jsonlContext = nullptr;
    // 사람용 줄 출력이다(비면 Log API — ConsoleProgressReporterDesc::write).
    ProgressLogWriter consoleWrite = nullptr;
    void* consoleContext = nullptr;
    // 부모 제어 읽기다(비면 표준 입력 — ParentControlListenerDesc::read).
    ParentControlReader controlRead = nullptr;
    void* controlContext = nullptr;
    // 종료 요청(Ctrl-C) 확인이다(선택 — 명령 쪽이 Launch IsTerminationRequested를 감싼다).
    bool (*terminationRequested)(void* context) noexcept = nullptr;
    void* terminationContext = nullptr;
};

// 명령의 진행 연결이다(Docs/Design/ProgressReporting 9.5절). hub를 (꺼져 있으면) 켜고, 모드대로
// 보고기를 만들어 펌프 스레드로 돌리고, --parent-control이면 부모 제어 수신기를 켠다. 명령은
// 최상위 작업을 UserCancellable로 열고 작업 token으로 취소를 보며, 작업을 끝낸 뒤 Stop을 부른다.
// 취소로 끝난 명령의 종료 코드는 ProgressCancelledExitCode(130)다. JSONL 모드면 명령의 다른 표준
// 출력 줄은 표준 오류로 보낸다(9.3절).
//
// 사용 예:
//   CommandProgress progress;
//   if (auto started = progress.Start(allocator, options); !started) { ... }
//   auto task = ProgressHub::Get().Begin(desc);     // UserCancellable
//   ... task.Value().Token() ...                    // 일
//   progress.Flush();                               // 마지막 계수 줄
//   (void)task.Value().Finish(outcome);
//   progress.Stop();
//
// 스레드: Start·Stop·소멸은 한 스레드(명령의 주 스레드)에서 한다.
class ROCKS_PROGRESS_API CommandProgress {
public:
    CommandProgress() noexcept;
    // Stop을 부른다.
    ~CommandProgress() noexcept;
    CommandProgress(const CommandProgress&) = delete;
    CommandProgress& operator=(const CommandProgress&) = delete;

    // 연결을 켠다. allocator는 hub(켠다면)·보고기·스레드 상태를 할당하고 Stop까지 살아 있으며
    // 다른 스레드의 할당·해제를 받아야 한다. hub가 이미 켜져 있으면 그 hub를 쓰고 끄지 않는다.
    // 오류: 이미 켰으면 AlreadyExists, hub Initialize·펌프 스레드·수신기 시작의 오류(켠 것은
    // 되돌린다), 할당 실패.
    Result<void> Start(Allocator& allocator, const CommandProgressOptions& options,
                       const CommandProgressDesc& desc = {}) noexcept;

    // 간격과 상관없이 지금의 progress 줄을 쓴다(JSONL 모드만 — 사람용 줄은 간격대로다). 작업을
    // 끝내기 직전에 부르면 마지막 계수(done = total)가 end 줄 앞에 나간다. 켜지 않았으면 아무 일도
    // 하지 않는다.
    void Flush() noexcept;

    // 수신기 → 펌프 스레드(마지막 Tick) → JSONL Flush → 소유한 hub Shutdown 순으로 닫는다. 켜지
    // 않았으면 아무 일도 하지 않는다.
    void Stop() noexcept;

    // 이 연결이 hub를 켰는가(시험·진단)다.
    bool OwnsHub() const noexcept;

private:
    struct State;
    static void Pump(void* context, uint64 nowNanoseconds) noexcept;

    Allocator* allocator_ = nullptr;
    State* state_ = nullptr;
};

} // namespace rocks
