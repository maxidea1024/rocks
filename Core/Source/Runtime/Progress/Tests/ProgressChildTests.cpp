#include <Core/Memory/SystemAllocator.h>
#include <Core/Move.h>
#include <Core/Text/StringBuilder.h>
#include <Localization/Catalog.h>
#include <Localization/Text.h>
#include <Platform/Process.h>
#include <Platform/StandardStream.h>
#include <Platform/Thread.h>
#include <Progress/ChildProgressReader.h>
#include <Progress/ParentControlListener.h>
#include <Progress/ProgressCounter.h>
#include <Progress/ProgressHub.h>
#include <Progress/ProgressJsonlWriter.h>
#include <TestSupport/Test.h>
#include <string.h>

namespace rocks::test {
bool RegisterProcessProbe(const char* mode,
                          int (*function)(int, const char* const*) noexcept) noexcept;
} // namespace rocks::test

using namespace rocks;
namespace progress_child_tests_file {

// 프로세스 경계 시험(Docs/Design/ProgressReporting 14장 18–21, 10장). 줄 처리는 FeedLine으로 바로
// 보고, 실제 자식은 시험 실행 파일을 "--process-probe progress-child <시나리오>"로 띄운다. 자식과의
// 순서는 줄(자식이 쓴 begin 뒤에 부모가 움직임)과 pipe EOF로 정한다 — 시간에 기대지 않는다.

// ---- 자식(probe) ----

constexpr uint64 ChildWorkTotal = 4;
constexpr uint32 ChildWaitMilliseconds = 1;

void ChildFlush(ProgressJsonlWriter& jsonl) noexcept {
    (void)jsonl.Flush(0);
}

ProgressTaskHandle ChildBegin(Allocator& allocator, StringView title, ProgressTaskId parent,
                              ProgressCancel cancel) noexcept {
    auto text = Text::Invariant(allocator, title);
    if (!text) {
        return {};
    }

    ProgressTaskDesc desc;
    desc.title = &text.Value();
    desc.parent = parent;
    desc.cancel = cancel;
    auto task = ProgressHub::Get().Begin(desc);
    return task ? Move(task.Value()) : ProgressTaskHandle();
}

// 시나리오:
//   work         최상위 작업·하위 작업·계수 4 → Succeeded, 종료 0
//   wait-cancel  --parent-control 수신, 취소될 때까지 기다림 → Cancelled, 종료 130
//   ignore-cancel 수신하지만 취소를 무시하고 끝나지 않음(부모 강제 종료 경로)
//   crash        begin 뒤 end 없이 종료 3
//   no-end       begin 뒤 end 없이 종료 0
//   no-control   수신 없이(stdin을 보지 않음) 끝까지 → Succeeded, 종료 0
//   bad-version  다른 판의 줄을 쓴 뒤 정상 진행 → 종료 0
int ProgressChildProbe(int argc, const char* const* argv) noexcept {
    if (argc < 4) {
        return 90;
    }

    const StringView scenario(argv[3]);
    SystemAllocator allocator;
    if (!ProgressHub::Get().Initialize(allocator, {})) {
        return 91;
    }

    int code = 0;
    {
        ProgressJsonlWriter jsonl(allocator);
        if (scenario == StringView("bad-version")) {
            const char line[] = "{\"v\":2,\"e\":\"begin\",\"id\":1}\n";
            (void)platform::WriteStandardStream(platform::StandardStream::Output, line,
                                                sizeof(line) - 1);
        }

        ProgressTaskHandle work =
            ChildBegin(allocator, "Child work", NoProgressTask, ProgressCancel::UserCancellable);
        ChildFlush(jsonl);
        ParentControlListener control;
        const bool listens =
            scenario == StringView("wait-cancel") || scenario == StringView("ignore-cancel");
        if (listens && !control.Start(allocator)) {
            return 92;
        }

        // crash·no-end는 end 줄 없이 끝난다(중계는 아래에서 닫는다 — 시험 allocator 누수 검사).
        if (scenario == StringView("crash")) {
            code = 3;
        } else if (scenario == StringView("no-end")) {
            code = 0;
        } else if (scenario == StringView("wait-cancel")) {
            while (!work.Token().IsCancellationRequested()) {
                platform::SleepFor(ChildWaitMilliseconds);
            }

            (void)work.Finish(ProgressOutcome::Cancelled);
            ChildFlush(jsonl);
            control.Stop();
            code = ProgressCancelledExitCode;
        } else if (scenario == StringView("ignore-cancel")) {
            for (;;) {
                platform::SleepFor(ChildWaitMilliseconds);
            }
        } else {
            auto counter = ProgressCounter::ForTask(work, ChildWorkTotal);
            if (!counter) {
                return 93;
            }

            counter.Value().Add(2);
            ProgressTaskHandle step =
                ChildBegin(allocator, "Child step", work.Id(), ProgressCancel::NotCancellable);
            ChildFlush(jsonl);
            (void)step.Finish(ProgressOutcome::Succeeded);
            counter.Value().Add(2);
            ChildFlush(jsonl);
            (void)work.Finish(ProgressOutcome::Succeeded);
            ChildFlush(jsonl);
        }
    }

    ProgressHub::Get().Shutdown();
    return code;
}

const bool ChildProbeRegistered =
    rocks::test::RegisterProcessProbe("progress-child", &ProgressChildProbe);

// ---- 부모 ----

struct Hub {
    SystemAllocator allocator;
    bool ready = false;

    Hub() noexcept {
        ready = static_cast<bool>(ProgressHub::Get().Initialize(allocator, {}));
    }

    ~Hub() noexcept {
        ProgressHub::Get().Shutdown();
    }

    ProgressTaskHandle Begin(StringView title,
                             ProgressCancel cancel = ProgressCancel::NotCancellable) noexcept {
        return ChildBegin(allocator, title, NoProgressTask, cancel);
    }
};

// 사건 기록에서 부모 작업 아래로 만들어진 하위 작업들의 끝을 모은다.
struct Ends {
    uint32 begins = 0;
    uint32 succeeded = 0;
    uint32 failed = 0;
    uint32 cancelled = 0;
    uint32 abandoned = 0;
};

Ends ReadEnds(Allocator& allocator, uint64& cursor) noexcept {
    Ends ends;
    ProgressSnapshot snapshot(allocator);
    if (!ProgressHub::Get().Snapshot(snapshot, cursor)) {
        return ends;
    }

    for (const ProgressEvent& event : snapshot.events) {
        if (event.kind == ProgressEventKind::Begin) {
            ++ends.begins;
            continue;
        }

        switch (event.outcome) {
        case ProgressOutcome::Succeeded:
            ++ends.succeeded;
            break;
        case ProgressOutcome::Failed:
            ++ends.failed;
            break;
        case ProgressOutcome::Cancelled:
            ++ends.cancelled;
            break;
        case ProgressOutcome::Abandoned:
            ++ends.abandoned;
            break;
        }
    }

    return ends;
}

// 시험 18(줄 처리): begin·하위 작업·progress·end, 깨진 줄은 무시, 끝난 작업 아래는 함께 닫힘.
TEST(Progress_ChildReaderMirrorsLines) {
    Hub hub;
    REQUIRE(hub.ready);
    ProgressTaskHandle parent = hub.Begin("Parent");
    ChildProgressReaderDesc desc;
    desc.parent = parent.Id();
    ChildProgressReader reader(hub.allocator, desc);
    uint64 cursor = 0;
    (void)ReadEnds(hub.allocator, cursor);
    REQUIRE(reader.FeedLine("{\"v\":1,\"e\":\"begin\",\"id\":7,\"parent\":0,\"title\":\"Cook\","
                            "\"titleId\":\"\",\"titleRev\":0,\"pres\":\"background\","
                            "\"cancel\":true}"));
    REQUIRE(reader.FeedLine("{\"v\":1,\"e\":\"begin\",\"id\":9,\"parent\":7,\"title\":\"Step\","
                            "\"pres\":\"hidden\",\"cancel\":false}"));
    REQUIRE(reader.FeedLine("{\"v\":1,\"e\":\"progress\",\"id\":7,\"shape\":\"count\","
                            "\"frac\":0.5,\"done\":2,\"total\":4,\"msg\":\"Maps/Arena\","
                            "\"msgId\":\"\",\"msgRev\":0,\"cancelRequested\":false}"));
    CHECK(reader.FeedLine("{not json").Error() == ErrorCode::CorruptData);
    CHECK(reader.FeedLine("{\"v\":1,\"e\":\"mystery\"}").Error() == ErrorCode::CorruptData);
    CHECK(reader.LiveSubtasks() == 2);
    {
        ProgressSnapshot snapshot(hub.allocator);
        uint64 viewCursor = 0;
        REQUIRE(ProgressHub::Get().Snapshot(snapshot, viewCursor));
        REQUIRE(snapshot.tasks.Size() == 3);
        const ProgressTaskView& cook = snapshot.tasks[1];
        CHECK(cook.parent == parent.Id());
        CHECK(cook.shape == ProgressShape::Count);
        CHECK(cook.done == 2);
        CHECK(cook.total == 4);
        CHECK(!cook.cancellable);
        CHECK(snapshot.tasks[2].parent == cook.id);
        CHECK(snapshot.tasks[2].presentation == ProgressPresentation::Hidden);
        auto message = ResolveText(hub.allocator, cook.taskMessage, CatalogSnapshot());
        REQUIRE(message);
        CHECK(message.Value().value.View() == StringView("Maps/Arena"));
    }

    REQUIRE(reader.FeedLine("{\"v\":1,\"e\":\"end\",\"id\":7,\"outcome\":\"succeeded\","
                            "\"detail\":\"\",\"detailId\":\"\",\"detailRev\":0}"));
    CHECK(reader.LiveSubtasks() == 0);
    const Ends ends = ReadEnds(hub.allocator, cursor);
    CHECK(ends.begins == 2);
    CHECK(ends.succeeded == 1);
    CHECK(ends.abandoned == 1);
    CHECK(parent.Finish(ProgressOutcome::Succeeded));
}

// 시험 18: 버전 불일치는 Unsupported로 그 뒤 줄을 끊고(남은 하위 작업 Abandoned), 대응표·hub 상한을
// 넘는 하위 작업은 만들지 않으며, Finish는 비정상 종료면 Failed·정상 종료면 Abandoned로 닫는다.
TEST(Progress_ChildReaderVersionLimitsAndFinish) {
    Hub hub;
    REQUIRE(hub.ready);
    ProgressTaskHandle parent = hub.Begin("Parent");
    uint64 cursor = 0;
    (void)ReadEnds(hub.allocator, cursor);
    {
        ChildProgressReaderDesc desc;
        desc.parent = parent.Id();
        ChildProgressReader reader(hub.allocator, desc);
        REQUIRE(reader.FeedLine("{\"v\":1,\"e\":\"begin\",\"id\":1,\"parent\":0,\"title\":\"A\"}"));
        CHECK(reader.FeedLine("{\"v\":2,\"e\":\"begin\",\"id\":2,\"parent\":0}").Error() ==
              ErrorCode::Unsupported);
        CHECK(reader.LiveSubtasks() == 0);
        CHECK(reader.FeedLine("{\"v\":1,\"e\":\"begin\",\"id\":3,\"parent\":0,\"title\":\"B\"}")
                  .Error() == ErrorCode::Unsupported);
    }

    Ends ends = ReadEnds(hub.allocator, cursor);
    CHECK(ends.begins == 1);
    CHECK(ends.abandoned == 1);
    {
        ChildProgressReaderDesc desc;
        desc.parent = parent.Id();
        ChildProgressReader reader(hub.allocator, desc);
        REQUIRE(reader.FeedLine("{\"v\":1,\"e\":\"begin\",\"id\":1,\"parent\":0,\"title\":\"A\"}"));
        reader.Finish(false);
        CHECK(reader.LiveSubtasks() == 0);
    }

    ends = ReadEnds(hub.allocator, cursor);
    CHECK(ends.failed == 1);
    {
        // hub에 parent가 이미 하나 있으므로 하위 작업은 MaxLiveProgressTasks - 1개까지다.
        ChildProgressReaderDesc desc;
        desc.parent = parent.Id();
        ChildProgressReader reader(hub.allocator, desc);
        StringBuilder<128> line(hub.allocator);
        ErrorCode last = ErrorCode::None;
        for (uint32 id = 1; id <= MaxChildProgressTasks; ++id) {
            line.Clear();
            REQUIRE(line.TryAppendFormat(
                "{{\"v\":1,\"e\":\"begin\",\"id\":{},\"parent\":0,\"title\":\"T\"}}", id));
            auto fed = reader.FeedLine(line.View());
            last = fed ? ErrorCode::None : fed.Error();
        }

        CHECK(last == ErrorCode::CapacityExceeded);
        CHECK(reader.LiveSubtasks() == MaxChildProgressTasks - 1);
    }

    CHECK(parent.Finish(ProgressOutcome::Succeeded));
}

// 실제 자식을 띄운다.
struct ChildProcess {
    char path[4096]{};
    usize pathBytes = 0;
    platform::ProcessArgument arguments[3]{};
    platform::Process process;

    bool Start(const char* scenario, platform::ProcessStreamMode input) noexcept {
        if (!platform::GetExecutablePath(path, sizeof(path), pathBytes)) {
            return false;
        }

        arguments[0] = {"--process-probe", 15};
        arguments[1] = {"progress-child", 14};
        arguments[2] = {scenario, strlen(scenario)};
        platform::ProcessStartInfo info;
        info.executable = path;
        info.executableBytes = pathBytes;
        info.arguments = arguments;
        info.argumentCount = 3;
        info.input = input;
        info.output = platform::ProcessStreamMode::Pipe;
        info.error = platform::ProcessStreamMode::Null;
        return static_cast<bool>(process.Start(info));
    }
};

struct ChildRun {
    bool started = false;
    bool ran = false;
    platform::ProcessExit exit;
    Ends ends;
};

// scenario 자식을 띄워 parent 아래로 읽는다. cancelFirst면 읽기 전에 부모 작업을 취소해 둔다(자식은
// begin 뒤에 수신을 켜므로 cancel 줄은 pipe에 머물다 그때 읽힌다). closeInput이면 읽기 전에 입력을
// 닫는다.
ChildRun RunChild(Hub& hub, const char* scenario, platform::ProcessStreamMode input,
                  bool cancelFirst, bool closeInput, uint32 graceMilliseconds,
                  bool& terminated) noexcept {
    ChildRun run;
    ProgressTaskHandle parent = hub.Begin("Parent", ProgressCancel::UserCancellable);
    uint64 cursor = 0;
    (void)ReadEnds(hub.allocator, cursor);
    ChildProcess child;
    run.started = child.Start(scenario, input);
    if (!run.started) {
        return run;
    }

    ChildProgressReaderDesc desc;
    desc.parent = parent.Id();
    desc.cancel = parent.Token();
    desc.cancelGraceMilliseconds = graceMilliseconds;
    ChildProgressReader reader(hub.allocator, desc);
    if (cancelFirst) {
        // 자식의 begin 줄(하위 작업)을 받은 뒤에 취소한다 — 순서는 줄이 정한다(자식 시작 시간과
        // 무관).
        char chunk[256];
        while (reader.LiveSubtasks() == 0) {
            const platform::FileTransfer read =
                child.process.Read(platform::ProcessStream::Output, chunk, sizeof(chunk));
            if (!read.status) {
                break;
            }

            reader.FeedBytes(chunk, read.bytes);
        }

        (void)ProgressHub::Get().RequestCancel(parent.Id());
    }

    if (closeInput) {
        (void)child.process.CloseInput();
    }

    auto exit = reader.Run(child.process);
    run.ran = static_cast<bool>(exit);
    if (exit) {
        run.exit = exit.Value();
    }

    terminated = reader.Terminated();
    (void)parent.Finish(ProgressOutcome::Succeeded);
    run.ends = ReadEnds(hub.allocator, cursor);
    return run;
}

// 시험 18: 정상 왕복(자식의 최상위·하위 작업이 부모 아래로, Succeeded·종료 0), 비정상 종료 →
// Failed, end 없는 정상 종료 → Abandoned, 다른 판의 줄 → 진행만 끊고 자식은 계속.
TEST(Progress_ChildProcessRoundTrips) {
    REQUIRE(ChildProbeRegistered);
    Hub hub;
    REQUIRE(hub.ready);
    bool terminated = false;
    const ChildRun work =
        RunChild(hub, "work", platform::ProcessStreamMode::Null, false, false, 0, terminated);
    REQUIRE(work.started);
    REQUIRE(work.ran);
    CHECK(work.exit.kind == platform::ProcessExitKind::Exited && work.exit.code == 0);
    // 부모(시험 쪽) 하나 + 자식 둘이 시작했고, 자식 둘·부모가 Succeeded로 끝났다.
    CHECK(work.ends.begins == 2);
    CHECK(work.ends.succeeded == 3);

    const ChildRun crash =
        RunChild(hub, "crash", platform::ProcessStreamMode::Null, false, false, 0, terminated);
    REQUIRE(crash.ran);
    CHECK(crash.exit.code == 3);
    CHECK(crash.ends.failed == 1);

    const ChildRun noEnd =
        RunChild(hub, "no-end", platform::ProcessStreamMode::Null, false, false, 0, terminated);
    REQUIRE(noEnd.ran);
    CHECK(noEnd.exit.code == 0);
    CHECK(noEnd.ends.abandoned == 1);

    const ChildRun badVersion = RunChild(hub, "bad-version", platform::ProcessStreamMode::Null,
                                         false, false, 0, terminated);
    REQUIRE(badVersion.ran);
    CHECK(badVersion.exit.code == 0);
    CHECK(badVersion.ends.begins == 0);
}

// 시험 19: 부모 취소 → cancel 줄 → 자식이 Cancelled로 끝나고 종료 코드 130.
TEST(Progress_ChildProcessCancelLine) {
    REQUIRE(ChildProbeRegistered);
    Hub hub;
    REQUIRE(hub.ready);
    bool terminated = false;
    const ChildRun run = RunChild(hub, "wait-cancel", platform::ProcessStreamMode::Pipe, true,
                                  false, DefaultChildCancelGraceMilliseconds, terminated);
    REQUIRE(run.ran);
    CHECK(!terminated);
    CHECK(run.exit.kind == platform::ProcessExitKind::Exited);
    CHECK(run.exit.code == static_cast<uint32>(ProgressCancelledExitCode));
    CHECK(run.ends.cancelled == 1);
}

// 시험 20: --parent-control 자식은 stdin EOF(부모가 입력을 닫음)를 취소로 보고, 수신하지 않는
// 자식은 닫힌 stdin이어도 끝까지 간다.
TEST(Progress_ChildProcessInputEof) {
    REQUIRE(ChildProbeRegistered);
    Hub hub;
    REQUIRE(hub.ready);
    bool terminated = false;
    const ChildRun eof = RunChild(hub, "wait-cancel", platform::ProcessStreamMode::Pipe, false,
                                  true, DefaultChildCancelGraceMilliseconds, terminated);
    REQUIRE(eof.ran);
    CHECK(eof.exit.code == static_cast<uint32>(ProgressCancelledExitCode));
    CHECK(eof.ends.cancelled == 1);

    const ChildRun closed =
        RunChild(hub, "no-control", platform::ProcessStreamMode::Null, false, false, 0, terminated);
    REQUIRE(closed.ran);
    CHECK(closed.exit.code == 0);
    CHECK(closed.ends.succeeded >= 2);
}

// 시험 21: 자식이 취소를 무시하면 유예 뒤 강제 종료하고 남은 하위 작업은 Failed다.
TEST(Progress_ChildProcessForcedTermination) {
    REQUIRE(ChildProbeRegistered);
    Hub hub;
    REQUIRE(hub.ready);
    // 유예는 자식이 끝나지 않는 시나리오라 결과에 영향이 없다(길이만 시험 시간).
    constexpr uint32 GraceMilliseconds = 200;
    bool terminated = false;
    const ChildRun run = RunChild(hub, "ignore-cancel", platform::ProcessStreamMode::Pipe, true,
                                  false, GraceMilliseconds, terminated);
    REQUIRE(run.ran);
    CHECK(terminated);
    CHECK(run.exit.kind != platform::ProcessExitKind::Exited ||
          run.exit.code != static_cast<uint32>(ProgressCancelledExitCode));
    CHECK(run.ends.failed == 1);
}

// 10.1절: JSONL로 옮긴 작업 트리는 부모 쪽에서 같은 비율로 합성된다 — 가중치(weight·selfWeight)와
// 자기 진행(self)을 함께 보낸다. 자기 보고 없는 작업은 미러에서도 w₀ = 0이다.
TEST(Progress_JsonlRoundTripKeepsComposition) {
    Hub hub;
    REQUIRE(hub.ready);
    const auto text = [&hub](StringView source) noexcept {
        auto made = Text::Invariant(hub.allocator, source);
        return made ? Move(made.Value()) : Text();
    };
    const Text workTitle = text("Work");
    const Text partTitle = text("Part");
    const Text quietTitle = text("Quiet");
    ProgressTaskDesc workDesc;
    workDesc.title = &workTitle;
    workDesc.selfWeight = 3.0;
    auto work = ProgressHub::Get().Begin(workDesc);
    REQUIRE(work);
    REQUIRE(work.Value().SetExternalProgress(ProgressShape::Fraction, 0.5, 0, 0));
    ProgressTaskDesc partDesc;
    partDesc.title = &partTitle;
    partDesc.parent = work.Value().Id();
    partDesc.weightInParent = 2.0;
    auto part = ProgressHub::Get().Begin(partDesc);
    REQUIRE(part);
    auto counter = ProgressCounter::ForTask(part.Value(), 4);
    REQUIRE(counter);
    counter.Value().Add(1);
    ProgressTaskDesc quietDesc;
    quietDesc.title = &quietTitle;
    quietDesc.parent = work.Value().Id();
    auto quiet = ProgressHub::Get().Begin(quietDesc);
    REQUIRE(quiet);

    // 원본 줄을 모은다(이 뒤에 만드는 미러는 줄에 들지 않는다).
    struct Collected {
        ChildProgressReader* reader = nullptr;
    };
    ProgressTaskHandle parent = hub.Begin("Parent");
    ChildProgressReaderDesc readerDesc;
    readerDesc.parent = parent.Id();
    ChildProgressReader reader(hub.allocator, readerDesc);
    Collected collected;
    collected.reader = &reader;
    ProgressJsonlWriterDesc writerDesc;
    writerDesc.write = [](void* context, StringView line) noexcept {
        (void)static_cast<Collected*>(context)->reader->FeedLine(line);
    };
    writerDesc.context = &collected;
    {
        // parent는 줄에 들지만 미러가 그 아래에 하나 더 생길 뿐 비교에는 쓰지 않는다.
        ProgressJsonlWriter jsonl(hub.allocator, writerDesc);
        REQUIRE(jsonl.Flush(0));
    }

    ProgressSnapshot snapshot(hub.allocator);
    uint64 cursor = 0;
    REQUIRE(ProgressHub::Get().Snapshot(snapshot, cursor));
    double original = -1.0;
    double mirrored = -1.0;
    for (const ProgressTaskView& view : snapshot.tasks) {
        if (view.id == work.Value().Id()) {
            original = view.fraction;
        }
    }

    // 미러된 Work는 parent 아래의 "Work" 하위 작업이다.
    for (const ProgressTaskView& view : snapshot.tasks) {
        auto title = ResolveText(hub.allocator, view.title, CatalogSnapshot());
        if (title && title.Value().value.View() == StringView("Work") &&
            view.id != work.Value().Id()) {
            mirrored = view.fraction;
        }
    }

    // (3·0.5 + 2·0.25 + 1·0) / (3 + 2 + 1) = 2/6
    CHECK(original > 0.33 && original < 0.34);
    CHECK(mirrored == original);
    CHECK(quiet.Value().Finish(ProgressOutcome::Succeeded));
    CHECK(part.Value().Finish(ProgressOutcome::Succeeded));
    CHECK(work.Value().Finish(ProgressOutcome::Succeeded));
    reader.Finish(true);
    CHECK(parent.Finish(ProgressOutcome::Succeeded));
}

} // namespace progress_child_tests_file
