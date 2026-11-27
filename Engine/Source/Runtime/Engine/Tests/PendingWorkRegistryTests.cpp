#include <Core/Diagnostics/LogSystem.h>
#include <Core/Diagnostics/RuntimeLogSink.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Move.h>
#include <Engine/Engine.h>
#include <Engine/PendingWorkRegistry.h>
#include <Localization/Text.h>
#include <Platform/Fatal.h>
#include <Progress/ProgressHub.h>
#include <Progress/ProgressScope.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/OsThread.h>
#include <TestSupport/Test.h>
#include <stdio.h>
#include <string.h>

using namespace rocks;
namespace pending_work_registry_tests_file {

// 대기 작업 등록소 시험(Docs/Design/ProgressReporting 14장 22–26). 가짜 source가 불린 순서를 한
// 기록에 남긴다. 다른 스레드 호출은 RunOnOsThread(만들고 끝까지 기다림)로 한다.
constexpr usize JournalCapacity = 64;
constexpr usize EventBytes = 48;
// 할당 실패 시험이 실패 지점을 찾아 도는 최대 횟수다(Register 한 번의 할당 수보다 넉넉히).
constexpr uint64 MaxFailureAttempts = 32;

struct Journal {
    char events[JournalCapacity][EventBytes]{};
    usize count = 0;

    void Note(const char* verb, StringView name) noexcept {
        if (count < JournalCapacity) {
            snprintf(events[count], EventBytes, "%s:%.*s", verb, static_cast<int>(name.ByteCount()),
                     name.Data());
            ++count;
        }
    }

    void Clear() noexcept {
        count = 0;
    }

    template <usize Count>
    bool Is(const char* const (&expected)[Count]) const noexcept {
        bool same = count == Count;
        for (usize index = 0; same && index < Count; ++index) {
            same = strcmp(events[index], expected[index]) == 0;
        }

        if (!same) {
            printf("journal:");
            for (usize index = 0; index < count; ++index) {
                printf(" %s", events[index]);
            }

            printf("\n");
        }

        return same;
    }
};

class FakeSource;
using FakeHook = void (*)(FakeSource& self) noexcept;

class FakeSource final : public PendingWorkSource {
public:
    FakeSource(Journal& journal, StringView name, Span<const StringView> runBefore,
               uint64 remaining) noexcept
        : journal_(journal), name_(name), runBefore_(runBefore), remaining_(remaining) {
    }

    StringView Name() const noexcept override {
        return name_;
    }

    const Text& DisplayName() const noexcept override {
        return displayName_;
    }

    Span<const StringView> RunBefore() const noexcept override {
        return runBefore_;
    }

    uint64 RemainingCount() const noexcept override {
        return remaining_;
    }

    Result<void> FinishFor(Span<Object* const> targets) noexcept override {
        journal_.Note("finishFor", name_);
        lastTargets = targets;
        return Result<void>::Success();
    }

    void CancelFor(Span<Object* const> targets) noexcept override {
        journal_.Note("cancelFor", name_);
        lastTargets = targets;
    }

    Result<void> FinishAll() noexcept override {
        journal_.Note("finishAll", name_);
        if (onFinishAll != nullptr) {
            onFinishAll(*this);
        }

        remaining_ = 0;
        return failWith == ErrorCode::None ? Result<void>::Success()
                                           : Result<void>::Failure(failWith);
    }

    void Shutdown() noexcept override {
        journal_.Note("shutdown", name_);
    }

    void ApplyCompleted(const PendingWorkPumpParams& params) noexcept override {
        journal_.Note("apply", name_);
        lastParams = params;
        ++applies;
        if (onApply != nullptr) {
            onApply(*this);
        }
    }

    Text displayName_;
    Span<Object* const> lastTargets;
    PendingWorkPumpParams lastParams;
    uint32 applies = 0;
    ErrorCode failWith = ErrorCode::None;
    FakeHook onFinishAll = nullptr;
    FakeHook onApply = nullptr;
    PendingWorkRegistry* registry = nullptr;
    // 재진입 시험이 등록하려는 다른 source다.
    PendingWorkSource* other = nullptr;
    // 재진입 시험 결과다.
    ErrorCode registerError = ErrorCode::None;
    ErrorCode unregisterError = ErrorCode::None;
    ErrorCode finishAllError = ErrorCode::None;
    ErrorCode finishForError = ErrorCode::None;
    ErrorCode cancelForError = ErrorCode::None;
    ErrorCode applyError = ErrorCode::None;

private:
    Journal& journal_;
    StringView name_;
    Span<const StringView> runBefore_;
    uint64 remaining_ = 0;
};

// 로그 템플릿을 남기는 sink다(Manual 모드에서 Pump로 받는다).
class TemplateCapture final : public RuntimeLogSink {
public:
    static constexpr uint32 MaxRecords = 32;
    static constexpr usize TextBytes = 160;

    Result<void> Write(const LogEvent& event) noexcept override {
        if (count < MaxRecords) {
            levels[count] = event.level;
            const usize bytes = event.messageTemplate.ByteCount() < TextBytes - 1
                                    ? event.messageTemplate.ByteCount()
                                    : TextBytes - 1;
            memcpy(templates[count], event.messageTemplate.Data(), bytes);
            templates[count][bytes] = 0;
            ++count;
        }

        return Result<void>::Success();
    }

    // level이고 템플릿이 text인 기록 수다.
    uint32 Count(LogLevel level, const char* text) const noexcept {
        uint32 matches = 0;
        for (uint32 index = 0; index < count; ++index) {
            if (levels[index] == level && strcmp(templates[index], text) == 0) {
                ++matches;
            }
        }

        return matches;
    }

    LogLevel levels[MaxRecords]{};
    char templates[MaxRecords][TextBytes]{};
    uint32 count = 0;
};

// 시험 동안 Manual LogSystem에 capture를 붙여 설치한다. 앞 시험이 설치 밖에서 버린 기록의 알림은
// 버리는 설치 하나로 먼저 소비한다.
struct CapturedLogs {
    CapturedLogs() noexcept : logs(allocator, MakeConfig()) {
        {
            LogSystem reset(allocator, LogSystemConfig::ForTool());
            (void)reset.Start();
            LogInstallScope drained(reset);
        }

        if (!logs.Start() || !logs.AddSink(capture)) {
            platform::Fatal("Manual log system did not start");
        }
    }

    static LogSystemConfig MakeConfig() noexcept {
        LogSystemConfig config;
        config.mode = LogMode::Manual;
        config.globalMinimum = LogLevel::Debug;
        return config;
    }

    uint32 Count(LogLevel level, const char* text) noexcept {
        (void)logs.Pump();
        return capture.Count(level, text);
    }

    SystemAllocator allocator;
    TemplateCapture capture;
    LogSystem logs;
};

constexpr const char* CycleTemplate = "Pending work source {Name} makes a RunBefore cycle; other "
                                      "sources' RunBefore entries naming it are ignored";
constexpr const char* OffThreadShutdownTemplate =
    "Pending work registry Shutdown called off its owner thread; ignored";
constexpr const char* NestedShutdownTemplate =
    "Pending work registry Shutdown called during a registry call; ignored";
constexpr const char* LeftRegisteredTemplate =
    "Pending work registry destroyed with {Count} registered sources (modules must unregister "
    "before they unload)";

// 시험 22: 합계, RunBefore 순서(뒤에 등록한 것도 앞으로), 순환은 등록을 받고 앞 source가 새
// source를 가리키는 의존을 무시(UE와 같다), 없는 이름은 무시, Unregister 뒤 순서 유지.
TEST(PendingWork_OrderSumAndCycles) {
    CapturedLogs logs;
    LogInstallScope installed(logs.logs);
    SystemAllocator allocator;
    Journal journal;
    PendingWorkRegistry registry(allocator);
    constexpr StringView afterTextures[] = {"Textures", "Missing"};
    constexpr StringView afterShaders[] = {"Shaders"};
    FakeSource shaders(journal, "Shaders", Span<const StringView>(afterTextures), 3);
    FakeSource textures(journal, "Textures", Span<const StringView>(afterShaders), 4);
    FakeSource meshes(journal, "Meshes", Span<const StringView>(afterShaders), 5);
    // Shaders는 아직 없는 Textures를 기다린다(무시).
    REQUIRE(registry.Register(shaders));
    CHECK(logs.Count(LogLevel::Error, CycleTemplate) == 0);
    // Textures가 Shaders 뒤라고 하면 순환이다 — 등록은 받고 Shaders → Textures 의존을 무시한다.
    REQUIRE(registry.Register(textures));
    CHECK(logs.Count(LogLevel::Error, CycleTemplate) == 1);
    REQUIRE(registry.Register(meshes));
    CHECK(logs.Count(LogLevel::Error, CycleTemplate) == 1);
    CHECK(registry.RemainingCount() == 12);
    REQUIRE(registry.SourceCount() == 3);
    CHECK(registry.SourceNameAt(0) == StringView("Shaders"));
    CHECK(registry.SourceNameAt(1) == StringView("Textures"));
    CHECK(registry.SourceNameAt(2) == StringView("Meshes"));
    CHECK(registry.SourceNameAt(3).IsEmpty());

    FakeSource duplicate(journal, "Shaders", {}, 0);
    FakeSource empty(journal, "", {}, 0);
    CHECK(registry.Register(duplicate).Error() == ErrorCode::AlreadyExists);
    CHECK(registry.Register(shaders).Error() == ErrorCode::AlreadyExists);
    CHECK(registry.Register(empty).Error() == ErrorCode::InvalidArgument);
    CHECK(registry.Unregister(duplicate).Error() == ErrorCode::NotFound);
    CHECK(registry.SourceCount() == 3);

    // 가운데·앞을 빼도 남은 순서는 그대로다(색인을 당긴다).
    REQUIRE(registry.Unregister(textures));
    REQUIRE(registry.SourceCount() == 2);
    CHECK(registry.SourceNameAt(0) == StringView("Shaders"));
    CHECK(registry.SourceNameAt(1) == StringView("Meshes"));
    REQUIRE(registry.Unregister(shaders));
    REQUIRE(registry.SourceCount() == 1);
    CHECK(registry.SourceNameAt(0) == StringView("Meshes"));
    REQUIRE(registry.Unregister(meshes));
    CHECK(registry.RemainingCount() == 0);
    CHECK(registry.SourceCount() == 0);
}

// 시험 22: 뒤에 등록한 의존은 앞으로 간다(같은 순위는 등록 순서).
TEST(PendingWork_LaterDependencyMovesFirst) {
    SystemAllocator allocator;
    Journal journal;
    PendingWorkRegistry registry(allocator);
    constexpr StringView afterTextures[] = {"Textures"};
    FakeSource materials(journal, "Materials", Span<const StringView>(afterTextures), 1);
    FakeSource audio(journal, "Audio", {}, 1);
    FakeSource textures(journal, "Textures", {}, 1);
    REQUIRE(registry.Register(materials));
    REQUIRE(registry.Register(audio));
    REQUIRE(registry.Register(textures));
    REQUIRE(registry.SourceCount() == 3);
    CHECK(registry.SourceNameAt(0) == StringView("Audio"));
    CHECK(registry.SourceNameAt(1) == StringView("Textures"));
    CHECK(registry.SourceNameAt(2) == StringView("Materials"));
    REQUIRE(registry.Unregister(materials));
    REQUIRE(registry.Unregister(audio));
    REQUIRE(registry.Unregister(textures));
}

// 시험 22: 모든 할당 지점의 실패가 등록을 되돌린다(순환 경로 포함 — 로그 없음). Unregister는
// 할당하지 않는다.
TEST(PendingWork_RegisterRollsBackOnAllocationFailure) {
    CapturedLogs logs;
    LogInstallScope installed(logs.logs);
    SystemAllocator system;
    test::NFailAllocator allocator(system);
    Journal journal;
    PendingWorkRegistry registry(allocator);
    constexpr StringView afterTextures[] = {"Textures"};
    constexpr StringView afterShaders[] = {"Shaders"};
    FakeSource shaders(journal, "Shaders", Span<const StringView>(afterTextures), 3);
    FakeSource textures(journal, "Textures", Span<const StringView>(afterShaders), 4);
    REQUIRE(registry.Register(shaders));

    uint32 failures = 0;
    bool registered = false;
    for (uint64 attempt = 1; attempt <= MaxFailureAttempts && !registered; ++attempt) {
        allocator.FailOnAttempt(attempt);
        auto result = registry.Register(textures);
        allocator.DisableFailure();
        if (result) {
            registered = true;
            break;
        }

        ++failures;
        CHECK(result.Error() == ErrorCode::OutOfMemory);
        CHECK(registry.SourceCount() == 1);
        CHECK(registry.RemainingCount() == 3);
        CHECK(logs.Count(LogLevel::Error, CycleTemplate) == 0);
    }

    REQUIRE(registered);
    // 칸 늘리기·첫 정렬 둘·순환 뒤 정렬 둘 — 실패 지점이 적어도 셋이다.
    CHECK(failures >= 3);
    CHECK(logs.Count(LogLevel::Error, CycleTemplate) == 1);
    REQUIRE(registry.SourceCount() == 2);

    allocator.FailOnAttempt(1);
    REQUIRE(registry.Unregister(shaders));
    CHECK(allocator.Attempts() == 0);
    allocator.DisableFailure();
    REQUIRE(registry.SourceCount() == 1);
    CHECK(registry.SourceNameAt(0) == StringView("Textures"));
    REQUIRE(registry.Unregister(textures));
}

struct ThreadCall {
    PendingWorkRegistry* registry = nullptr;
    FakeSource* source = nullptr;
    FakeSource* registered = nullptr;
    ErrorCode registerError = ErrorCode::None;
    ErrorCode unregisterError = ErrorCode::None;
    ErrorCode finishAllError = ErrorCode::None;
    ErrorCode finishForError = ErrorCode::None;
    ErrorCode cancelForError = ErrorCode::None;
    ErrorCode applyError = ErrorCode::None;
    uint64 remaining = 0;
    usize sourceCount = 0;
    bool nameEmpty = false;
};

void CallFromOtherThread(void* context) noexcept {
    auto& call = *static_cast<ThreadCall*>(context);
    Object* const targets[] = {nullptr};
    call.registerError = call.registry->Register(*call.source).Error();
    call.unregisterError = call.registry->Unregister(*call.registered).Error();
    call.finishAllError = call.registry->FinishAll().Error();
    call.finishForError = call.registry->FinishFor(Span<Object* const>(targets)).Error();
    call.cancelForError = call.registry->CancelFor(Span<Object* const>(targets)).Error();
    call.applyError = call.registry->ApplyCompleted({}).Error();
    call.registry->Shutdown();
    call.remaining = call.registry->RemainingCount();
    call.sourceCount = call.registry->SourceCount();
    call.nameEmpty = call.registry->SourceNameAt(0).IsEmpty();
}

// 시험 23: 소유 스레드 밖 호출은 WrongThread다(RemainingCount 제외, Shutdown은 Error 로그·no-op).
TEST(PendingWork_OwnerThreadOnly) {
    CapturedLogs logs;
    LogInstallScope installed(logs.logs);
    SystemAllocator allocator;
    Journal journal;
    PendingWorkRegistry registry(allocator);
    FakeSource shaders(journal, "Shaders", {}, 7);
    FakeSource other(journal, "Other", {}, 1);
    REQUIRE(registry.Register(shaders));
    ThreadCall call;
    call.registry = &registry;
    call.source = &other;
    call.registered = &shaders;
    REQUIRE(test::RunOnOsThread(CallFromOtherThread, &call));
    CHECK(call.registerError == ErrorCode::WrongThread);
    CHECK(call.unregisterError == ErrorCode::WrongThread);
    CHECK(call.finishAllError == ErrorCode::WrongThread);
    CHECK(call.finishForError == ErrorCode::WrongThread);
    CHECK(call.cancelForError == ErrorCode::WrongThread);
    CHECK(call.applyError == ErrorCode::WrongThread);
    CHECK(call.remaining == 7);
    CHECK(call.sourceCount == 0);
    CHECK(call.nameEmpty);
    CHECK(journal.count == 0);
    CHECK(logs.Count(LogLevel::Error, OffThreadShutdownTemplate) == 1);
    // 밖에서 부른 Shutdown은 아무 일도 하지 않았으므로 등록이 아직 된다.
    FakeSource late(journal, "Late", {}, 0);
    REQUIRE(registry.Register(late));
    CHECK(registry.AdoptCurrentThread().Error() == ErrorCode::Busy);
    REQUIRE(registry.Unregister(late));
    REQUIRE(registry.Unregister(shaders));
}

void AdoptOnThread(void* context) noexcept {
    auto& registry = *static_cast<PendingWorkRegistry*>(context);
    (void)registry.AdoptCurrentThread();
}

// 시험 23: 등록이 없으면 AdoptCurrentThread가 소유 스레드를 바꾼다.
TEST(PendingWork_AdoptCurrentThreadMovesOwner) {
    SystemAllocator allocator;
    Journal journal;
    PendingWorkRegistry registry(allocator);
    FakeSource shaders(journal, "Shaders", {}, 1);
    REQUIRE(test::RunOnOsThread(AdoptOnThread, &registry));
    CHECK(registry.Register(shaders).Error() == ErrorCode::WrongThread);
    REQUIRE(registry.AdoptCurrentThread());
    REQUIRE(registry.Register(shaders));
    REQUIRE(registry.Unregister(shaders));
}

struct ProgressSeen {
    bool sawBlocking = false;
    uint32 parts = 0;
    double boundSelfFraction = 0.0;
};

ProgressSeen progressSeen;

// FinishAll 안에서 진행을 본다: 막는 작업, 그 아래 하위 작업 수, 바인딩된 하위 작업의 자기 진행.
void ObserveProgress(FakeSource& self) noexcept {
    (void)self;
    SystemAllocator allocator;
    ProgressSnapshot snapshot(allocator);
    uint64 cursor = 0;
    if (!ProgressHub::Get().Snapshot(snapshot, cursor)) {
        return;
    }

    ProgressTaskId blocking = NoProgressTask;
    for (const ProgressTaskView& view : snapshot.tasks) {
        if (view.presentation == ProgressPresentation::Blocking) {
            progressSeen.sawBlocking = true;
            blocking = view.id;
        }
    }

    for (const ProgressTaskView& view : snapshot.tasks) {
        if (blocking != NoProgressTask && view.parent == blocking) {
            ++progressSeen.parts;
            if (view.selfFraction > progressSeen.boundSelfFraction) {
                progressSeen.boundSelfFraction = view.selfFraction;
            }
        }
    }
}

// 바인딩된 하위 작업 위에서 구간으로 보고한 뒤 본다(4 중 2 끝 — 자기 진행 0.5).
void ReportAndObserve(FakeSource& self) noexcept {
    ProgressScope scope(4.0);
    scope.EnterStep(1.0);
    scope.EnterStep(1.0);
    scope.EnterStep(1.0);
    ObserveProgress(self);
}

// 시험 동안 hub를 켜고 REQUIRE 실패로 빠져도 끈다.
struct HubSession {
    explicit HubSession(Allocator& allocator) noexcept {
        started = static_cast<bool>(ProgressHub::Get().Initialize(allocator, {}));
    }

    ~HubSession() noexcept {
        if (started) {
            ProgressHub::Get().Shutdown();
        }
    }

    bool started = false;
};

// 시험 24: FinishAll은 남은 수와 상관없이 모든 source를 순서대로 부르고(사이사이 ApplyCompleted
// 없음) 끝에 남은 수가 0이다. 할 일이 있으면 막는 진행 작업 아래 종류별 하위 작업이 있고, source의
// 구간이 바인딩된 하위 작업을 채운다. DisplayName이 없으면 종류 이름이 제목이다. source 오류는
// 처음 것을 돌려주고 나머지는 계속.
TEST(PendingWork_FinishAllCallsEverySourceAndReportsProgress) {
    SystemAllocator allocator;
    HubSession hub(allocator);
    REQUIRE(hub.started);
    Journal journal;
    PendingWorkRegistry registry(allocator);
    constexpr StringView afterTextures[] = {"Textures"};
    FakeSource shaders(journal, "Shaders", Span<const StringView>(afterTextures), 3);
    FakeSource textures(journal, "Textures", {}, 4);
    FakeSource idle(journal, "Idle", {}, 0);
    auto title = Text::Invariant(allocator, "Compiling shaders");
    REQUIRE(title);
    shaders.displayName_ = Move(title.Value());
    shaders.onFinishAll = &ReportAndObserve;
    idle.onFinishAll = &ObserveProgress;
    textures.onFinishAll = &ObserveProgress;
    textures.failWith = ErrorCode::DependencyFailed;
    shaders.failWith = ErrorCode::Cancelled;
    REQUIRE(registry.Register(shaders));
    REQUIRE(registry.Register(textures));
    REQUIRE(registry.Register(idle));
    progressSeen = {};
    CHECK(registry.FinishAll().Error() == ErrorCode::DependencyFailed);
    const char* const expected[] = {"finishAll:Textures", "finishAll:Shaders", "finishAll:Idle"};
    CHECK(journal.Is(expected));
    CHECK(registry.RemainingCount() == 0);
    CHECK(progressSeen.sawBlocking);
    // source마다 자기 하위 작업 하나만 살아 있다(앞 것은 끝났다) — Textures(DisplayName이 없어 종류
    // 이름이 제목)·Shaders·Idle(남은 수 0이어도 하위 작업) 셋.
    CHECK(progressSeen.parts == 3);
    CHECK(progressSeen.boundSelfFraction > 0.4);
    CHECK(progressSeen.boundSelfFraction < 0.6);
    CHECK(ProgressHub::Get().Stats().invalidWorkWarnings == 0);

    // 할 일이 없으면 막는 진행 작업을 만들지 않는다(그래도 모든 source를 부른다).
    journal.Clear();
    progressSeen = {};
    textures.failWith = ErrorCode::None;
    shaders.failWith = ErrorCode::None;
    shaders.onFinishAll = &ObserveProgress;
    REQUIRE(registry.FinishAll());
    CHECK(journal.Is(expected));
    CHECK(!progressSeen.sawBlocking);
    CHECK(progressSeen.parts == 0);
    REQUIRE(registry.Unregister(shaders));
    REQUIRE(registry.Unregister(textures));
    REQUIRE(registry.Unregister(idle));
}

// 시험 24: hub가 꺼져 있으면 진행 없이 끝낸다.
TEST(PendingWork_FinishAllWithoutHub) {
    SystemAllocator allocator;
    Journal journal;
    PendingWorkRegistry registry(allocator);
    FakeSource shaders(journal, "Shaders", {}, 2);
    REQUIRE(registry.Register(shaders));
    REQUIRE(registry.FinishAll());
    const char* const expected[] = {"finishAll:Shaders"};
    CHECK(journal.Is(expected));
    CHECK(registry.RemainingCount() == 0);
    REQUIRE(registry.Unregister(shaders));
}

// source 안에서 등록소를 다시 부른다(전부 Busy여야 한다 — Shutdown은 Error 로그·no-op).
void ReenterRegistry(FakeSource& self) noexcept {
    Object* const targets[] = {nullptr};
    self.registerError = self.registry->Register(*self.other).Error();
    self.unregisterError = self.registry->Unregister(self).Error();
    self.finishAllError = self.registry->FinishAll().Error();
    self.finishForError = self.registry->FinishFor(Span<Object* const>(targets)).Error();
    self.cancelForError = self.registry->CancelFor(Span<Object* const>(targets)).Error();
    self.applyError = self.registry->ApplyCompleted({}).Error();
    self.registry->Shutdown();
}

bool AllBusy(const FakeSource& source) noexcept {
    return source.registerError == ErrorCode::Busy && source.unregisterError == ErrorCode::Busy &&
           source.finishAllError == ErrorCode::Busy && source.finishForError == ErrorCode::Busy &&
           source.cancelForError == ErrorCode::Busy && source.applyError == ErrorCode::Busy;
}

// 시험 25: FinishAll·ApplyCompleted 안에서 등록소 호출은 Busy이고 Shutdown은 무시된다.
TEST(PendingWork_ReentryIsBusy) {
    CapturedLogs logs;
    LogInstallScope installed(logs.logs);
    SystemAllocator allocator;
    Journal journal;
    PendingWorkRegistry registry(allocator);
    FakeSource shaders(journal, "Shaders", {}, 1);
    FakeSource other(journal, "Other", {}, 0);
    shaders.registry = &registry;
    shaders.other = &other;
    shaders.onFinishAll = &ReenterRegistry;
    REQUIRE(registry.Register(shaders));
    REQUIRE(registry.FinishAll());
    CHECK(AllBusy(shaders));

    shaders.onFinishAll = nullptr;
    shaders.registerError = ErrorCode::None;
    shaders.unregisterError = ErrorCode::None;
    shaders.finishAllError = ErrorCode::None;
    shaders.finishForError = ErrorCode::None;
    shaders.cancelForError = ErrorCode::None;
    shaders.applyError = ErrorCode::None;
    shaders.onApply = &ReenterRegistry;
    REQUIRE(registry.ApplyCompleted({}));
    CHECK(AllBusy(shaders));
    CHECK(logs.Count(LogLevel::Error, NestedShutdownTemplate) == 2);
    CHECK(registry.SourceCount() == 1);
    // 안에서 부른 Shutdown은 무시됐으므로 등록이 아직 된다.
    REQUIRE(registry.Register(other));
    REQUIRE(registry.Unregister(other));
    REQUIRE(registry.Unregister(shaders));
}

// 시험 25: FinishFor·CancelFor는 모든 source에 같은 목록을 넘기고 빈 목록이면 아무 일도 하지
// 않는다. Shutdown은 RunBefore 순서(UE와 같다)이고 두 번째는 no-op, 그 뒤 Register는
// ShuttingDown이지만 Unregister는 된다.
TEST(PendingWork_TargetsAndShutdown) {
    SystemAllocator allocator;
    Journal journal;
    PendingWorkRegistry registry(allocator);
    constexpr StringView afterTextures[] = {"Textures"};
    FakeSource shaders(journal, "Shaders", Span<const StringView>(afterTextures), 1);
    FakeSource textures(journal, "Textures", {}, 1);
    REQUIRE(registry.Register(shaders));
    REQUIRE(registry.Register(textures));
    Object* const targets[] = {nullptr, nullptr};
    REQUIRE(registry.FinishFor(Span<Object* const>(targets)));
    REQUIRE(registry.CancelFor(Span<Object* const>(targets)));
    const char* const routed[] = {"finishFor:Textures", "finishFor:Shaders", "cancelFor:Textures",
                                  "cancelFor:Shaders"};
    CHECK(journal.Is(routed));
    CHECK(shaders.lastTargets.Data() == targets);
    CHECK(textures.lastTargets.Data() == targets);
    CHECK(shaders.lastTargets.Size() == 2);

    journal.Clear();
    REQUIRE(registry.FinishFor({}));
    REQUIRE(registry.CancelFor({}));
    CHECK(journal.count == 0);

    registry.Shutdown();
    registry.Shutdown();
    const char* const down[] = {"shutdown:Textures", "shutdown:Shaders"};
    CHECK(journal.Is(down));
    FakeSource late(journal, "Late", {}, 0);
    CHECK(registry.Register(late).Error() == ErrorCode::ShuttingDown);
    REQUIRE(registry.Unregister(shaders));
    REQUIRE(registry.Unregister(textures));
}

// 등록이 남은 채 소멸하면 Warning이다.
TEST(PendingWork_DestroyWithSourcesWarns) {
    CapturedLogs logs;
    LogInstallScope installed(logs.logs);
    SystemAllocator allocator;
    Journal journal;
    FakeSource shaders(journal, "Shaders", {}, 0);
    {
        PendingWorkRegistry registry(allocator);
        REQUIRE(registry.Register(shaders));
    }

    CHECK(logs.Count(LogLevel::Warning, LeftRegisteredTemplate) == 1);
    {
        PendingWorkRegistry registry(allocator);
        REQUIRE(registry.Register(shaders));
        REQUIRE(registry.Unregister(shaders));
    }

    CHECK(logs.Count(LogLevel::Warning, LeftRegisteredTemplate) == 1);
}

// 시험 26: ApplyCompleted 매개변수(시간 예산·Tryout 전용)가 그대로 간다.
TEST(PendingWork_ApplyCompletedPassesParams) {
    SystemAllocator allocator;
    Journal journal;
    PendingWorkRegistry registry(allocator);
    FakeSource shaders(journal, "Shaders", {}, 1);
    REQUIRE(registry.Register(shaders));
    PendingWorkPumpParams params;
    params.timeBudgetNanoseconds = 2'000'000;
    params.tryoutAssetsOnly = true;
    REQUIRE(registry.ApplyCompleted(params));
    CHECK(shaders.lastParams.timeBudgetNanoseconds == 2'000'000);
    CHECK(shaders.lastParams.tryoutAssetsOnly);
    REQUIRE(registry.Unregister(shaders));
}

// 프레임 시작 hook 뒤 ApplyCompleted, 종료 시작 hook 전 등록소 Shutdown을 기록한다.
class JournalHooks final : public EngineLoopHooks {
public:
    explicit JournalHooks(Journal& journal) noexcept : journal_(journal) {
    }

    void BeginFrame(Engine&) noexcept override {
        journal_.Note("hook", "BeginFrame");
    }

    void BeginShutdown(Engine&) noexcept override {
        journal_.Note("hook", "BeginShutdown");
    }

private:
    Journal& journal_;
};

// Engine은 매 프레임 BeginFrame 뒤 프레임 예산으로 ApplyCompleted를 부르고, Shutdown 처음(종료
// hook 전)에 등록소 Shutdown을 부른다.
TEST(PendingWork_EngineAppliesEachFrameAndShutsDown) {
    SystemAllocator allocator;
    Journal journal;
    FakeSource shaders(journal, "Shaders", {}, 0);
    JournalHooks hooks(journal);
    {
        EngineConfig config;
        config.modules = &test::ProcessModules();
        config.maxFrames = 2;
        config.fixedFrameDeltaSeconds = 1.0 / 60.0;
        config.hooks = &hooks;
        Engine engine(allocator);
        REQUIRE(engine.Initialize(config));
        REQUIRE(engine.PendingWork().Register(shaders));
        REQUIRE(engine.RunFrame());
        REQUIRE(engine.RunFrame());
        CHECK(shaders.applies == 2);
        CHECK(shaders.lastParams.timeBudgetNanoseconds == PendingWorkFrameBudgetNanoseconds);
        CHECK(!shaders.lastParams.tryoutAssetsOnly);
        REQUIRE(engine.Shutdown());
        const char* const expected[] = {"hook:BeginFrame",  "apply:Shaders",
                                        "hook:BeginFrame",  "apply:Shaders",
                                        "shutdown:Shaders", "hook:BeginShutdown"};
        CHECK(journal.Is(expected));
        REQUIRE(engine.PendingWork().Unregister(shaders));
    }
}

} // namespace pending_work_registry_tests_file
