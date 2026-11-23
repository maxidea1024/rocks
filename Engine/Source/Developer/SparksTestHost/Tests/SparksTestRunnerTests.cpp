#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/Path.h>
#include <Core/Text/String.h>
#include <Core/Text/StringBuilder.h>
#include <HostConfig/HostSettings.h>
#include <Platform/Fatal.h>
#include <Platform/File.h>
#include <SparksCompiler/Compilation.h>
#include <SparksCompiler/Diagnostic.h>
#include <SparksCompiler/Source.h>
#include <SparksTestHost/TestRunner.h>
#include <SparksVm/BytecodeFile.h>
#include <SparksVm/VmSession.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

// `@test` 실행기 시험이다(Docs/Design/SparksTools.ko.md 9.6절 — LC8d): 결과 종류마다(통과·Assert
// 실패·다른 fault·`try` 실패·엔진 필요·끝나지 않음·하위 실행 fault), 프레임 수, 보고 형식, 시험
// 사이 독립, 모듈 선택, 입력 거부, VM 최상위 진입(CallFunction·Delivery::child), 할당 실패 전수.
using namespace rocks;
using namespace rocks::sparks;
using namespace rocks::sparks::testing;
namespace sparks_test_runner_tests_file {

namespace {

constexpr usize PathCapacity = 4096;

constexpr StringView Helpers = R"(fn Twice(n: int): int {
    return n * 2;
}

fn Checked(n: int): Result<int> {
    if (n < 0) {
        return Error.NotFound;
    }

    return n;
}
)";

constexpr StringView Checks = R"(import Helpers;

@test fn Passes() {
    Assert(Twice(3) == 6, "twice");
}

@test async fn TwoTicks() {
    await NextTick();
    await NextTick();
}

@test async fn FixedOnce() {
    await NextFixedTick();
}

@test async fn WaitsForAll() {
    await All(NextTick(), Delay(0.5, Clock.Game));
    await Race(Delay(100.0, Clock.Game), NextTick());
}

@test fn AssertFails() {
    Assert(Twice(2) == 5, "wrong");
}

@test fn Overflows() {
    var big = 9223372036854775807;
    big += 1;
    Log.Info("${big}");
}

@test async fn FaultsLater() {
    await NextTick();
    let zero = Twice(0);
    Log.Info("${Math.Div(1, zero)}");
}

@test async fn ChildFault() {
    await All(FaultsLater(), TwoTicks());
}

@test fn TryFails() {
    let n = try Checked(-1);
    Assert(n == 0, "unreachable");
}

@test fn UsesEngine() {
    Assert(Actor.CountAll() == 0, "none");
}

@test async fn Forever() {
    while (true) {
        await NextTick();
    }
}

fn NotATest() {
}
)";

// 엔진 기능 거부 종류마다(엔진 struct 기본값·로드 요청)와 그 사이의 통과(거부 이름이 새지 않음),
// 색인 fault다.
constexpr StringView Engine = R"(@test fn MakesStruct() {
    let spawn = SpawnParams(Count: 2, Category: Tag("Game.Spawn"));
    Assert(spawn.Count == 2, "count");
}

@test fn TagOnly() {
    let tag = Tag("Game.Spawn");
    Assert(tag == Tag("Game.Spawn"), "same");
}

@test async fn Loads() {
    let icon: Soft<DataAsset> = Asset("/Game/Icon");
    let request = icon.Load();
    let data = try await request;
    _ = data;
}

@test fn Index() {
    var values = [1];
    values[5] = 2;
    Log.Info("${values.Count}");
}
)";

// 시험의 프레임 상한(Forever가 정확히 이만큼 진행한다).
constexpr uint32 FrameLimit = 50;

bool ReadText(Allocator& allocator, StringView path, String& out) noexcept {
    platform::File file;
    if (!file.Open(platform::PhysicalFileSystem(), path.Data(), path.ByteCount(),
                   platform::FileOpenMode::ReadExisting)) {
        return false;
    }

    uint64 size = 0;
    Array<char> bytes(allocator);
    const bool ok = file.Size(size) && bytes.TryResize(static_cast<usize>(size)) &&
                    (size == 0 || file.ReadExact(bytes.Data(), bytes.Size()).status);
    const bool closed = static_cast<bool>(file.Close());
    return ok && closed && out.TryAssign(StringView(bytes.Data(), bytes.Size()));
}

String EnginePath(Allocator& allocator, StringView relative) noexcept {
    char engine[PathCapacity];
    auto found = FindEngineDirectory(Span<char>(engine, PathCapacity));
    if (!found) {
        CHECK(false);
        return String(allocator);
    }

    auto path = Path::Combine(allocator, StringView(engine, found.Value()), relative);
    if (!path) {
        CHECK(false);
        return String(allocator);
    }

    return Move(path.Value());
}

// 패키지 `Game`(full)과 시험 schema의 컴파일이다.
class Workspace final {
public:
    explicit Workspace(Allocator& allocator) noexcept
        : allocator_(allocator), schema_(allocator), sources(allocator), diagnostics(allocator),
          compilation(allocator, sources) {
    }

    // modules: (모듈 경로, 원문) 쌍. 값은 진단 없이 컴파일했는지다.
    bool Build(Span<const StringView> paths, Span<const StringView> texts) noexcept {
        const String schemaPath =
            EnginePath(allocator_, "Source/Developer/SparksTestHost/Schema/Engine.sparkbind.json");
        if (!ReadText(allocator_, schemaPath.View(), schema_)) {
            return false;
        }

        auto schema = sources.TryAdd("Schema/Engine.sparkbind.json", schema_.View(), diagnostics);
        auto manifest =
            sources.TryAdd("Game/SparksPackage.json5",
                           "{ name: \"Game\", sparks: 1, profile: \"full\" }\n", diagnostics);
        if (!schema || !manifest || !compilation.AddBindingSchema(schema.Value())) {
            return false;
        }

        auto package = compilation.AddPackage(manifest.Value());
        if (!package) {
            return false;
        }

        for (usize at = 0; at < paths.Size(); ++at) {
            StringBuilder<128> display(allocator_);
            display.Append("Game/").Append(paths[at]);
            auto id = sources.TryAdd(display.View(), texts[at], diagnostics);
            if (!id || !compilation.AddModule(package.Value(), id.Value(), paths[at])) {
                return false;
            }
        }

        if (!compilation.Check(diagnostics)) {
            return false;
        }

        diagnostics.Sort(sources);
        if (!diagnostics.IsEmpty()) {
            StringBuilder<4096> text(allocator_);
            static_cast<void>(AppendDiagnostics(text, sources, diagnostics));
            platform::WriteDiagnostic(text.View().Data(), text.View().ByteCount());
            return false;
        }

        return true;
    }

    uint32 Find(StringView path) const noexcept {
        for (uint32 module = 0; module < compilation.ModuleCount(); ++module) {
            if (compilation.ModulePath(module) == path) {
                return module;
            }
        }

        return NoModule;
    }

private:
    Allocator& allocator_;
    String schema_;

public:
    SourceSet sources;
    DiagnosticList diagnostics;
    Compilation compilation;
};

StringView NameOf(const Compilation& compilation, const TestResult& result) noexcept {
    return compilation.Symbols()[result.symbol].name;
}

// 원문 text에서 needle(한 번 나오는 글자)의 byte 위치다.
uint32 OffsetOf(StringView text, StringView needle) noexcept {
    for (usize at = 0; at + needle.ByteCount() <= text.ByteCount(); ++at) {
        if (StringView(text.Data() + at, needle.ByteCount()) == needle) {
            return static_cast<uint32>(at);
        }
    }

    CHECK(false);
    return 0;
}

// 결과 하나가 기대와 같은지 본다(위치는 원문 글자 needle의 시작·길이).
void Expect(const Compilation& compilation, const TestResult& result, StringView name,
            TestOutcome outcome, VmFaultKind fault, StringView needle, uint32 width,
            uint32 frames) noexcept {
    CHECK(NameOf(compilation, result) == name);
    CHECK(result.outcome == outcome);
    CHECK(result.fault == fault);
    CHECK(result.frames == frames);
    const uint32 begin = OffsetOf(Checks, needle);
    CHECK(result.span.begin == begin);
    CHECK(result.span.end == begin + width);
}

bool Same(const TestResult& left, const TestResult& right) noexcept {
    return left.module == right.module && left.symbol == right.symbol &&
           left.outcome == right.outcome && left.fault == right.fault &&
           left.errorCode == right.errorCode && left.source == right.source &&
           left.span.begin == right.span.begin && left.span.end == right.span.end &&
           left.frames == right.frames && left.detail.View() == right.detail.View();
}

const StringView ModulePaths[] = {"Helpers.spark", "Checks.spark"};
const StringView ModuleTexts[] = {Helpers, Checks};

} // namespace

TEST(SparksTestRunner_ResultsOfEachKind) {
    SystemAllocator allocator;
    {
        Workspace workspace(allocator);
        REQUIRE(workspace.Build(ModulePaths, ModuleTexts));
        const uint32 modules[] = {workspace.Find("Checks.spark")};
        TestOptions options;
        options.maxFrames = FrameLimit;
        TestReport report(allocator);
        REQUIRE(RunTests(allocator, workspace.compilation, workspace.sources, modules, options,
                         report));
        const Span<const TestResult> results = report.Results();
        REQUIRE(results.Size() == 11);
        const Compilation& c = workspace.compilation;
        const VmFaultKind none = VmFaultKind::None;
        Expect(c, results[0], "Passes", TestOutcome::Passed, none, "Passes()", 6, 0);
        Expect(c, results[1], "TwoTicks", TestOutcome::Passed, none, "TwoTicks()", 8, 2);
        Expect(c, results[2], "FixedOnce", TestOutcome::Passed, none, "FixedOnce()", 9, 1);
        CHECK(NameOf(c, results[3]) == "WaitsForAll");
        CHECK(results[3].outcome == TestOutcome::Passed);
        CHECK(results[3].frames > 30 && results[3].frames < FrameLimit); // Delay 0.5 s
        Expect(c, results[4], "AssertFails", TestOutcome::Faulted, VmFaultKind::AssertFailed,
               "Assert(Twice(2)", 6, 0);
        Expect(c, results[5], "Overflows", TestOutcome::Faulted, VmFaultKind::IntegerOverflow,
               "big += 1", 3, 0);
        Expect(c, results[6], "FaultsLater", TestOutcome::Faulted, VmFaultKind::DivideByZero,
               "Math.Div", 4, 1);
        // 하위 실행(All 인자)의 fault는 부모(시험)의 결과다 — 위치는 하위 함수 안.
        Expect(c, results[7], "ChildFault", TestOutcome::Faulted, VmFaultKind::DivideByZero,
               "Math.Div", 4, 1);
        Expect(c, results[8], "TryFails", TestOutcome::EndedWithError, none, "TryFails()", 8, 0);
        CHECK(results[8].errorCode == 1); // Error.NotFound(첫 case) + 1
        Expect(c, results[9], "UsesEngine", TestOutcome::NeedsEngine,
               VmFaultKind::ContractViolation, "Actor.CountAll", 5, 0);
        CHECK(results[9].detail.View() == "Actor.CountAll");
        Expect(c, results[10], "Forever", TestOutcome::DidNotFinish, none, "Forever()", 7,
               FrameLimit);
        for (const TestResult& result : results) {
            CHECK(result.source == workspace.compilation.ModuleSource(modules[0]));
            CHECK(result.module == modules[0]);
            CHECK(result.detail.IsEmpty() == (result.outcome != TestOutcome::NeedsEngine));
        }

        CHECK(report.PassedCount() == 4);
        CHECK(report.FailedCount() == 7);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SparksTestRunner_EngineRefusalsAndIndexFault) {
    SystemAllocator allocator;
    {
        Workspace workspace(allocator);
        const StringView paths[] = {"Engine.spark"};
        const StringView texts[] = {Engine};
        REQUIRE(workspace.Build(paths, texts));
        const uint32 modules[] = {0};
        TestReport report(allocator);
        REQUIRE(RunTests(allocator, workspace.compilation, workspace.sources, modules, {}, report));
        const Span<const TestResult> results = report.Results();
        REQUIRE(results.Size() == 4);
        auto at = [&](StringView needle) noexcept { return OffsetOf(Engine, needle); };
        CHECK(results[0].outcome == TestOutcome::NeedsEngine);
        CHECK(results[0].detail.View() == "SpawnParams");
        CHECK(results[0].span.begin == at("SpawnParams("));
        CHECK(results[0].span.end == at("SpawnParams(") + 11);
        CHECK(results[1].outcome == TestOutcome::Passed); // Tag 리터럴은 값이라 된다
        CHECK(results[1].detail.IsEmpty());
        CHECK(results[2].outcome == TestOutcome::NeedsEngine);
        CHECK(results[2].detail.View() == "Soft.Load");
        CHECK(results[2].span.begin == at("icon.Load"));
        CHECK(results[3].outcome == TestOutcome::Faulted);
        CHECK(results[3].fault == VmFaultKind::IndexOutOfRange);
        CHECK(results[3].span.begin == at("values[5]"));
        CHECK(results[3].span.end == at("values[5]") + 6);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SparksTestRunner_ReportFormat) {
    SystemAllocator allocator;
    {
        Workspace workspace(allocator);
        REQUIRE(workspace.Build(ModulePaths, ModuleTexts));
        const uint32 modules[] = {workspace.Find("Checks.spark")};
        TestOptions options;
        options.maxFrames = FrameLimit;
        TestReport report(allocator);
        REQUIRE(RunTests(allocator, workspace.compilation, workspace.sources, modules, options,
                         report));
        StringBuilder<4096> text(allocator);
        for (const TestResult& result : report.Results()) {
            if (result.outcome != TestOutcome::Passed) {
                REQUIRE(AppendTestReport(text, workspace.compilation, workspace.sources, result));
            }
        }

        constexpr StringView Expected =
            R"(Game/Checks.spark:22:5: error AssertFailed: Test 'AssertFails' failed.
        Assert(Twice(2) == 5, "wrong");
        ^^^^^^
  The assertion is false.
Game/Checks.spark:27:5: error IntegerOverflow: Test 'Overflows' failed.
        big += 1;
        ^^^
  The test stopped with the fault 'IntegerOverflow'.
Game/Checks.spark:34:17: error DivideByZero: Test 'FaultsLater' failed.
        Log.Info("${Math.Div(1, zero)}");
                    ^^^^
  The test stopped with the fault 'DivideByZero'.
Game/Checks.spark:34:17: error DivideByZero: Test 'ChildFault' failed.
        Log.Info("${Math.Div(1, zero)}");
                    ^^^^
  The test stopped with the fault 'DivideByZero'.
Game/Checks.spark:41:10: error EndedWithError: Test 'TryFails' failed.
    @test fn TryFails() {
             ^^^^^^^^
  A 'try' failed with 'Error.NotFound'.
Game/Checks.spark:47:12: error NeedsEngine: Test 'UsesEngine' failed.
        Assert(Actor.CountAll() == 0, "none");
               ^^^^^
  'Actor.CountAll' needs the engine; tests run on the headless host until engine integration (EI).
Game/Checks.spark:50:16: error DidNotFinish: Test 'Forever' failed.
    @test async fn Forever() {
                   ^^^^^^^
  The test was still waiting after 50 frames.
)";
        CHECK(text.View() == Expected);
        if (text.View() != Expected) {
            platform::WriteDiagnostic(text.View().Data(), text.View().ByteCount());
        }
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SparksTestRunner_TestsAreIndependent) {
    // 같은 모듈을 두 번 돌린다: 앞 차례의 정지 실행(Forever)·거부 이름(UsesEngine)·fault 기록이 뒤
    // 차례에 남지 않아 결과가 같다. 모듈 순서는 호출자, 시험 없는 모듈은 결과가 없다.
    SystemAllocator allocator;
    {
        Workspace workspace(allocator);
        REQUIRE(workspace.Build(ModulePaths, ModuleTexts));
        const uint32 checks = workspace.Find("Checks.spark");
        const uint32 helpers = workspace.Find("Helpers.spark");
        const uint32 twice[] = {checks, helpers, checks};
        TestOptions options;
        options.maxFrames = FrameLimit;
        TestReport report(allocator);
        REQUIRE(
            RunTests(allocator, workspace.compilation, workspace.sources, twice, options, report));
        REQUIRE(report.Results().Size() == 22);
        for (usize at = 0; at < 11; ++at) {
            CHECK(Same(report.Results()[at], report.Results()[at + 11]));
        }

        const uint32 none[] = {helpers};
        REQUIRE(
            RunTests(allocator, workspace.compilation, workspace.sources, none, options, report));
        CHECK(report.Results().IsEmpty()); // 이전 결과는 버린다
        REQUIRE(RunTests(allocator, workspace.compilation, workspace.sources, Span<const uint32>(),
                         options, report));
        CHECK(report.Results().IsEmpty());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SparksTestRunner_RejectsBadInput) {
    SystemAllocator allocator;
    {
        Workspace workspace(allocator);
        REQUIRE(workspace.Build(ModulePaths, ModuleTexts));
        const uint32 modules[] = {workspace.Find("Checks.spark")};
        TestReport report(allocator);
        TestOptions options;
        options.maxFrames = 0;
        auto zero =
            RunTests(allocator, workspace.compilation, workspace.sources, modules, options, report);
        CHECK(!zero && zero.Error() == ErrorCode::InvalidArgument);
        CHECK(report.Reason() == "the frame limit must be from 1 to 1000000");
        options.maxFrames = MaxTestFrames + 1;
        auto big =
            RunTests(allocator, workspace.compilation, workspace.sources, modules, options, report);
        CHECK(!big && big.Error() == ErrorCode::InvalidArgument);
        options.maxFrames = MaxTestFrames;
        const uint32 outside[] = {workspace.compilation.ModuleCount()};
        auto range =
            RunTests(allocator, workspace.compilation, workspace.sources, outside, options, report);
        CHECK(!range && range.Error() == ErrorCode::InvalidArgument);
        CHECK(report.Reason() == "a module number is out of range");
        CHECK(report.Results().IsEmpty());
    }

    {
        // 진단이 있는 컴파일(bytecode 없는 모듈)은 돌리지 않는다.
        Workspace workspace(allocator);
        const StringView paths[] = {"Broken.spark"};
        const StringView texts[] = {"@test fn F() {\n    let x: int = \"text\";\n}\n"};
        CHECK(!workspace.Build(paths, texts));
        TestReport report(allocator);
        const uint32 modules[] = {0};
        auto ran =
            RunTests(allocator, workspace.compilation, workspace.sources, modules, {}, report);
        CHECK(!ran && ran.Error() == ErrorCode::InvalidArgument);
        CHECK(report.Reason() ==
              "a module has no bytecode (the compilation must finish without diagnostics)");
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SparksTestRunner_CallFunctionSession) {
    // VM 최상위 진입(SparksRuntime 2.8절): 동기 반환, 인자 모양 거부, 정지·재개와 하위 실행 표시,
    // fault 기록(instance 없음), CloseSession의 취소와 ledger 0, 닫힌 뒤 거부.
    SystemAllocator allocator;
    {
        constexpr StringView Flow = R"(fn Add(a: int, b: int): int {
    return a + b;
}

@test async fn Child() {
    await NextTick();
    await NextTick();
}

@test async fn Root() {
    await All(Child(), NextTick());
}

@test fn Bad() {
    Assert(Add(1, 1) == 3, "bad");
}

@test async fn Wait() {
    await Delay(100.0, Clock.Game);
}

class Box : ScriptableComponent {
    fn Size(): int {
        return 1;
    }
}
)";
        Workspace workspace(allocator);
        const StringView paths[] = {"Flow.spark"};
        const StringView texts[] = {Flow};
        REQUIRE(workspace.Build(paths, texts));
        const BytecodeModule* compiled = workspace.compilation.ModuleBytecode(0);
        REQUIRE(compiled != nullptr);
        Array<uint8> file(allocator);
        REQUIRE(WriteBytecodeFile(*compiled, file));
        BytecodeModule loaded(allocator);
        BytecodeRejection rejection;
        REQUIRE(ReadBytecodeFile(file.View(), loaded, rejection));
        VmLinkedModule linked(allocator);
        StringBuilder<256> reason(allocator);
        REQUIRE(LinkModule(loaded, nullptr, linked, reason));
        VmLinkedModule* links[] = {&linked};
        REQUIRE(LinkImports(links, reason));
        auto function = [&](StringView name) noexcept {
            for (uint32 index = 0; index < loaded.functions.Size(); ++index) {
                if (loaded.String(loaded.functions[index].name) == name) {
                    return index;
                }
            }

            CHECK(false);
            return NoBytecodeIndex;
        };

        VmSession session(allocator, nullptr);
        REQUIRE(session.AddModule(linked));
        const uint64 arguments[] = {2, 3};
        auto added = session.CallFunction(linked, function("Add"), arguments, {}, {});
        REQUIRE(added);
        CHECK(added.Value().outcome == VmOutcome::Returned);
        REQUIRE(session.Execution().ReturnedSlots().Size() == 1);
        CHECK(session.Execution().ReturnedSlots()[0] == 5);

        // 인자 모양·함수 번호가 틀리면 거부한다.
        const uint64 one[] = {2};
        auto shape = session.CallFunction(linked, function("Add"), one, {}, {});
        CHECK(!shape && shape.Error() == ErrorCode::InvalidArgument);
        auto range =
            session.CallFunction(linked, static_cast<uint32>(loaded.functions.Size()), {}, {}, {});
        CHECK(!range && range.Error() == ErrorCode::InvalidArgument);
        // 메서드(this를 받는다)는 instance 없이 부르지 않는다 — 모양을 맞춘 handle을 줘도 거부.
        const uint64 receiver[] = {1};
        auto method = session.CallFunction(linked, function("Box.Size"), {}, receiver, {});
        CHECK(!method && method.Error() == ErrorCode::InvalidArgument);

        // fault: 위치 기록은 남고 instance는 없다(0).
        auto bad = session.CallFunction(linked, function("Bad"), {}, {}, {});
        REQUIRE(bad);
        CHECK(bad.Value().outcome == VmOutcome::Faulted);
        CHECK(bad.Value().fault.kind == VmFaultKind::AssertFailed);
        REQUIRE(session.Faults().Size() == 1);
        CHECK(session.Faults()[0].instance == 0);
        CHECK(session.Faults()[0].kind == VmFaultKind::AssertFailed);

        // 정지·재개: Child(하위 실행) 기록은 child, Root의 끝은 child가 아니다.
        auto root = session.CallFunction(linked, function("Root"), {}, {}, {});
        REQUIRE(root);
        CHECK(root.Value().outcome == VmOutcome::Suspended);
        CHECK(session.SuspendedCount() >= 1);
        bool childSeen = false;
        bool rootDone = false;
        for (uint32 frame = 0; frame < 4 && !rootDone; ++frame) {
            REQUIRE(session.Advance(1.0 / 60.0, 1.0 / 60.0));
            for (const VmSession::Delivery& delivery : session.Deliveries()) {
                if (delivery.function == function("Child")) {
                    CHECK(delivery.child);
                    childSeen = true;
                }

                if (delivery.function == function("Root") &&
                    delivery.result.outcome == VmOutcome::Returned) {
                    CHECK(!delivery.child);
                    CHECK(delivery.subscriber == 0);
                    rootDone = true;
                }
            }
        }

        CHECK(childSeen);
        CHECK(rootDone);
        CHECK(session.SuspendedCount() == 0);

        // 남은 정지 실행은 CloseSession이 취소한다(Detach는 건드리지 않는다).
        auto wait = session.CallFunction(linked, function("Wait"), {}, {}, {});
        REQUIRE(wait);
        CHECK(wait.Value().outcome == VmOutcome::Suspended);
        session.DetachAll();
        CHECK(session.SuspendedCount() == 1);
        REQUIRE(session.CloseSession());
        CHECK(session.SuspendedCount() == 0);
        CHECK(session.WaitCount() == 0);
        session.Execution().ReleaseReturned();
        CHECK(session.Execution().LiveBuffers() == 0);
        auto closed = session.CallFunction(linked, function("Add"), arguments, {}, {});
        CHECK(!closed && closed.Error() == ErrorCode::InvalidArgument);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SparksTestRunner_AllocationFailureEverywhere) {
    // 실행기의 모든 할당 시도를 하나씩 실패시킨다: OutOfMemory이거나, 견뎠으면 결과가 기준과 같다
    // (삼킨 실패가 없다). 끝나면 실행기가 남긴 할당이 없다.
    SystemAllocator backing;
    {
        Workspace workspace(backing);
        REQUIRE(workspace.Build(ModulePaths, ModuleTexts));
        const uint32 modules[] = {workspace.Find("Checks.spark")};
        TestOptions options;
        options.maxFrames = FrameLimit;
        TestReport baseline(backing);
        REQUIRE(RunTests(backing, workspace.compilation, workspace.sources, modules, options,
                         baseline));
        uint64 attempts = 0;
        uint64 failures = 0;
        for (uint64 failAt = 0; failAt == 0 || failAt <= attempts; ++failAt) {
            test::NFailAllocator allocator(backing);
            const uint64 before = backing.Stats().liveAllocations;
            {
                TestReport report(allocator);
                if (failAt != 0) {
                    allocator.FailOnAttempt(failAt);
                }

                auto ran = RunTests(allocator, workspace.compilation, workspace.sources, modules,
                                    options, report);
                const uint64 used = allocator.Attempts();
                allocator.DisableFailure();
                if (failAt == 0) {
                    REQUIRE(ran);
                    attempts = used;
                } else if (!ran) {
                    CHECK(ran.Error() == ErrorCode::OutOfMemory);
                    ++failures;
                } else {
                    REQUIRE(report.Results().Size() == baseline.Results().Size());
                    for (usize at = 0; at < report.Results().Size(); ++at) {
                        CHECK(Same(report.Results()[at], baseline.Results()[at]));
                    }
                }
            }

            CHECK(backing.Stats().liveAllocations == before);
        }

        CHECK(failures > 0);
    }

    CHECK(backing.Stats().liveAllocations == 0);
}

} // namespace sparks_test_runner_tests_file
