#include "ShaderManagerTestSupport.h"

#include <Core/CancelToken.h>
#include <Core/Text/Json.h>
#include <Core/Text/String.h>
#include <Localization/Text.h>
#include <Platform/Atomics.h>
#include <Platform/Process.h>
#include <Platform/Sync.h>
#include <Progress/ProgressCounter.h>
#include <Progress/ProgressHub.h>
#include <Progress/ProgressSnapshot.h>
#include <ShaderCompiler/ShaderCompileCommandlet.h>
#include <ShaderCompiler/ShaderCompileManager.h>
#include <ShaderCompiler/ShaderWorkerChannel.h>
#include <Base/InitializerList.h>
#include <TestSupport/Test.h>
#include <stdio.h>
#include <string.h>

// 셰이더 컴파일 진행·취소 시험이다(Docs/Design/ShaderSystem 11.1절 PG5 메모, 진행 보고 14장 29–31):
// sink 합계(더한 수 = 끝난 수 = job 수 — 실패 job·잠정 완료 포함)와 ProgressCounter 연결, 시작 전
// 취소, 진행 중 묶음 취소(held hook이 token을 취소하고 Keep — 워커 강제 종료와 대기 묶음), 명령의
// JSONL 출력과 취소 종료 코드 130. 순서는 hook·hub 취소 대기로 정한다(시간에 기대지 않는다). 워커가
// 없거나 도구 저장소가 준비되지 않았으면 그 시험만 SKIP이다.
using namespace rocks;

namespace shader_compile_progress_tests_file {

using namespace rocks::shader;
using namespace rocks::shader_source_test;
using namespace rocks::shader_manager_test;

namespace {

// 프로세스 종료 기다림의 실패 판정 상한이다(정상 경로는 OS 대기로 바로 끝난다).
constexpr uint32 ExitWaitMilliseconds = 60'000;

// sink가 받은 수다(채널 스레드도 부르므로 원자).
struct SinkTotals {
    platform::Atomic<uint64> added{0};
    platform::Atomic<uint64> finished{0};
    platform::Atomic<uint64> addedCalls{0};

    static void Added(void* context, uint64 jobs) noexcept {
        auto& self = *static_cast<SinkTotals*>(context);
        (void)self.added.FetchAdd<platform::MemoryOrder::AcquireRelease>(jobs);
        (void)self.addedCalls.FetchAdd<platform::MemoryOrder::AcquireRelease>(1u);
    }

    static void Finished(void* context, uint64 jobs) noexcept {
        (void)static_cast<SinkTotals*>(context)
            ->finished.FetchAdd<platform::MemoryOrder::AcquireRelease>(jobs);
    }

    ShaderCompileProgressSink Sink() noexcept {
        ShaderCompileProgressSink sink;
        sink.added = &Added;
        sink.finished = &Finished;
        sink.context = this;
        return sink;
    }

    uint64 Added() const noexcept {
        return added.Load<platform::MemoryOrder::Acquire>();
    }

    uint64 Finished() const noexcept {
        return finished.Load<platform::MemoryOrder::Acquire>();
    }
};

// held hook: 묶음의 token을 취소하고 워커를 막힌 채 두게 한다(채널이 다음 읽기 시간 초과에서
// 취소를 보고 워커를 강제 종료한다). 워커 PID를 남긴다.
struct HeldCancel {
    CancelSource* source = nullptr;
    platform::Atomic<uint32> worker{0};
    platform::Atomic<uint32> calls{0};
};

ShaderWorkerHeldAction CancelAndKeep(void* context, uint32, uint32 workerProcessId,
                                     Span<const uint32>) noexcept {
    auto& held = *static_cast<HeldCancel*>(context);
    held.worker.Store<platform::MemoryOrder::Release>(workerProcessId);
    (void)held.calls.FetchAdd<platform::MemoryOrder::AcquireRelease>(1u);
    held.source->Cancel();
    return ShaderWorkerHeldAction::Keep;
}

// 명령 출력을 모은다(JSONL은 펌프 스레드가 Output으로, 결과 줄은 명령 스레드가 Error로 쓴다 —
// 잠금).
struct Captured {
    Captured() noexcept : out(TestAllocator()), error(TestAllocator()) {
    }

    static Result<void> Write(void* context, CommandletStream stream, StringView text) noexcept {
        auto& self = *static_cast<Captured*>(context);
        platform::LockGuard lock(self.gate);
        return (stream == CommandletStream::Output ? self.out : self.error).TryAppend(text);
    }

    platform::Mutex gate;
    String out;
    String error;
};

Result<int32> RunCommand(Captured& captured, ShaderCompileCommandContext& context,
                         std::initializer_list<StringView> args) noexcept {
    CommandletOutput output;
    output.write = &Captured::Write;
    output.context = &captured;
    return RunShaderCompileCommand(&context, Span<const StringView>(args.begin(), args.size()),
                                   output);
}

// 표준 출력의 JSONL 줄을 검사한다: 줄마다 v = 1 객체이고, e별 칸을 모은다.
struct JsonlSummary {
    usize lines = 0;
    bool allVersioned = true;
    bool begun = false;
    bool finalCount = false;
    char endOutcome[16] = {};
};

JsonlSummary ReadJsonl(const String& text, uint64 jobs) noexcept {
    JsonlSummary summary;
    StringView rest = text.View();
    while (!rest.IsEmpty()) {
        StringView line;
        StringView after;
        if (!rest.Split("\n", line, after, StringComparison::CaseSensitive)) {
            line = rest;
            after = StringView();
        }

        rest = after;
        if (line.IsEmpty()) {
            continue;
        }

        ++summary.lines;
        JsonDocument document(TestAllocator());
        if (!document.Parse(line).Succeeded()) {
            summary.allVersioned = false;
            continue;
        }

        const JsonValue root = document.Root();
        auto version = root.Find("v").TryGetUInt64();
        const JsonValue event = root.Find("e");
        if (!version || version.Value() != 1 || !event.IsValid() || !event.IsString()) {
            summary.allVersioned = false;
            continue;
        }

        const StringView kind = event.GetString();
        const JsonValue title = root.Find("title");
        if (kind == "begin" && title.IsValid() && title.IsString() &&
            title.GetString() == "Compiling shaders") {
            summary.begun = true;
        }

        const JsonValue shape = root.Find("shape");
        auto done = root.Find("done").TryGetUInt64();
        auto total = root.Find("total").TryGetUInt64();
        if (kind == "progress" && shape.IsValid() && shape.IsString() &&
            shape.GetString() == "count" && done && total && done.Value() == jobs &&
            total.Value() == jobs) {
            summary.finalCount = true;
        }

        const JsonValue outcome = root.Find("outcome");
        if (kind == "end" && outcome.IsValid() && outcome.IsString()) {
            snprintf(summary.endOutcome, sizeof(summary.endOutcome), "%.*s",
                     static_cast<int>(outcome.GetString().ByteCount()), outcome.GetString().Data());
        }
    }

    return summary;
}

bool Contains(const String& text, const char* part) noexcept {
    return strstr(text.Data(), part) != nullptr;
}

} // namespace

// sink는 호출마다 added 한 번(job 수)이고 끝난 수 합이 job 수다 — 실패 job·묶음 하나짜리(잠정 완료
// 줄 정산) 모두. ProgressCounter sink는 작업 계수를 done = total로 채운다.
TEST(ShaderCompileProgress_SinksCountEveryJob) {
    ManagerCase test;
    const char* skip = nullptr;
    if (!test.Make("progresssinks", skip)) {
        if (skip != nullptr) {
            printf("[SKIP] ShaderCompileProgress_SinksCountEveryJob: %s\n", skip);
        }

        return;
    }

    Array<ShaderCompileJob> jobs(TestAllocator());
    test.Jobs(jobs);
    ShaderCompileJob broken = jobs[0];
    broken.entryPoint = "NoSuchEntry";
    REQUIRE(jobs.TryPushBack(broken));
    ShaderCompileManagerConfig config = test.Config("Work");
    config.workerCount = 2;
    config.maxBatchJobs = 1;
    ShaderCompileManager manager(config);
    REQUIRE(manager.Start());
    SinkTotals totals;
    SinkTotals second;
    const ShaderCompileProgressSink sinks[] = {totals.Sink(), second.Sink()};
    ShaderCompileControl control;
    control.sinks = Span<const ShaderCompileProgressSink>(sinks);
    Array<ShaderCompileJobResult> results(TestAllocator());
    REQUIRE(manager.Compile(jobs.View(), results, control));
    CHECK(results[jobs.Size() - 1].status == ShaderCompileStatus::CompileError);
    CHECK(totals.Added() == jobs.Size() && totals.Finished() == jobs.Size());
    CHECK(totals.addedCalls.Load<platform::MemoryOrder::Acquire>() == 1u);
    CHECK(second.Added() == jobs.Size() && second.Finished() == jobs.Size());
    const ShaderCompileProgress progress = manager.Progress();
    CHECK(progress.cancelled == 0 && progress.failed == 1 &&
          progress.completed == jobs.Size() - 1 && progress.provisional == 0);

    // 작업 수준 계수로 잇는다(hub 스냅샷의 완료/총량).
    REQUIRE(ProgressHub::Get().Initialize(TestAllocator(), {}));
    {
        auto title = Text::Invariant(TestAllocator(), "Compiling shaders");
        REQUIRE(title);
        ProgressTaskDesc desc;
        desc.title = &title.Value();
        auto task = ProgressHub::Get().Begin(desc);
        REQUIRE(task);
        auto counter = ProgressCounter::ForTask(task.Value(), 0);
        REQUIRE(counter);
        const ShaderCompileProgressSink counted[] = {MakeShaderCompileCounterSink(counter.Value())};
        ShaderCompileControl countedControl;
        countedControl.sinks = Span<const ShaderCompileProgressSink>(counted);
        REQUIRE(manager.Compile(jobs.View(), results, countedControl));
        ProgressSnapshot snapshot(TestAllocator());
        uint64 cursor = 0;
        REQUIRE(ProgressHub::Get().Snapshot(snapshot, cursor));
        bool seen = false;
        for (const ProgressTaskView& view : snapshot.tasks) {
            if (view.id == task.Value().Id()) {
                seen = true;
                CHECK(view.shape == ProgressShape::Count);
                CHECK(view.total == jobs.Size() && view.done == jobs.Size());
            }
        }

        CHECK(seen);
        CHECK(task.Value().Finish(ProgressOutcome::Succeeded));
    }

    ProgressHub::Get().Shutdown();
}

// 시작 전 취소: 워커를 띄우지 않고 모든 job이 Cancelled, Compile은 Cancelled, sink 합계는 그대로다.
TEST(ShaderCompileProgress_CancelBeforeStart) {
    ManagerCase test;
    const char* skip = nullptr;
    if (!test.Make("progresscancelfirst", skip)) {
        if (skip != nullptr) {
            printf("[SKIP] ShaderCompileProgress_CancelBeforeStart: %s\n", skip);
        }

        return;
    }

    Array<ShaderCompileJob> jobs(TestAllocator());
    test.Jobs(jobs);
    ShaderCompileManager manager(test.Config("Work"));
    REQUIRE(manager.Start());
    auto source = CancelSource::Create(TestAllocator());
    REQUIRE(source);
    source.Value().Cancel();
    SinkTotals totals;
    const ShaderCompileProgressSink sinks[] = {totals.Sink()};
    ShaderCompileControl control;
    control.cancel = source.Value().Token();
    control.sinks = Span<const ShaderCompileProgressSink>(sinks);
    Array<ShaderCompileJobResult> results(TestAllocator());
    const Result<void> compiled = manager.Compile(jobs.View(), results, control);
    CHECK(!compiled && compiled.Error() == ErrorCode::Cancelled);
    REQUIRE(results.Size() == jobs.Size());
    for (const ShaderCompileJobResult& result : results) {
        CHECK(result.status == ShaderCompileStatus::Cancelled);
    }

    CHECK(totals.Added() == jobs.Size() && totals.Finished() == jobs.Size());
    const ShaderCompileProgress progress = manager.Progress();
    CHECK(progress.cancelled == jobs.Size() && progress.failed == 0 &&
          progress.workersStarted == 0 && progress.batches == 0);
}

// 진행 중 묶음 취소: 첫 묶음(Hold)의 held hook이 token을 취소하고 Keep을 고르면 채널이 워커를 강제
// 종료하고(그 워커 프로세스가 끝난다) 묶음이 Cancelled이며, 뒤 묶음은 워커 없이 Cancelled다.
TEST(ShaderCompileProgress_CancelRunningBatch) {
    ManagerCase test;
    const char* skip = nullptr;
    if (!test.Make("progresscancelrunning", skip)) {
        if (skip != nullptr) {
            printf("[SKIP] ShaderCompileProgress_CancelRunningBatch: %s\n", skip);
        }

        return;
    }

    Array<ShaderCompileJob> jobs(TestAllocator());
    test.Jobs(jobs);
    jobs[0].testHook = ShaderWorkerJobKind::Hold;
    auto source = CancelSource::Create(TestAllocator());
    REQUIRE(source);
    HeldCancel held;
    held.source = &source.Value();
    ShaderCompileManagerConfig config = test.Config("Work");
    config.workerCount = 1;
    config.maxBatchJobs = 1;
    config.testHooks = true;
    config.channelHooks.held = &CancelAndKeep;
    config.channelHooks.context = &held;
    ShaderCompileManager manager(config);
    REQUIRE(manager.Start());
    SinkTotals totals;
    const ShaderCompileProgressSink sinks[] = {totals.Sink()};
    ShaderCompileControl control;
    control.cancel = source.Value().Token();
    control.sinks = Span<const ShaderCompileProgressSink>(sinks);
    Array<ShaderCompileJobResult> results(TestAllocator());
    const Result<void> compiled = manager.Compile(jobs.View(), results, control);
    CHECK(!compiled && compiled.Error() == ErrorCode::Cancelled);
    REQUIRE(results.Size() == jobs.Size());
    for (const ShaderCompileJobResult& result : results) {
        CHECK(result.status == ShaderCompileStatus::Cancelled);
    }

    CHECK(held.calls.Load<platform::MemoryOrder::Acquire>() == 1u);
    const uint32 worker = held.worker.Load<platform::MemoryOrder::Acquire>();
    REQUIRE(worker != 0);
    // 강제 종료한 워커는 끝났다(남은 워커 없음).
    CHECK(platform::WaitForProcessExit(worker, ExitWaitMilliseconds) ==
          platform::WaitResult::Signaled);
    CHECK(totals.Added() == jobs.Size() && totals.Finished() == jobs.Size());
    const ShaderCompileProgress progress = manager.Progress();
    CHECK(progress.cancelled == jobs.Size() && progress.workersStarted == 1 &&
          progress.batches == jobs.Size() && progress.provisional == 0);

    // 취소되지 않은 다음 Compile은 새 워커로 끝난다.
    Array<ShaderCompileJobResult> again(TestAllocator());
    REQUIRE(manager.Compile(Span<const ShaderCompileJob>(jobs.Data() + 1, 1), again));
    CHECK(again[0].status == ShaderCompileStatus::Success);
    CHECK(manager.Progress().workersStarted == 2);
}

// shader-compile --progress=jsonl: 표준 출력은 모두 JSONL v1이고 begin·마지막 계수·end succeeded가
// 있으며 결과 줄은 표준 오류다. hub 취소 대기 뒤 실행하면 종료 코드 130·end cancelled다.
TEST(ShaderCompileProgress_CommandWritesJsonlAndCancels) {
    ManagerCase test;
    const char* skip = nullptr;
    if (!test.Make("progresscommand", skip)) {
        if (skip != nullptr) {
            printf("[SKIP] ShaderCompileProgress_CommandWritesJsonlAndCancels: %s\n", skip);
        }

        return;
    }

    char project[ManagerPathBytes];
    char projectOption[ManagerPathBytes + 16];
    REQUIRE(test.fixture.Child("Project", project, sizeof(project)));
    snprintf(projectOption, sizeof(projectOption), "--project=%s", project);
    ShaderCompileCommandContext context;
    context.allocator = &TestAllocator();
    context.workerExecutable = StringView(test.worker);
    const StringView compute = "/Project/Test.rsf:CSMain:compute";
    const StringView pixel = "/Project/Test.rsf:PSMain:pixel";
    {
        Captured captured;
        auto code = RunCommand(captured, context,
                               {StringView(projectOption), "--format=D3D12_SM6", "--no-cache",
                                "--define=SCALE=2", "--progress=jsonl", compute, pixel});
        REQUIRE(code);
        CHECK(code.Value() == 0);
        const JsonlSummary summary = ReadJsonl(captured.out, 2);
        CHECK(summary.lines >= 3 && summary.allVersioned && summary.begun && summary.finalCount);
        CHECK(strcmp(summary.endOutcome, "succeeded") == 0);
        CHECK(Contains(captured.error, "shader-compile: D3D12_SM6 /Project/Test.rsf:CSMain"));
        CHECK(Contains(captured.error, "cancelled 0 cache-hits 0"));
        if (!summary.finalCount || summary.endOutcome[0] == 0) {
            fprintf(stderr, "stdout:\n%s\nstderr:\n%s\n", captured.out.Data(),
                    captured.error.Data());
        }
    }

    // 둘째 모드 인자는 사용법 오류다.
    {
        Captured captured;
        auto code =
            RunCommand(captured, context,
                       {"--progress=jsonl", "--no-progress", "--format=D3D12_SM6", compute});
        REQUIRE(code);
        CHECK(code.Value() == 2);
    }

    // 취소 대기(진행 보고 7.4절): 명령이 쓰는 켜진 hub에 전체 취소를 먼저 건다 — 명령의 작업은
    // 취소된 채 시작한다(부모 cancel 줄·Ctrl-C가 첫 Begin보다 먼저 온 경우와 같다).
    REQUIRE(ProgressHub::Get().Initialize(TestAllocator(), {}));
    ProgressHub::Get().RequestCancelAllTopLevel();
    {
        Captured captured;
        auto code = RunCommand(captured, context,
                               {StringView(projectOption), "--format=D3D12_SM6", "--no-cache",
                                "--define=SCALE=2", "--progress=jsonl", compute, pixel});
        REQUIRE(code);
        CHECK(code.Value() == ProgressCancelledExitCode);
        const JsonlSummary summary = ReadJsonl(captured.out, 2);
        CHECK(summary.allVersioned && summary.begun);
        CHECK(strcmp(summary.endOutcome, "cancelled") == 0);
        CHECK(Contains(captured.error, " miss Cancelled"));
        CHECK(Contains(captured.error, "cancelled 2 cache-hits 0 workers 0"));
    }

    ProgressHub::Get().Shutdown();
}

} // namespace shader_compile_progress_tests_file
