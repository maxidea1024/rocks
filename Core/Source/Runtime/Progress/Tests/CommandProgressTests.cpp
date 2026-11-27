#include <Core/Containers/Array.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Move.h>
#include <Core/Text/Json.h>
#include <Core/Text/String.h>
#include <Localization/Text.h>
#include <Platform/Sync.h>
#include <Progress/CommandProgress.h>
#include <Progress/ProgressCounter.h>
#include <Progress/ProgressHub.h>
#include <Progress/ProgressTaskHandle.h>
#include <TestSupport/Test.h>
#include <TestSupport/TestGate.h>

using namespace rocks;
namespace command_progress_tests_file {

// 명령 진행 연결 시험(Docs/Design/ProgressReporting 9.5절, 14장 27). 줄은 주입 writer로 모은다(펌프
// 스레드와 시험 스레드가 쓰므로 잠금).
struct Collected {
    explicit Collected(Allocator& allocator) noexcept : allocator(allocator), lines(allocator) {
    }

    Allocator& allocator;
    platform::Mutex gate;
    Array<String> lines;

    static void Write(void* context, StringView line) noexcept {
        auto& self = *static_cast<Collected*>(context);
        platform::LockGuard lock(self.gate);
        if (auto added = self.lines.TryEmplaceBack(self.allocator)) {
            (void)added.Value()->TryAssign(line);
        }
    }

    // e 칸이 event이고 key 칸 문자열이 value인 줄이 있는가.
    bool HasEvent(StringView event, StringView key, StringView value) noexcept {
        platform::LockGuard lock(gate);
        for (const String& line : lines) {
            JsonDocument document(allocator);
            if (!document.Parse(line.View()).Succeeded()) {
                continue;
            }

            const JsonValue kind = document.Root().Find("e");
            const JsonValue field = document.Root().Find(key);
            if (kind.IsValid() && kind.IsString() && kind.GetString() == event && field.IsValid() &&
                field.IsString() && field.GetString() == value) {
                return true;
            }
        }

        return false;
    }

    // count 모양 progress 줄 가운데 done = total = expected인 것이 있는가.
    bool HasFinalCount(uint64 expected) noexcept {
        platform::LockGuard lock(gate);
        for (const String& line : lines) {
            JsonDocument document(allocator);
            if (!document.Parse(line.View()).Succeeded()) {
                continue;
            }

            const JsonValue shape = document.Root().Find("shape");
            auto done = document.Root().Find("done").TryGetUInt64();
            auto total = document.Root().Find("total").TryGetUInt64();
            if (shape.IsValid() && shape.IsString() && shape.GetString() == "count" && done &&
                total && done.Value() == expected && total.Value() == expected) {
                return true;
            }
        }

        return false;
    }
};

Result<ProgressTaskHandle> BeginCommandTask(Allocator& allocator, const char* title) noexcept {
    auto text = Text::Invariant(allocator, title);
    if (!text) {
        return Result<ProgressTaskHandle>::Failure(text.Error());
    }

    ProgressTaskDesc desc;
    desc.title = &text.Value();
    desc.cancel = ProgressCancel::UserCancellable;
    return ProgressHub::Get().Begin(desc);
}

// 부모 제어 줄 "cancel" 하나를 준 뒤 gate가 열릴 때까지 NoProgress를 돌려주는 가짜 입력이다.
struct CancelInput {
    bool sent = false;
    test::TestGate delivered;
};

platform::FileTransfer ReadCancel(void* context, void* buffer, usize capacity,
                                  uint32 timeoutMilliseconds) noexcept {
    (void)timeoutMilliseconds;
    auto& input = *static_cast<CancelInput*>(context);
    platform::FileTransfer result;
    constexpr StringView Line = "cancel\n";
    if (input.sent || capacity < Line.ByteCount()) {
        result.status.code = platform::FileCode::NoProgress;
        input.delivered.Open();
        return result;
    }

    for (usize index = 0; index < Line.ByteCount(); ++index) {
        static_cast<char*>(buffer)[index] = Line.Data()[index];
    }

    input.sent = true;
    result.bytes = Line.ByteCount();
    return result;
}

// 네 인자를 읽고, 둘째 모드 인자·모르는 값은 Invalid, 다른 인자는 NotProgress다.
TEST(Progress_CommandProgressParsesArguments) {
    CommandProgressOptions options;
    CHECK(ParseCommandProgressArgument("--project=Game", options) ==
          CommandProgressArgument::NotProgress);
    CHECK(options.mode == CommandProgressMode::Console && !options.parentControl);
    CHECK(ParseCommandProgressArgument("--progress=jsonl", options) ==
          CommandProgressArgument::Consumed);
    CHECK(options.mode == CommandProgressMode::Jsonl);
    CHECK(ParseCommandProgressArgument("--no-progress", options) ==
          CommandProgressArgument::Invalid);
    CHECK(ParseCommandProgressArgument("--parent-control", options) ==
          CommandProgressArgument::Consumed);
    CHECK(options.parentControl && options.mode == CommandProgressMode::Jsonl);

    CommandProgressOptions off;
    CHECK(ParseCommandProgressArgument("--no-progress", off) == CommandProgressArgument::Consumed);
    CHECK(off.mode == CommandProgressMode::Off);
    CommandProgressOptions console;
    CHECK(ParseCommandProgressArgument("--progress=console", console) ==
          CommandProgressArgument::Consumed);
    CHECK(console.mode == CommandProgressMode::Console && console.modeGiven);
    CommandProgressOptions bad;
    CHECK(ParseCommandProgressArgument("--progress=xml", bad) == CommandProgressArgument::Invalid);
    CHECK(ParseCommandProgressArgument("--progress", bad) == CommandProgressArgument::NotProgress);
}

// 꺼진 hub를 켜고 Stop에서 끈다. JSONL 모드는 begin·마지막 계수(Flush)·end 줄을 쓴다. 두 번째
// Start는 AlreadyExists, Stop은 두 번 불러도 된다.
TEST(Progress_CommandProgressWritesJsonlAndOwnsTheHub) {
    SystemAllocator allocator;
    Collected collected(allocator);
    CommandProgressOptions options;
    options.mode = CommandProgressMode::Jsonl;
    CommandProgressDesc desc;
    desc.jsonlWrite = &Collected::Write;
    desc.jsonlContext = &collected;
    CommandProgress progress;
    REQUIRE(progress.Start(allocator, options, desc));
    CHECK(progress.OwnsHub());
    CHECK(progress.Start(allocator, options, desc).Error() == ErrorCode::AlreadyExists);
    {
        auto begun = BeginCommandTask(allocator, "Compiling things");
        REQUIRE(begun);
        ProgressTaskHandle task = Move(begun.Value());
        auto counter = ProgressCounter::ForTask(task, 0);
        REQUIRE(counter);
        counter.Value().AddTotal(3);
        counter.Value().Add(3);
        progress.Flush();
        CHECK(task.Finish(ProgressOutcome::Succeeded));
    }

    progress.Stop();
    progress.Stop();
    CHECK(collected.HasEvent("begin", "title", "Compiling things"));
    CHECK(collected.HasFinalCount(3));
    CHECK(collected.HasEvent("end", "outcome", "succeeded"));
    // Stop이 hub를 껐다 — 다시 켤 수 있다.
    REQUIRE(ProgressHub::Get().Initialize(allocator, {}));
    ProgressHub::Get().Shutdown();
}

// 이미 켜진 hub는 그대로 쓰고 끄지 않는다. --parent-control의 cancel 줄은 최상위 작업을 취소한다.
TEST(Progress_CommandProgressUsesTheRunningHubAndParentControl) {
    SystemAllocator allocator;
    REQUIRE(ProgressHub::Get().Initialize(allocator, {}));
    {
        auto begun = BeginCommandTask(allocator, "Child command");
        REQUIRE(begun);
        ProgressTaskHandle task = Move(begun.Value());
        CancelInput input;
        CommandProgressOptions options;
        options.mode = CommandProgressMode::Off;
        options.parentControl = true;
        CommandProgressDesc desc;
        desc.controlRead = &ReadCancel;
        desc.controlContext = &input;
        CommandProgress progress;
        REQUIRE(progress.Start(allocator, options, desc));
        CHECK(!progress.OwnsHub());
        // cancel 줄을 준 뒤 다음 읽기가 오면 그 줄은 처리되었다(같은 수신 스레드).
        input.delivered.Wait();
        CHECK(task.Token().IsCancellationRequested());
        CHECK(task.Finish(ProgressOutcome::Cancelled));
        progress.Stop();
    }

    // 연결이 끄지 않았다: 켜져 있으므로 다시 켜기는 AlreadyExists다.
    CHECK(ProgressHub::Get().Initialize(allocator, {}).Error() == ErrorCode::AlreadyExists);
    ProgressHub::Get().Shutdown();
}

} // namespace command_progress_tests_file
