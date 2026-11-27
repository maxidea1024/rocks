#include <Core/Memory/SystemAllocator.h>
#include <Core/Move.h>
#include <Engine/CountedPendingWork.h>
#include <Engine/Engine.h>
#include <Engine/PendingWorkRegistry.h>
#include <Localization/Text.h>
#include <Platform/Atomics.h>
#include <Platform/Thread.h>
#include <Progress/ProgressHub.h>
#include <TestSupport/Test.h>
#include <TestSupport/TestGate.h>

using namespace rocks;
namespace counted_pending_work_tests_file {

// 수로 추적하는 대기 작업 종류 시험(Docs/Design/ProgressReporting 8장 PG5 연결, 14장 28). 다른
// 스레드의 일 구간은 명시 gate로 순서를 정한다.
constexpr StringView ShadersName = "Shaders";
constexpr StringView TexturesName = "Textures";
constexpr StringView AfterTextures[] = {TexturesName};

struct Fixture {
    explicit Fixture(Allocator& allocator) noexcept {
        auto text = Text::Invariant(allocator, "Shaders");
        ready = text && static_cast<bool>(work.Initialize(allocator, ShadersName, text.Value(),
                                                          Span<const StringView>(AfterTextures)));
    }

    CountedPendingWork work;
    bool ready = false;
};

// 다른 스레드에서 일 구간을 열고 gate를 기다린 뒤 끝낸다.
struct Worker {
    CountedPendingWork* work = nullptr;
    uint64 jobs = 0;
    // 구간을 열고 일을 더했다.
    test::TestGate started;
    // 시험이 끝내라고 연다(waitForCancel이면 쓰지 않는다).
    test::TestGate go;
    // Shutdown 시험: go 대신 token 취소를 기다린다.
    bool waitForCancel = false;
    // 구간을 닫기 직전에 세운다.
    platform::Atomic<uint32> finished{0};

    static void Run(void* context) noexcept {
        auto& self = *static_cast<Worker*>(context);
        CountedPendingWork::WorkScope scope = self.work->BeginWork();
        self.work->AddWork(self.jobs);
        self.started.Open();
        if (self.waitForCancel) {
            // 취소는 Shutdown이 반드시 건다(상태로 판정 — 시간 상한 없음).
            const CancelToken token = self.work->Token();
            while (!token.IsCancellationRequested()) {
                platform::YieldThread();
            }
        } else {
            self.go.Wait();
        }

        self.work->FinishWork(self.jobs);
        self.finished.Store<platform::MemoryOrder::Release>(1u);
    }
};

// 남은 수·일 중의 1·계약 함수·등록소 합계다.
TEST(PendingWork_CountedRemainingAndContract) {
    SystemAllocator allocator;
    Fixture fixture(allocator);
    REQUIRE(fixture.ready);
    CountedPendingWork& work = fixture.work;
    auto text = Text::Invariant(allocator, "Again");
    REQUIRE(text);
    CHECK(work.Initialize(allocator, ShadersName, text.Value()).Error() ==
          ErrorCode::AlreadyExists);
    CountedPendingWork unnamed;
    CHECK(unnamed.Initialize(allocator, StringView(), text.Value()).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(work.Name() == ShadersName);
    CHECK(work.DisplayName().IsValid());
    REQUIRE(work.RunBefore().Size() == 1);
    CHECK(work.RunBefore()[0] == TexturesName);
    CHECK(work.RemainingCount() == 0);
    work.AddWork(5);
    work.FinishWork(2);
    CHECK(work.RemainingCount() == 3);
    work.FinishWork(3);
    CHECK(work.RemainingCount() == 0);
    {
        // 일 중에는 남은 수가 0이어도 1이다(UE GetNumRemainingAssets).
        CountedPendingWork::WorkScope scope = work.BeginWork();
        CHECK(work.ActiveWork() == 1);
        CHECK(work.RemainingCount() == 1);
        CountedPendingWork::WorkScope moved = Move(scope);
        CHECK(work.ActiveWork() == 1);
    }

    CHECK(work.ActiveWork() == 0);
    CHECK(work.RemainingCount() == 0);
    CHECK(work.FinishFor({}));
    work.CancelFor({});
    work.ApplyCompleted({});
    CHECK(!work.Token().IsCancellationRequested());

    PendingWorkRegistry registry(allocator);
    REQUIRE(registry.Register(work));
    work.AddWork(7);
    CHECK(registry.RemainingCount() == 7);
    work.FinishWork(7);
    CHECK(registry.RemainingCount() == 0);
    REQUIRE(registry.Unregister(work));
}

// FinishAll은 다른 스레드의 일 구간이 닫힐 때까지 막는다(그 스레드의 마지막 일까지 끝난 뒤
// 돌아온다). 등록소가 연 막는 작업 아래에서 돈다.
TEST(PendingWork_CountedFinishAllWaitsForOpenWork) {
    SystemAllocator allocator;
    REQUIRE(ProgressHub::Get().Initialize(allocator, {}));
    {
        Fixture fixture(allocator);
        REQUIRE(fixture.ready);
        PendingWorkRegistry registry(allocator);
        REQUIRE(registry.Register(fixture.work));
        Worker worker;
        worker.work = &fixture.work;
        worker.jobs = 4;
        platform::Thread thread;
        REQUIRE(thread.Start(&Worker::Run, &worker) == platform::ThreadStatus::Success);
        worker.started.Wait();
        CHECK(registry.RemainingCount() == 4);
        worker.go.Open();
        CHECK(registry.FinishAll());
        CHECK(worker.finished.Load<platform::MemoryOrder::Acquire>() == 1u);
        CHECK(fixture.work.ActiveWork() == 0);
        CHECK(registry.RemainingCount() == 0);
        CHECK(thread.Join() == platform::ThreadStatus::Success);
        // 할 일이 없으면 바로 돌아온다.
        CHECK(registry.FinishAll());
        CHECK(ProgressHub::Get().Stats().invalidWorkWarnings == 0);
        REQUIRE(registry.Unregister(fixture.work));
    }

    ProgressHub::Get().Shutdown();
}

// Shutdown은 token을 취소하고 일 구간이 닫힐 때까지 기다린다. 두 번째는 기다리기만 하고, 그 뒤의
// 일은 시작하자마자 취소를 본다. 등록소 Shutdown도 같은 길이다.
TEST(PendingWork_CountedShutdownCancelsAndWaits) {
    SystemAllocator allocator;
    Fixture fixture(allocator);
    REQUIRE(fixture.ready);
    PendingWorkRegistry registry(allocator);
    REQUIRE(registry.Register(fixture.work));
    Worker worker;
    worker.work = &fixture.work;
    worker.jobs = 2;
    worker.waitForCancel = true;
    platform::Thread thread;
    REQUIRE(thread.Start(&Worker::Run, &worker) == platform::ThreadStatus::Success);
    worker.started.Wait();
    registry.Shutdown();
    CHECK(worker.finished.Load<platform::MemoryOrder::Acquire>() == 1u);
    CHECK(fixture.work.Token().IsCancellationRequested());
    CHECK(fixture.work.ActiveWork() == 0);
    CHECK(thread.Join() == platform::ThreadStatus::Success);
    fixture.work.Shutdown();
    {
        CountedPendingWork::WorkScope late = fixture.work.BeginWork();
        CHECK(fixture.work.Token().IsCancellationRequested());
    }

    CHECK(registry.FinishAll());
    REQUIRE(registry.Unregister(fixture.work));
}

// Engine은 엔진 묶음 동안 등록소를 호스트 서비스로 올린다. 같은 로더의 둘째 Engine은 올리지 않고,
// 올린 Engine이 끝나면 서비스가 사라진다.
TEST(PendingWork_EngineOffersRegistryAsHostService) {
    SystemAllocator allocator;
    ModuleLoader& modules = test::ProcessModules();
    CHECK(modules.Services().Find<PendingWorkRegistry>() == nullptr);
    EngineConfig config;
    config.modules = &modules;
    {
        Engine first(allocator);
        Engine second(allocator);
        REQUIRE(first.Initialize(config));
        CHECK(modules.Services().Find<PendingWorkRegistry>() == &first.PendingWork());
        REQUIRE(second.Initialize(config));
        CHECK(modules.Services().Find<PendingWorkRegistry>() == &first.PendingWork());
        REQUIRE(first.Shutdown());
        CHECK(modules.Services().Find<PendingWorkRegistry>() == nullptr);
        REQUIRE(second.Shutdown());
    }

    CHECK(modules.Services().Find<PendingWorkRegistry>() == nullptr);
    {
        Engine engine(allocator);
        REQUIRE(engine.Initialize(config));
        CHECK(modules.Services().Find<PendingWorkRegistry>() == &engine.PendingWork());
        REQUIRE(engine.Shutdown());
    }

    CHECK(modules.Services().Find<PendingWorkRegistry>() == nullptr);
}

} // namespace counted_pending_work_tests_file
