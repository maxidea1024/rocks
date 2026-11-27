#include <Core/Memory/SystemAllocator.h>
#include <Core/Move.h>
#include <Engine/Engine.h>
#include <Engine/ModuleStartProgress.h>
#include <Localization/Catalog.h>
#include <Localization/Text.h>
#include <Progress/ProgressBinding.h>
#include <Progress/ProgressHub.h>
#include <TestSupport/Test.h>

using namespace rocks;
namespace engine_progress_tests_file {

// 시작 진행 구간 시험(Docs/Design/SplashScreen 8.1절, Docs/Design/ModuleLoader 3.16절). 실제
// ProgressHub에 작업을 열고 이 스레드를 Primary로 묶은 뒤, 모듈 시작 어댑터와 Engine::Initialize가
// 여는 구간의 비율·문구를 스냅샷으로 본다.
constexpr double Tolerance = 1e-12;

bool Near(double left, double right) noexcept {
    const double difference = left - right;
    return difference < Tolerance && difference > -Tolerance;
}

struct Observed {
    double fraction = -1.0;
    bool messageMatches = false;
};

// 작업의 자기 진행과 한 줄 문구(원문)를 본다.
Observed Observe(Allocator& allocator, ProgressTaskId task, StringView expectedMessage) noexcept {
    Observed observed;
    ProgressSnapshot snapshot(allocator);
    uint64 cursor = 0;
    if (!ProgressHub::Get().Snapshot(snapshot, cursor)) {
        return observed;
    }

    for (const ProgressTaskView& view : snapshot.tasks) {
        if (view.id != task) {
            continue;
        }

        observed.fraction = view.selfFraction;
        auto resolved = ResolveText(allocator, view.DisplayMessage(), CatalogSnapshot());
        observed.messageMatches = resolved && resolved.Value().value.View() == expectedMessage;
    }

    return observed;
}

struct BoundTask {
    SystemAllocator allocator;
    bool ready = false;
    ProgressTaskHandle task;

    BoundTask() noexcept {
        ready = static_cast<bool>(ProgressHub::Get().Initialize(allocator, {}));
        auto title = Text::Invariant(allocator, "Starting Test");
        if (!ready || !title) {
            ready = false;
            return;
        }

        ProgressTaskDesc desc;
        desc.title = &title.Value();
        auto begun = ProgressHub::Get().Begin(desc);
        ready = static_cast<bool>(begun);
        if (ready) {
            task = Move(begun.Value());
        }
    }

    ~BoundTask() noexcept {
        if (task.IsValid()) {
            (void)task.Finish(ProgressOutcome::Succeeded);
        }

        ProgressHub::Get().Shutdown();
    }
};

// 어댑터는 StartPhase 하나마다 모듈 수 구간을 열고 모듈마다 "Starting module <이름>"으로 들어간다.
// 로더가 부르는 관찰자 함수를 직접 불러(로더에서 꺼내) 순서를 고정한다. 긴 이름은 code point
// 경계에서 잘리고, 관찰자를 놓으면 이전 관찰자가 돌아온다.
TEST(Engine_ModuleStartProgressReportsModules) {
    BoundTask fixture;
    REQUIRE(fixture.ready);
    ModuleLoader& loader = test::ProcessModules();
    const ModuleStartObserver before = loader.SetStartObserver({});
    (void)loader.SetStartObserver(before);
    {
        auto binding = ProgressBinding::Bind(fixture.task.Id(), ProgressBindingRole::Primary);
        REQUIRE(binding);
        ModuleStartProgress progress(loader, fixture.allocator);
        const ModuleStartObserver observer = loader.SetStartObserver({});
        (void)loader.SetStartObserver(observer);
        REQUIRE(observer.phaseBegin != nullptr);
        observer.phaseBegin(observer.context, 4);
        observer.moduleStarting(observer.context, "World");
        Observed first = Observe(fixture.allocator, fixture.task.Id(), "Starting module World");
        CHECK(Near(first.fraction, 0.0));
        CHECK(first.messageMatches);
        observer.moduleStarting(observer.context, "Physics");
        observer.moduleStarting(observer.context, "Audio");
        CHECK(Near(Observe(fixture.allocator, fixture.task.Id(), "Starting module Audio").fraction,
                   0.5));

        // "가"(3 byte)를 200개 — 문구 칸보다 길다.
        char longName[600];
        for (usize index = 0; index < 200; ++index) {
            longName[index * 3] = '\xEA';
            longName[index * 3 + 1] = '\xB0';
            longName[index * 3 + 2] = '\x80';
        }

        observer.moduleStarting(observer.context, StringView(longName, sizeof(longName)));
        {
            ProgressSnapshot snapshot(fixture.allocator);
            uint64 cursor = 0;
            REQUIRE(ProgressHub::Get().Snapshot(snapshot, cursor));
            REQUIRE(snapshot.tasks.Size() == 1);
            auto resolved = ResolveText(fixture.allocator, snapshot.tasks[0].DisplayMessage(),
                                        CatalogSnapshot());
            REQUIRE(resolved);
            const StringView message = resolved.Value().value.View();
            CHECK(message.StartsWith("Starting module "));
            CHECK((message.ByteCount() - StringView("Starting module ").ByteCount()) % 3 == 0);
        }

        observer.phaseEnd(observer.context);
        CHECK(Near(Observe(fixture.allocator, fixture.task.Id(), "").fraction, 1.0));
    }

    CHECK(loader.SetStartObserver(before).phaseBegin == before.phaseBegin);
}

// Engine::Initialize 안에서 서브시스템 초기화는 둘째 구간(0.6에서 시작, 문구 "Creating engine
// subsystems")이다. 서브시스템이 그 순간의 스냅샷을 남긴다.
struct InitObservation {
    Allocator* allocator = nullptr;
    ProgressTaskId task = NoProgressTask;
    Observed observed;
};

InitObservation initObservation;

class ProgressProbeSubsystem final : public EngineSubsystem {
public:
    Result<void> Initialize(Engine&) noexcept override {
        initObservation.observed =
            Observe(*initObservation.allocator, initObservation.task, "Creating engine subsystems");
        return Result<void>::Success();
    }
};

TEST(Engine_InitializeReportsProgressSteps) {
    BoundTask fixture;
    REQUIRE(fixture.ready);
    initObservation = {};
    initObservation.allocator = &fixture.allocator;
    initObservation.task = fixture.task.Id();
    {
        auto binding = ProgressBinding::Bind(fixture.task.Id(), ProgressBindingRole::Primary);
        REQUIRE(binding);
        const EngineSubsystemType subsystems[] = {
            MakeSubsystemType<EngineSubsystem, ProgressProbeSubsystem>("ProgressProbe")};
        EngineConfig config;
        config.modules = &test::ProcessModules();
        config.subsystems = subsystems;
        Engine engine(fixture.allocator);
        REQUIRE(engine.Initialize(config));
        CHECK(Near(initObservation.observed.fraction, 0.6));
        CHECK(initObservation.observed.messageMatches);
        // 초기화가 끝나면 그 범위가 닫혀 전체를 채운다.
        CHECK(Near(Observe(fixture.allocator, fixture.task.Id(), "").fraction, 1.0));
        REQUIRE(engine.Shutdown());
    }
}

} // namespace engine_progress_tests_file
