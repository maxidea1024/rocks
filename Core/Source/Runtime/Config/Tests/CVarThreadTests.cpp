#include "ConfigTestSupport.h"

#include <Config/CVar.h>
#include <Platform/Process.h>
#include <Platform/Sync.h>
#include <Platform/Thread.h>
#include <TestSupport/HostFailure.h>

// CVar CVR2 여러 스레드 시험이다(Docs/Design/CVarSystem.ko.md 10.1·19절). 순서는 모두 명시적
// gate(semaphore)로 만들고 sleep·경쟁 운에 기대지 않는다.
// - 스트레스: 게임 스레드가 프레임마다 변수 둘을 짝(a = n, b = -n)으로 바꿔 Sync하고, 렌더 스레드가
//   프레임 gate마다 그 프레임 snapshot에 묶어 읽는다(다음 프레임의 Sync와 겹친다). 짝 어긋남 0건.
// - 묶음 수명: 작업 스레드가 묶은 snapshot은 그 사이 여러 Sync가 지나도 살아 있고 값이 그대로다.
// - 동시 실행: 다른 스레드의 Set과 게임 lane의 Register·Sync를 엇갈려 돌린다. 이전 snapshot에서 새
//   변수는 기본값이다.
// - 사망 시험(자식 프로세스): 게임 lane의 등록 안 된 핸들 Get, 묶음 없는 Get, 묶음이 남은 채 문맥
// 파괴.
using namespace rocks;
using namespace rocks::config;
using namespace rocks::test::config;

namespace rocks::test {
bool RegisterProcessProbe(const char* mode, int (*function)(int, char**) noexcept) noexcept;
} // namespace rocks::test

namespace cvar_thread_test {

constinit CVar<int32> PairA = cvar::Var("game.pair.a", 0).Help("Pair first half");

constinit CVar<int32> PairB = cvar::Var("game.pair.b", 0).Help("Pair second half");

constinit CVar<int32> Written = cvar::Var("game.pair.written", 0).Help("Written from a worker");

constinit CVar<int32> Arrived =
    cvar::Var("game.pair.arrived", 42).Help("Registered while a worker writes");

constinit CVar<int32> Orphan = cvar::Var("game.pair.orphan", 1).Help("Never registered");

} // namespace cvar_thread_test

namespace {

using namespace cvar_thread_test;

constexpr int32 StressFrames = 200;

// 게임 스레드가 렌더 스레드에 프레임 snapshot을 넘기는 자리다.
struct RenderMailbox {
    platform::Semaphore ready{0, 1};
    platform::Semaphore done{0, 1};
    SharedPtr<const ConfigSnapshot> snapshot;
    bool stop = false;
    int32 frames = 0;
    int32 mismatches = 0;
    int32 lastA = 0;
};

void RenderLoop(void* context) noexcept {
    auto& mailbox = *static_cast<RenderMailbox*>(context);
    for (;;) {
        if (mailbox.ready.Wait() != platform::WaitResult::Signaled) {
            ++mailbox.mismatches;
            return;
        }

        if (mailbox.stop) {
            return;
        }

        {
            // 이 프레임 snapshot에 묶는다. 묶은 동안 게임 스레드는 다음 프레임을 Sync한다.
            cvar::SnapshotBinding bind{mailbox.snapshot};
            mailbox.snapshot = SharedPtr<const ConfigSnapshot>();
            mailbox.done.Release();
            for (int32 read = 0; read < 64; ++read) {
                const int32 a = PairA.Get();
                const int32 b = PairB.Get();
                mailbox.mismatches += a == -b ? 0 : 1;
                mailbox.lastA = a;
            }

            ++mailbox.frames;
        }
    }
}

} // namespace

// 짝으로 바꾼 두 변수를 다른 스레드가 같은 snapshot에서 읽으면 늘 짝이 맞는다(10절 — 같은 Sync의
// 변경은 같은 snapshot에 함께 든다).
TEST(CVar_StressPairsStayConsistentAcrossThreads) {
    ConfigFixture fixture;
    ConfigContext& context = fixture.Context();
    REQUIRE(context.Register(PairA, PairB));
    RenderMailbox mailbox;
    platform::Thread render;
    REQUIRE(render.Start(&RenderLoop, &mailbox) == platform::ThreadStatus::Success);
    for (int32 frame = 1; frame <= StressFrames; ++frame) {
        REQUIRE(PairA.Set(frame));
        REQUIRE(PairB.Set(-frame));
        REQUIRE(context.Sync());
        CHECK(PairA.Get() == frame && PairB.Get() == -frame);
        // 앞 프레임을 읽는 렌더 스레드가 묶음을 잡은 뒤에 이 프레임을 넘긴다.
        if (frame > 1) {
            REQUIRE(mailbox.done.Wait() == platform::WaitResult::Signaled);
        }

        mailbox.snapshot = context.Acquire();
        mailbox.ready.Release();
    }

    REQUIRE(mailbox.done.Wait() == platform::WaitResult::Signaled);
    mailbox.stop = true;
    mailbox.ready.Release();
    REQUIRE(render.Join() == platform::ThreadStatus::Success);
    CHECK(mailbox.frames == StressFrames);
    CHECK(mailbox.mismatches == 0);
    CHECK(mailbox.lastA == StressFrames);
}

namespace {

// 묶음을 쥐고 게임 스레드가 여러 프레임을 지나도록 기다리는 작업이다.
struct LongTask {
    SharedPtr<const ConfigSnapshot> snapshot;
    platform::Semaphore bound{0, 1};
    platform::Semaphore release{0, 1};
    int32 before = 0;
    int32 after = 0;
};

void RunLongTask(void* context) noexcept {
    auto& task = *static_cast<LongTask*>(context);
    cvar::SnapshotBinding bind{Move(task.snapshot)};
    task.before = PairA.Get();
    task.bound.Release();
    static_cast<void>(task.release.Wait());
    task.after = PairA.Get();
}

} // namespace

// 작업이 묶은 snapshot은 프레임보다 오래 살고(참조를 쥔다), 그 사이 Sync가 바꾼 값을 보지 않는다.
TEST(CVar_BindingOutlivesFrames) {
    ConfigFixture fixture;
    ConfigContext& context = fixture.Context();
    REQUIRE(context.Register(PairA, PairB));
    REQUIRE(PairA.Set(1));
    REQUIRE(context.Sync());
    LongTask task;
    task.snapshot = context.Acquire();
    WeakPtr<const ConfigSnapshot> watched(task.snapshot);
    platform::Thread worker;
    REQUIRE(worker.Start(&RunLongTask, &task) == platform::ThreadStatus::Success);
    REQUIRE(task.bound.Wait() == platform::WaitResult::Signaled);
    for (int32 frame = 2; frame < 6; ++frame) {
        REQUIRE(PairA.Set(frame));
        REQUIRE(context.Sync());
    }

    CHECK(PairA.Get() == 5);
    CHECK(watched.Lock().Get() != nullptr);
    task.release.Release();
    REQUIRE(worker.Join() == platform::ThreadStatus::Success);
    CHECK(task.before == 1 && task.after == 1);
    CHECK(watched.Lock().Get() == nullptr);
}

namespace {

// 다른 스레드가 묶은 snapshot으로 Set을 넣는 작업이다.
struct Writer {
    SharedPtr<const ConfigSnapshot> snapshot;
    platform::Semaphore start{0, 1};
    platform::Semaphore running{0, 1};
    platform::Semaphore wrote{0, 1};
    int32 failures = 0;
    int32 orphanBefore = 0;
    int32 arrivedOld = 0;
};

void RunWriter(void* context) noexcept {
    auto& writer = *static_cast<Writer*>(context);
    cvar::SnapshotBinding bind{Move(writer.snapshot)};
    // 등록 안 된 핸들은 게임 lane이 아니므로 기본값이다(5절).
    writer.orphanBefore = Orphan.Get();
    static_cast<void>(writer.start.Wait());
    // 이 snapshot은 Arrived 등록 전 것이므로 칸이 없다 — 기본값이다.
    writer.arrivedOld = Arrived.Get();
    for (int32 value = 1; value <= 100; ++value) {
        writer.failures += Written.Set(value) ? 0 : 1;
        if (value == 1) {
            // 첫 쓰기를 넣은 뒤 게임 lane이 등록·Sync를 시작하게 한다(나머지 쓰기와 겹친다).
            writer.running.Release();
        }
    }

    writer.wrote.Release();
}

} // namespace

// 다른 스레드의 Set과 게임 lane의 Register·Sync가 엇갈려도 넣은 순서대로 적용된다. 등록 직후 같은
// 프레임에 Get할 수 있고(대기 값 적용), 등록 전 snapshot에서는 새 변수가 기본값이다.
TEST(CVar_OtherThreadSetsInterleaveWithRegisterAndSync) {
    ConfigFixture fixture;
    ConfigContext& context = fixture.Context();
    REQUIRE(context.Register(Written));
    REQUIRE(context.AddAssignment(ConfigLayer::Project, "game", "game.pair.arrived", "7"));
    REQUIRE(context.Sync());
    Writer writer;
    writer.snapshot = context.Acquire();
    platform::Thread thread;
    REQUIRE(thread.Start(&RunWriter, &writer) == platform::ThreadStatus::Success);
    writer.start.Release();
    REQUIRE(writer.running.Wait() == platform::WaitResult::Signaled);
    // 작업이 쓰는 동안 게임 lane은 등록하고 Sync를 돈다(어느 프레임에 들어가든 값은 넣은 순서다).
    REQUIRE(context.Register(Arrived));
    CHECK(Arrived.Get() == 7);
    REQUIRE(context.Sync());
    REQUIRE(writer.wrote.Wait() == platform::WaitResult::Signaled);
    REQUIRE(thread.Join() == platform::ThreadStatus::Success);
    REQUIRE(context.Sync());
    CHECK(writer.failures == 0 && writer.orphanBefore == 1 && writer.arrivedOld == 42);
    CHECK(Written.Get() == 100);
}

namespace {

// 다른 스레드가 게시와 겨루며 Acquire하는 작업이다.
struct Acquirer {
    ConfigContext* context = nullptr;
    platform::Semaphore start{0, 1};
    int32 nulls = 0;
    int32 backwards = 0;
    uint64 last = 0;
};

void RunAcquirer(void* context) noexcept {
    auto& acquirer = *static_cast<Acquirer*>(context);
    static_cast<void>(acquirer.start.Wait());
    for (int32 round = 0; round < 2000; ++round) {
        SharedPtr<const ConfigSnapshot> snapshot = acquirer.context->Acquire();
        if (snapshot.Get() == nullptr) {
            ++acquirer.nulls;
            continue;
        }

        acquirer.backwards += snapshot->Generation() < acquirer.last ? 1 : 0;
        acquirer.last = snapshot->Generation();
    }
}

// 다른 스레드의 묶음 없는 Set·큐 한도다.
struct Flooder {
    SharedPtr<const ConfigSnapshot> snapshot;
    ErrorCode unbound = ErrorCode::None;
    ErrorCode overflow = ErrorCode::None;
    usize accepted = 0;
};

void RunFlooder(void* context) noexcept {
    auto& flooder = *static_cast<Flooder*>(context);
    auto unbound = Written.Set(1);
    flooder.unbound = unbound ? ErrorCode::None : unbound.Error();
    cvar::SnapshotBinding bind{Move(flooder.snapshot)};
    for (usize index = 0; index <= MaxQueuedChanges; ++index) {
        auto written = Written.Set(static_cast<int32>(index % 1000));
        if (!written) {
            flooder.overflow = written.Error();
            break;
        }

        ++flooder.accepted;
    }
}

// 다른 스레드에서 범위 임시 변경을 없애는 작업이다.
void DestroyOverride(void* context) noexcept {
    *static_cast<UniquePtr<cvar::ScopedOverride<int32>>*>(context) = nullptr;
}

} // namespace

// 다른 스레드의 Acquire는 게시와 겨뤄도 null이 아니고 세대가 뒤로 가지 않는다.
TEST(CVar_AcquireRacesPublish) {
    ConfigFixture fixture;
    ConfigContext& context = fixture.Context();
    REQUIRE(context.Register(PairA));
    Acquirer acquirer;
    acquirer.context = &context;
    platform::Thread thread;
    REQUIRE(thread.Start(&RunAcquirer, &acquirer) == platform::ThreadStatus::Success);
    acquirer.start.Release();
    for (int32 frame = 1; frame <= 200; ++frame) {
        REQUIRE(PairA.Set(frame));
        REQUIRE(context.Sync());
    }

    REQUIRE(thread.Join() == platform::ThreadStatus::Success);
    CHECK(acquirer.nulls == 0 && acquirer.backwards == 0);
}

// 묶음 없는 스레드의 Set은 NotFound, 다른 스레드의 쓰기는 큐 한도를 넘으면 Busy다(버리지 않는다 —
// 넣은 것은 다음 Sync가 적용한다). 소유 스레드의 입력은 한도와 무관하다.
TEST(CVar_OtherThreadQueueLimitAndUnboundSet) {
    ConfigFixture fixture;
    ConfigContext& context = fixture.Context();
    REQUIRE(context.Register(Written));
    Flooder flooder;
    flooder.snapshot = context.Acquire();
    platform::Thread thread;
    REQUIRE(thread.Start(&RunFlooder, &flooder) == platform::ThreadStatus::Success);
    REQUIRE(thread.Join() == platform::ThreadStatus::Success);
    CHECK(flooder.unbound == ErrorCode::NotFound);
    CHECK(flooder.overflow == ErrorCode::Busy);
    CHECK(flooder.accepted == MaxQueuedChanges);
    // 소유 스레드의 쓰기는 한도와 무관하게 받는다(넣은 쓰기 6만여 개는 적용하지 않고 문맥과 함께
    // 버린다 — 적용 비용이 이 시험의 대상이 아니다).
    REQUIRE(Written.Set(5));
    REQUIRE(context.AddAssignment(ConfigLayer::Console, "console", "game.pair.written", "6"));
    REQUIRE(context.HasPendingChanges());
}

// 범위 임시 변경은 다른 스레드에서 없애도 값을 쓴 문맥에서 지워진다.
TEST(CVar_ScopedOverrideEndsOnAnotherThread) {
    ConfigFixture fixture;
    ConfigContext& context = fixture.Context();
    REQUIRE(context.Register(Written));
    auto scoped = UniquePtr<cvar::ScopedOverride<int32>>::TryMake(fixture.system, Written, 9);
    REQUIRE(scoped && scoped.Value()->Status());
    REQUIRE(context.Sync());
    CHECK(Written.Get() == 9);
    platform::Thread thread;
    REQUIRE(thread.Start(&DestroyOverride, &scoped.Value()) == platform::ThreadStatus::Success);
    REQUIRE(thread.Join() == platform::ThreadStatus::Success);
    REQUIRE(context.Sync());
    CHECK(Written.Get() == 0);
}

namespace {

// 사망 시험 자식이다(PrepareFatalTestProcess로 오류 대화상자를 막는다).
int CVarDeathProbe(int argc, char** argv) noexcept {
    rocks::test::PrepareFatalTestProcess();
    if (argc < 4) {
        return 20;
    }

    const char* scenario = argv[3];
    ConfigFixture fixture;
    if (strcmp(scenario, "unregistered") == 0) {
        static_cast<void>(Orphan.Get());
        return 0;
    }

    if (strcmp(scenario, "unbound") == 0) {
        struct Reader {
            static void Run(void*) noexcept {
                static_cast<void>(PairA.Get());
            }
        };

        platform::Thread thread;
        if (thread.Start(&Reader::Run, nullptr) != platform::ThreadStatus::Success) {
            return 21;
        }

        static_cast<void>(thread.Join());
        return 0;
    }

    if (strcmp(scenario, "binding-out-of-order") == 0) {
        // 같은 snapshot을 두 번 묶고 바깥 것부터 푼다.
        SharedPtr<const ConfigSnapshot> snapshot = fixture.Context().Acquire();
        auto outer = UniquePtr<cvar::SnapshotBinding>::TryMake(fixture.system, snapshot);
        auto inner = UniquePtr<cvar::SnapshotBinding>::TryMake(fixture.system, snapshot);
        if (!outer || !inner) {
            return 24;
        }

        outer.Value() = nullptr;
        return 0;
    }

    if (strcmp(scenario, "destroyed-off-owner") == 0) {
        struct Destroyer {
            static void Run(void* context) noexcept {
                *static_cast<UniquePtr<ConfigContext>*>(context) = nullptr;
            }
        };

        auto made = UniquePtr<ConfigContext>::TryMake(fixture.system, fixture.system,
                                                      fixture.objects, ConfigContextOptions{});
        if (!made || !made.Value()->Initialize()) {
            return 25;
        }

        platform::Thread thread;
        if (thread.Start(&Destroyer::Run, &made.Value()) != platform::ThreadStatus::Success) {
            return 26;
        }

        static_cast<void>(thread.Join());
        return 0;
    }

    if (strcmp(scenario, "binding-outlives-context") == 0) {
        auto made = UniquePtr<ConfigContext>::TryMake(fixture.system, fixture.system,
                                                      fixture.objects, ConfigContextOptions{});
        if (!made || !made.Value()->Initialize()) {
            return 22;
        }

        cvar::SnapshotBinding bind{made.Value()->Acquire()};
        made.Value() = nullptr;
        return 0;
    }

    return 23;
}

const bool CVarDeathProbeRegistered =
    rocks::test::RegisterProcessProbe("cvar-death", &CVarDeathProbe);

struct DeathRun {
    bool ran = false;
    bool abnormal = false;
    char error[4096] = {};
};

DeathRun RunDeathProbe(const char* scenario) noexcept {
    DeathRun result;
    char executable[4096];
    usize executableBytes = 0;
    if (!platform::GetExecutablePath(executable, sizeof(executable), executableBytes)) {
        return result;
    }

    const platform::ProcessArgument arguments[] = {
        {"--process-probe", 15}, {"cvar-death", 10}, {scenario, strlen(scenario)}};
    platform::ProcessStartInfo info;
    info.executable = executable;
    info.executableBytes = executableBytes;
    info.arguments = arguments;
    info.argumentCount = 3;
    info.output = platform::ProcessStreamMode::Null;
    info.error = platform::ProcessStreamMode::Pipe;
    platform::ProcessRunOptions options;
    options.error = result.error;
    options.errorCapacity = sizeof(result.error) - 1;
    const platform::ProcessRunResult run = platform::RunProcess(info, options);
    if (!run.status || run.timedOut) {
        return result;
    }

    result.ran = true;
    result
        .error[run.errorBytes < sizeof(result.error) ? run.errorBytes : sizeof(result.error) - 1] =
        '\0';
    result.abnormal = run.exit.kind != platform::ProcessExitKind::Exited || run.exit.code != 0;
    return result;
}

} // namespace

// 게임 lane의 등록 안 된 핸들 Get, 묶음 없는 스레드의 Get, 묶음이 남은 채 문맥 파괴는 Fatal이다.
TEST(CVar_MisuseIsFatal) {
    struct Case {
        const char* scenario;
        const char* message;
    };

    const Case cases[] = {
        {"unregistered", "CVar 'game.pair.orphan' is not registered"},
        {"unbound", "read on a thread without a snapshot binding"},
        {"binding-outlives-context", "ConfigContext destroyed while cvar::SnapshotBinding"},
        {"binding-out-of-order",
         "cvar::SnapshotBinding released on another thread or out of order"},
        {"destroyed-off-owner", "ConfigContext destroyed on a thread other than its owner"},
    };
    CHECK(CVarDeathProbeRegistered);
    for (const Case& entry : cases) {
        const DeathRun run = RunDeathProbe(entry.scenario);
        REQUIRE(run.ran);
        CHECK(run.abnormal);
        if (strstr(run.error, entry.message) == nullptr) {
            printf("scenario %s stderr: %s\n", entry.scenario, run.error);
            CHECK(false);
        }
    }
}
