#include "EngineAssetsTestSupport.h"

#include <Core/Memory/SystemAllocator.h>
#include <Core/Module/ModuleEntry.h>
#include <ModuleLoader/ModuleLoader.h>
#include <Object/ConstructionContext.h>
#include <Platform/Process.h>
#include <Platform/Thread.h>
#include <TestSupport/HostFailure.h>
#include <TestSupport/NFailAllocator.h>
#include <World/World.h>
#include <stdio.h>
#include <string.h>

// EngineAssets 엔진 연결 시험이다(Docs/Design/AssetSystem 11.8절): 디스크 쿠킹 장면을 시험
// 임시 디렉터리에 쓰고 설정 문맥의 `asset.*` CVar로 엔진을 띄운다. 로드 순서는 프레임 밖 동기
// 로드로 상주시킨 뒤 프레임 안에서 요청해 다음 Pump의 callback으로 정한다(디스크 IO가 끝나는
// 프레임에 기대지 않는다). GC는 객체 수 예산으로 조각 수를 정한다.
using namespace rocks;

namespace rocks::test {
bool RegisterProcessProbe(const char* mode,
                          int (*function)(int, const char* const*) noexcept) noexcept;
} // namespace rocks::test

namespace engine_assets_tests_file {

using namespace rocks::asset_test;
using namespace rocks::engine_assets_test;
using rocks::test::config::ConfigFixture;
using rocks::test::config::DevelopmentRules;

namespace {

// 프레임 안 요청·동기 로드 금지·다음 Pump callback을 기록하는 client다.
class FrameClient final : public EngineClient {
public:
    void OnEngineTick(Engine& engine) noexcept override {
        AssetManager& assets = engine.GetSubsystem<EngineAssetsSubsystem>()->Manager();
        const uint64 frame = engine.Time().frameIndex;
        if (requestFrame == 0) {
            requestFrame = frame;
            inFrame = engine.IsInFrame();
            auto blocking = assets.LoadBlocking(RequestOf(1), "EngineAssetsTests.Frame");
            blockingError = blocking ? ErrorCode::None : blocking.Error();
            auto requested = assets.RequestAsset(RequestOf(2));
            if (requested) {
                ticket = Move(requested.Value());
                auto callback = AssetReadyCallback::TryCreate(
                    engine.GetAllocator(),
                    [this, &engine](Result<AssetLease> lease, const AssetDiagnostic&) noexcept {
                        callbackFrame = engine.Time().frameIndex;
                        callbackError = lease ? ErrorCode::None : lease.Error();
                        if (lease) {
                            delivered = Move(lease.Value());
                        }
                    });
                registered = callback &&
                             assets.OnReady(ticket, TaskLane::Game, Move(callback.Value())).IsOk();
            }
        } else if (frame == requestFrame + 1) {
            // 다음 프레임의 client Tick 전(Subsystem.OnFrameBegin Pump)에 callback이 불렸다.
            seenBeforeTick = callbackFrame == frame;
        } else if (frame == requestFrame + 2 && release) {
            // 쥔 것을 모두 놓는다 — 다음 Pump가 Discard하고 그 프레임 끝에서 GC가 회수한다.
            ticket = LoadTicket();
            delivered = AssetLease();
            preloaded = AssetLease();
        }
    }

    void OnEngineShutdown(Engine&) noexcept override {
        ticket = LoadTicket();
        delivered = AssetLease();
        preloaded = AssetLease();
    }

    bool release = false;
    uint64 requestFrame = 0;
    uint64 callbackFrame = 0;
    bool inFrame = false;
    bool registered = false;
    bool seenBeforeTick = false;
    ErrorCode blockingError = ErrorCode::None;
    ErrorCode callbackError = ErrorCode::Cancelled;
    LoadTicket ticket;
    AssetLease delivered;
    AssetLease preloaded;
};

} // namespace

// 엔진 프레임(11.8절 5): 프레임 밖(Run 전) LoadBlocking은 디스크에서 끝까지 로드해 lease를 준다.
// 프레임 안(client Tick)에서는 IsInFrame이 참이고 LoadBlocking이 WrongContext이며, 상주 에셋의
// 요청 callback은 다음 프레임의 World Tick 전 Pump에서 한 번 불린다. 쿠킹 루트·프로파일은 설정 값
// 그대로다.
TEST(EngineAssets_LoadsThroughEngineFrames) {
    DiskScene disk("frames");
    REQUIRE(disk.Write());
    ConfigFixture fixture(DevelopmentRules());
    REQUIRE(PrepareSettings(fixture.Context(), disk.Root()));
    SystemAllocator allocator;
    FrameClient client;
    const EngineSubsystemType subsystems[] = {EngineAssetsSubsystemType()};
    EngineConfig config = MakeEngineConfig(&fixture.Context(), subsystems, &client);
    config.maxFrames = TestFrames;
    Engine engine(allocator);
    REQUIRE(engine.Initialize(config));
    EngineAssetsSubsystem* assets = engine.GetSubsystem<EngineAssetsSubsystem>();
    REQUIRE(assets != nullptr);
    CHECK(assets->CookedRoot() == disk.Root());
    CHECK(assets->RuntimeProfile() == "Windows");
    CHECK(!engine.IsInFrame());
    auto preloaded = assets->Manager().LoadBlocking(RequestOf(2), "EngineAssetsTests.Preload");
    REQUIRE(preloaded);
    client.preloaded = Move(preloaded.Value());
    REQUIRE(engine.Run());
    CHECK(client.inFrame);
    CHECK(client.blockingError == ErrorCode::WrongContext);
    CHECK(client.registered && client.callbackError == ErrorCode::None);
    CHECK(client.callbackFrame == client.requestFrame + 1 && client.seenBeforeTick);
    CHECK(client.delivered.IsValid() && client.delivered.Asset() == FixedId(2));
    CHECK(engine.Shutdown());
}

// GC 조합(11.8절 5, 14 완료 조건 2): 표식 scheduler 2(엔진 gcWorkers 0 — 직렬·2 — 엔진 GC
// scheduler로 병렬 표식) × 조각 크기 2(객체 수 예산 무제한·1) × cycle 도중 Discard 2(없음·있음) ×
// 생성 트랜잭션이 열린 프레임 2(없음·있음) = 16. 모두 놓은 단위는 다음 Pump에
// Discard되고 그 프레임 끝에서 cycle이 시작된다(Discard 주도 — 그 전 프레임은 수집하지 않는다).
// 열린 트랜잭션 프레임은 조각을 건너뛰고(Busy 횟수), cycle 도중 Discard가 있으면 끝난 뒤 한 번 더
// 돈다. 예산 1은 무제한(표식 → 회수 두 단계)보다 많은 프레임에 걸친다. 끝나면 살아 있는 객체가 줄고
// 실패가 없다.
TEST(EngineAssets_GarbageCollectionCombinations) {
    constexpr uint32 FrameLimit = 64;
    constexpr uint32 ParallelGcWorkers = 2;
    uint32 cases = 0;
    uint32 framesWhole[2] = {};
    uint32 framesSliced[2] = {};
    for (uint32 parallel = 0; parallel < 2; ++parallel) {
        const uint32 gcWorkers = parallel != 0 ? ParallelGcWorkers : 0;
        for (uint32 sliced = 0; sliced < 2; ++sliced) {
            for (uint32 discardDuringCycle = 0; discardDuringCycle < 2; ++discardDuringCycle) {
                for (uint32 busyFrame = 0; busyFrame < 2; ++busyFrame) {
                    char tag[32] = {};
                    snprintf(tag, sizeof(tag), "gc-%u%u%u%u", parallel, sliced, discardDuringCycle,
                             busyFrame);
                    DiskScene disk(tag);
                    REQUIRE(disk.Write());
                    ConfigFixture fixture(DevelopmentRules());
                    config::ConfigContext& settings = fixture.Context();
                    REQUIRE(PrepareSettings(settings, disk.Root()));
                    REQUIRE(settings.AddAssignment(config::ConfigLayer::Console, "test",
                                                   "asset.gc.time_ms", "60000"));
                    if (sliced != 0) {
                        REQUIRE(settings.AddAssignment(config::ConfigLayer::Console, "test",
                                                       "asset.gc.max_objects", "1"));
                    }

                    SystemAllocator allocator;
                    FrameClient client;
                    client.release = true;
                    const EngineSubsystemType subsystems[] = {EngineAssetsSubsystemType()};
                    Engine engine(allocator);
                    REQUIRE(engine.Initialize(
                        MakeEngineConfig(&settings, subsystems, &client, gcWorkers)));
                    // 병렬 축은 엔진 GC scheduler가 있고 worker가 그 수다(EngineAssets가 그것을
                    // StepCollection에 넘긴다).
                    WorkerScheduler* gcScheduler = engine.GcScheduler();
                    CHECK((gcScheduler != nullptr) == (parallel != 0));
                    CHECK(gcScheduler == nullptr || gcScheduler->Stats().workerCount == gcWorkers);
                    EngineAssetsSubsystem* assets = engine.GetSubsystem<EngineAssetsSubsystem>();
                    ObjectRegistry& registry = assets->Manager().Registry();
                    auto preloaded =
                        assets->Manager().LoadBlocking(RequestOf(2), "EngineAssetsTests.Preload");
                    auto extra =
                        assets->Manager().LoadBlocking(RequestOf(3), "EngineAssetsTests.Extra");
                    REQUIRE(preloaded && extra);
                    client.preloaded = Move(preloaded.Value());
                    const uint32 liveBefore = registry.Stats().liveObjects;
                    // 요청 프레임·callback 프레임·놓는 프레임까지는 수집하지 않는다.
                    for (uint32 frame = 0; frame < 3; ++frame) {
                        REQUIRE(engine.RunFrame());
                        CHECK(assets->Stats().gcCycles == 0 && !assets->Stats().gcPending);
                    }

                    // 다음 프레임 Pump가 Discard하고 그 프레임 끝에서 cycle이 시작된다.
                    REQUIRE(engine.RunFrame());
                    uint32 frames = 1;
                    CHECK(assets->Stats().gcPending);
                    REQUIRE(registry.CollectionPhase() != GcPhase::Idle);
                    if (discardDuringCycle != 0) {
                        extra.Value() = AssetLease();
                    }

                    if (busyFrame != 0) {
                        ConstructionContext transaction(registry, registry.OwnerAccess());
                        REQUIRE(engine.RunFrame());
                        ++frames;
                        CHECK(assets->Stats().gcBusySkips >= 1 && assets->Stats().gcCycles == 0);
                    }

                    while (assets->Stats().gcCycles == 0 && frames < FrameLimit) {
                        REQUIRE(engine.RunFrame());
                        ++frames;
                    }

                    CHECK(assets->Stats().gcCycles == 1);
                    if (discardDuringCycle == 0 && busyFrame == 0) {
                        (sliced == 0 ? framesWhole : framesSliced)[parallel] = frames;
                    }

                    // cycle 도중 Discard가 있었으면 수집 요청이 남아 둘째 cycle이 돈다.
                    CHECK(assets->Stats().gcPending == (discardDuringCycle != 0));
                    const uint64 expectedCycles = 1 + discardDuringCycle;
                    for (uint32 frame = 0;
                         frame < FrameLimit && assets->Stats().gcCycles < expectedCycles; ++frame) {
                        REQUIRE(engine.RunFrame());
                    }

                    CHECK(assets->Stats().gcCycles == expectedCycles && !assets->Stats().gcPending);
                    CHECK((assets->Stats().gcBusySkips != 0) == (busyFrame != 0));
                    CHECK(assets->Stats().gcFailures == 0 && !assets->Stats().lingering);
                    // 병렬 축은 모든 조각을 엔진 GC scheduler로 돌렸다.
                    CHECK((assets->Stats().gcParallelSteps != 0) == (parallel != 0));
                    CHECK(registry.Stats().liveObjects < liveBefore);
                    extra.Value() = AssetLease();
                    CHECK(engine.Shutdown());
                    ++cases;
                }
            }
        }
    }

    CHECK(cases == 16);
    CHECK(framesSliced[0] > framesWhole[0]);
    CHECK(framesSliced[1] > framesWhole[1]);
}

// 초기화 조합(11.8절 4, 14 완료 조건 2): 매니페스트 상태 6(정상·없음·자리가 디렉터리·빈 파일·손상·
// 형식 버전 다름) × 프로파일 2(일치·불일치) = 12. 정상·일치만 성공이고, 매니페스트 실패는
// 프로파일과 무관하게 그 오류(검사 순서 — 매니페스트를 먼저 읽는다), 정상·불일치는 Unsupported(진단
// IncompatibleCook)다. 실패면 엔진 상태가 Failed이고 실패 단계·오류·진단을 담은 Error 로그가 정확히
// 하나이며, 성공이면 시작 Info 로그가 하나다.
TEST(EngineAssets_InitializationCombinations) {
    constexpr const char* MissingManifestStage =
        IsReleaseBuild ? "content signature" : "manifest read";
    enum class ManifestState : uint8 {
        Valid,
        Missing,
        Directory,
        Empty,
        Corrupt,
        OtherVersion
    };
    struct Expected {
        ManifestState state;
        ErrorCode error;
        const char* stage;
        const char* diagnostic;
    };
    const Expected states[] = {
        {ManifestState::Valid, ErrorCode::None, "", ""},
        // 봉투도 rman도 없으면 Release는 봉투 단계에서, 그 밖은 rman 읽기에서 NotFound다.
        {ManifestState::Missing, ErrorCode::NotFound, MissingManifestStage, "None"},
        {ManifestState::Directory, ErrorCode::NotFound, MissingManifestStage, "None"},
        {ManifestState::Empty, ErrorCode::InvalidArgument, "manifest parse", "CorruptPackage"},
        {ManifestState::Corrupt, ErrorCode::InvalidArgument, "manifest parse", "CorruptPackage"},
        {ManifestState::OtherVersion, ErrorCode::Unsupported, "manifest parse",
         "UnsupportedVersion"},
    };
    ManualEngineAssetsLogs logs;
    LogInstallScope installed(logs.logs);
    SystemAllocator allocator;
    const EngineSubsystemType subsystems[] = {EngineAssetsSubsystemType()};
    uint32 cases = 0;
    for (const Expected& expected : states) {
        for (uint32 mismatch = 0; mismatch < 2; ++mismatch) {
            char tag[32] = {};
            snprintf(tag, sizeof(tag), "init-%u%u", static_cast<unsigned>(expected.state),
                     mismatch);
            DiskScene disk(tag);
            REQUIRE(disk.Write(expected.state == ManifestState::Valid ||
                               expected.state == ManifestState::OtherVersion));
            if (expected.state == ManifestState::OtherVersion) {
                // 형식 버전 칸(magic 뒤 LE32)만 바꾼다 — CRC보다 먼저 판정한다.
                Array<uint8> bytes(disk.env.allocator);
                REQUIRE(WriteCookedManifest(disk.env.allocator, disk.scene.manifest, bytes));
                constexpr usize VersionOffset = 4;
                bytes[VersionOffset] = static_cast<uint8>(bytes[VersionOffset] + 1);
                REQUIRE(disk.WriteManifestBytes(bytes.View()));
            } else if (expected.state == ManifestState::Directory) {
                StringBuilder<160> path(disk.env.allocator);
                REQUIRE(path.TryAppend(disk.Root()));
                REQUIRE(path.TryAppend("/"));
                REQUIRE(path.TryAppend(CookedManifestFileName));
                REQUIRE(platform::MakeDirectoryTree(platform::PhysicalFileSystem(),
                                                    path.View().Data(), path.View().ByteCount()));
            } else if (expected.state == ManifestState::Empty) {
                REQUIRE(disk.WriteManifestBytes({}));
            } else if (expected.state == ManifestState::Corrupt) {
                const uint8 garbage[] = {'R', 'M', 'A', 'N', 0, 1, 2, 3};
                REQUIRE(disk.WriteManifestBytes(garbage));
            }

            ConfigFixture fixture(DevelopmentRules());
            REQUIRE(
                PrepareSettings(fixture.Context(), disk.Root(), mismatch != 0 ? "Mac" : "Windows"));
            logs.Pump();
            CapturedLogs.Reset();
            Engine engine(allocator);
            const auto initialized =
                engine.Initialize(MakeEngineConfig(&fixture.Context(), subsystems, nullptr));
            const bool incompatible = expected.state == ManifestState::Valid && mismatch != 0;
            const ErrorCode want = incompatible ? ErrorCode::Unsupported : expected.error;
            CHECK((initialized ? ErrorCode::None : initialized.Error()) == want);
            CHECK((engine.State() == EngineState::Failed) == (want != ErrorCode::None));
            logs.Pump();
            if (want == ErrorCode::None) {
                CHECK(CapturedLogs.Count(LogLevel::Error, InitFailedLog) == 0);
                CHECK(CapturedLogs.Count(LogLevel::Info, StartedLog) == 1);
            } else {
                // 실패 단계·오류 이름·진단 이름이 메시지에 그대로 있다.
                char prefix[160] = {};
                snprintf(prefix, sizeof(prefix),
                         "EngineAssets initialization failed at %s: %s (diagnostic %s,",
                         incompatible ? "asset manager" : expected.stage, ErrorCodeName(want),
                         incompatible ? "IncompatibleCook" : expected.diagnostic);
                const char* message = CapturedLogs.Message(LogLevel::Error, InitFailedLog);
                const bool logged = CapturedLogs.Count(LogLevel::Error, InitFailedLog) == 1 &&
                                    strncmp(message, prefix, strlen(prefix)) == 0;
                if (!logged) {
                    printf("  expected prefix: %s\n", prefix);
                    CapturedLogs.Print();
                }

                CHECK(logged);
                CHECK(CapturedLogs.Count(LogLevel::Info, StartedLog) == 0);
            }

            if (initialized) {
                CHECK(engine.Shutdown());
            }

            ++cases;
        }
    }

    CHECK(cases == 12);
}

// 초기화 실패(11.8절 3·4): 설정 문맥 없음 InvalidArgument(그 Error 로그 하나 — CVar를 읽기 전에
// 거절한다), 매니페스트 없음 NotFound, 손상 매니페스트(CorruptPackage), 프로파일 다름
// Unsupported(진단 IncompatibleCook) — 엔진 초기화가 실패하고 상태는 Failed다.
TEST(EngineAssets_InitializationFailures) {
    SystemAllocator allocator;
    const EngineSubsystemType subsystems[] = {EngineAssetsSubsystemType()};
    {
        ManualEngineAssetsLogs logs;
        LogInstallScope installed(logs.logs);
        Engine engine(allocator);
        CHECK(engine.Initialize(MakeEngineConfig(nullptr, subsystems, nullptr)).Error() ==
              ErrorCode::InvalidArgument);
        CHECK(engine.State() == EngineState::Failed);
        logs.Pump();
        CHECK(CapturedLogs.Count(LogLevel::Error, NoSettingsLog) == 1);
        CHECK(CapturedLogs.Count(LogLevel::Error, InitFailedLog) == 0);
    }

    {
        DiskScene disk("no-manifest");
        REQUIRE(disk.Write(false));
        ConfigFixture fixture(DevelopmentRules());
        REQUIRE(PrepareSettings(fixture.Context(), disk.Root()));
        Engine engine(allocator);
        CHECK(
            engine.Initialize(MakeEngineConfig(&fixture.Context(), subsystems, nullptr)).Error() ==
            ErrorCode::NotFound);
    }

    {
        DiskScene disk("corrupt");
        REQUIRE(disk.Write());
        const uint8 garbage[] = {'R', 'M', 'A', 'N', 0, 1, 2, 3};
        REQUIRE(disk.WriteManifestBytes(garbage));
        ConfigFixture fixture(DevelopmentRules());
        REQUIRE(PrepareSettings(fixture.Context(), disk.Root()));
        Engine engine(allocator);
        const auto initialized =
            engine.Initialize(MakeEngineConfig(&fixture.Context(), subsystems, nullptr));
        CHECK(!initialized && engine.State() == EngineState::Failed);
    }

    {
        DiskScene disk("profile");
        REQUIRE(disk.Write());
        ConfigFixture fixture(DevelopmentRules());
        REQUIRE(PrepareSettings(fixture.Context(), disk.Root(), "Mac"));
        Engine engine(allocator);
        CHECK(
            engine.Initialize(MakeEngineConfig(&fixture.Context(), subsystems, nullptr)).Error() ==
            ErrorCode::Unsupported);
    }
}

namespace {

// 마지막 프레임에 상주하지 않은 에셋을 요청한 채 끝나는 client다(종료가 ShuttingDown을 준다).
class PendingClient final : public EngineClient {
public:
    void OnEngineTick(Engine& engine) noexcept override {
        if (requested) {
            return;
        }

        AssetManager& assets = engine.GetSubsystem<EngineAssetsSubsystem>()->Manager();
        auto made = assets.RequestAsset(RequestOf(3));
        if (!made) {
            return;
        }

        ticket = Move(made.Value());
        auto callback = AssetReadyCallback::TryCreate(
            engine.GetAllocator(),
            [this](Result<AssetLease> lease, const AssetDiagnostic&) noexcept {
                ++calls;
                error = lease ? ErrorCode::None : lease.Error();
            });
        requested =
            callback && assets.OnReady(ticket, TaskLane::Game, Move(callback.Value())).IsOk();
    }

    void OnEngineShutdown(Engine&) noexcept override {
        if (keepLease) {
            return;
        }

        ticket = LoadTicket();
        lease = AssetLease();
    }

    bool requested = false;
    bool keepLease = false;
    uint32 calls = 0;
    ErrorCode error = ErrorCode::None;
    LoadTicket ticket;
    AssetLease lease;
};

} // namespace

// 종료(11.8절 6): 마지막 프레임에 낸 요청(아직 Pump 전 — IO 없음)의 callback은 엔진 종료의
// CompleteInFlightWork에서 ShuttingDown을 한 번 받고, client가 OnEngineShutdown에서 handle을 놓으면
// 정상 종료한다.
TEST(EngineAssets_ShutdownDeliversShuttingDown) {
    DiskScene disk("shutdown");
    REQUIRE(disk.Write());
    ConfigFixture fixture(DevelopmentRules());
    REQUIRE(PrepareSettings(fixture.Context(), disk.Root()));
    SystemAllocator allocator;
    PendingClient client;
    const EngineSubsystemType subsystems[] = {EngineAssetsSubsystemType()};
    EngineConfig config = MakeEngineConfig(&fixture.Context(), subsystems, &client);
    config.maxFrames = 1;
    Engine engine(allocator);
    REQUIRE(engine.Initialize(config));
    REQUIRE(engine.Run());
    REQUIRE(client.requested);
    CHECK(client.calls == 0);
    CHECK(engine.Shutdown());
    CHECK(client.calls == 1 && client.error == ErrorCode::ShuttingDown);
}

namespace {

// 종료 계약 위반 probe: client가 lease를 놓지 않은 채 엔진을 끝낸다(Subsystem 정리의 AssetManager
// 파괴가 Fatal). 쿠킹 장면은 부모 시험이 쓰고 지운다(자식은 Fatal로 끝나 정리하지 못한다) — 루트는
// 인자로 받는다.
int EngineAssetsFatalProbe(int argc, const char* const* argv) noexcept {
    rocks::test::PrepareFatalTestProcess();
    constexpr int RootArgument = 3;
    if (argc <= RootArgument) {
        return 2;
    }

    const StringView root(argv[RootArgument], strlen(argv[RootArgument]));
    EngineAssetsSubsystem::SetContentTrust(&TestContentTrust());
    ConfigFixture fixture(DevelopmentRules());
    if (!PrepareSettings(fixture.Context(), root)) {
        return 3;
    }

    SystemAllocator allocator;
    PendingClient client;
    client.keepLease = true;
    const EngineSubsystemType subsystems[] = {EngineAssetsSubsystemType()};
    Engine engine(allocator);
    if (!engine.Initialize(MakeEngineConfig(&fixture.Context(), subsystems, &client))) {
        return 4;
    }

    auto lease = engine.GetSubsystem<EngineAssetsSubsystem>()->Manager().LoadBlocking(
        RequestOf(3), "EngineAssetsTests.Fatal");
    if (!lease) {
        return 5;
    }

    client.lease = Move(lease.Value());
    (void)engine.Shutdown();
    return 0;
}

const bool EngineAssetsProbeRegistered =
    rocks::test::RegisterProcessProbe("engine-assets", &EngineAssetsFatalProbe);

bool ProbeFails(StringView root, const char* message) noexcept {
    char executable[4096];
    usize executableBytes = 0;
    if (!platform::GetExecutablePath(executable, sizeof(executable), executableBytes)) {
        return false;
    }

    const platform::ProcessArgument arguments[] = {
        {"--process-probe", 15}, {"engine-assets", 13}, {root.Data(), root.ByteCount()}};
    platform::ProcessStartInfo info;
    info.executable = executable;
    info.executableBytes = executableBytes;
    info.arguments = arguments;
    info.argumentCount = 3;
    info.output = platform::ProcessStreamMode::Null;
    info.error = platform::ProcessStreamMode::Pipe;
    static char error[8192];
    platform::ProcessRunOptions options;
    options.error = error;
    options.errorCapacity = sizeof(error) - 1;
    const platform::ProcessRunResult run = platform::RunProcess(info, options);
    if (!run.status || run.timedOut) {
        return false;
    }

    error[run.errorBytes < sizeof(error) ? run.errorBytes : sizeof(error) - 1] = '\0';
    const bool abnormal = run.exit.kind != platform::ProcessExitKind::Exited || run.exit.code != 0;
    return abnormal && strstr(error, message) != nullptr;
}

} // namespace

// 종료 계약(11.8절 6): lease를 OnEngineShutdown에서 놓지 않으면 Subsystem 정리의 AssetManager
// 파괴가 자식 프로세스 Fatal로 끝난다(문구 대조).
TEST(EngineAssets_LeaseHeldPastShutdownIsFatal) {
    REQUIRE(EngineAssetsProbeRegistered);
    DiskScene disk("fatal");
    REQUIRE(disk.Write());
    CHECK(ProbeFails(disk.Root(), "AssetManager destroyed while leases"));
}

namespace {

// World Subsystem이 쥔 티켓 시험의 공유 상태다(Subsystem은 allocator만 받고 생성되므로 정적 자리).
struct WorldTicketState {
    Engine* engine = nullptr;
    uint32 calls = 0;
    ErrorCode error = ErrorCode::None;
    bool deinitInFrame = false;
    ErrorCode deinitBlocking = ErrorCode::None;
};

WorldTicketState WorldTicket;

// 상주하지 않은 3을 요청한 채 World와 함께 사라지는 World Subsystem이다.
class TicketWorldSubsystem final : public WorldSubsystem {
public:
    Result<void> Initialize(World&) noexcept override {
        AssetManager& assets = WorldTicket.engine->GetSubsystem<EngineAssetsSubsystem>()->Manager();
        auto made = assets.RequestAsset(RequestOf(3));
        if (!made) {
            return Result<void>::Failure(made.Error());
        }

        ticket = Move(made.Value());
        auto callback = AssetReadyCallback::TryCreate(
            WorldTicket.engine->GetAllocator(),
            [](Result<AssetLease> lease, const AssetDiagnostic&) noexcept {
                ++WorldTicket.calls;
                WorldTicket.error = lease ? ErrorCode::None : lease.Error();
            });
        if (!callback) {
            return Result<void>::Failure(callback.Error());
        }

        return assets.OnReady(ticket, TaskLane::Game, Move(callback.Value()));
    }

    void Deinitialize(World&) noexcept override {
        WorldTicket.deinitInFrame = WorldTicket.engine->IsInFrame();
        // client Tick(3단계)의 즉시 World 파괴도 프레임 안이라 동기 로드를 거절한다.
        auto blocking =
            WorldTicket.engine->GetSubsystem<EngineAssetsSubsystem>()->Manager().LoadBlocking(
                RequestOf(2), "EngineAssetsTests.WorldDeinit");
        WorldTicket.deinitBlocking = blocking ? ErrorCode::None : blocking.Error();
        ticket = LoadTicket();
    }

private:
    LoadTicket ticket;
};

// 1번 프레임에 World를 만들고 2번 프레임에 파괴하는 client다(World Tick 밖이라 그 자리에서 파괴한다
// — Docs/Design/EngineRuntime 18.5절).
class WorldClient final : public EngineClient {
public:
    void OnEngineTick(Engine& engine) noexcept override {
        ++ticks;
        if (ticks == 1) {
            WorldCreateDesc desc;
            desc.name = "Assets";
            desc.subsystems = subsystems;
            auto made = engine.CreateWorld(desc);
            world = made ? made.Value() : nullptr;
        } else if (ticks == 2 && world != nullptr) {
            destroyed = engine.DestroyWorld(world).IsOk();
            world = nullptr;
        }
    }

    Span<const WorldSubsystemType> subsystems;
    World* world = nullptr;
    uint32 ticks = 0;
    bool destroyed = false;
};

} // namespace

// World가 쥔 티켓(11.8절 5·6): World Subsystem이 요청한 티켓은 World 파괴(2번 프레임 client Tick —
// 그동안 IsInFrame은 참이고 LoadBlocking은 WrongContext다)가 놓고, 다음 프레임 Pump가 아직 끝나지
// 않은 그 요청을 취소해 callback이 Cancelled를 한 번 받는다(World가 사라진 뒤의 전달). 전제: 요청은
// 1번 프레임 client Tick의 World 생성에서 냈으므로 2번 프레임 Pump가 허가해 IO를 내고(Pump는 완료를
// 먼저 거두고 허가가 IO를 낸다 — 그 Pump 안에서는 끝나지 않는다), 3번 프레임 Pump는 완료를 거둔
// 뒤 공개 전에 취소를 확인한다. 그래서 요청은 끝나기 전에 취소된다 — IO 속도와 무관하다.
TEST(EngineAssets_WorldReleasedTicketIsCancelled) {
    DiskScene disk("world");
    REQUIRE(disk.Write());
    ConfigFixture fixture(DevelopmentRules());
    REQUIRE(PrepareSettings(fixture.Context(), disk.Root()));
    SystemAllocator allocator;
    WorldClient client;
    const WorldSubsystemType worldSubsystems[] = {
        MakeSubsystemType<WorldSubsystem, TicketWorldSubsystem>("Ticket")};
    client.subsystems = worldSubsystems;
    const EngineSubsystemType subsystems[] = {EngineAssetsSubsystemType()};
    Engine engine(allocator);
    WorldTicket = WorldTicketState{};
    WorldTicket.engine = &engine;
    REQUIRE(engine.Initialize(MakeEngineConfig(&fixture.Context(), subsystems, &client)));
    REQUIRE(engine.RunFrame());
    REQUIRE(client.world != nullptr);
    REQUIRE(engine.RunFrame());
    CHECK(client.destroyed && engine.WorldCount() == 0);
    CHECK(WorldTicket.deinitInFrame);
    CHECK(WorldTicket.deinitBlocking == ErrorCode::WrongContext);
    CHECK(WorldTicket.calls == 0);
    REQUIRE(engine.RunFrame());
    CHECK(WorldTicket.calls == 1 && WorldTicket.error == ErrorCode::Cancelled);
    CHECK(engine.Shutdown());
    WorldTicket.engine = nullptr;
}

// Pump 실패(11.8절 5): AssetManager를 직접 Shutdown하면 그 뒤 프레임의 Pump가 ShuttingDown으로
// 실패한다 — 처음 한 번만 Error로 기록하고(두 프레임 실패에 로그 하나) 횟수를 세며 프레임은
// 계속된다. 엔진 종료(두 번째 Shutdown)는 성공이다. 부팅 CVar(asset.cooked_root)는 시작 뒤 쓰기를
// 거부한다.
TEST(EngineAssets_PumpFailureAndBootValues) {
    ManualEngineAssetsLogs logs;
    LogInstallScope installed(logs.logs);
    DiskScene disk("pump");
    REQUIRE(disk.Write());
    ConfigFixture fixture(DevelopmentRules());
    config::ConfigContext& settings = fixture.Context();
    REQUIRE(PrepareSettings(settings, disk.Root()));
    CHECK(settings.AddAssignment(config::ConfigLayer::Console, "test", "asset.cooked_root", "X")
              .Error() == ErrorCode::InvalidArgument);
    SystemAllocator allocator;
    const EngineSubsystemType subsystems[] = {EngineAssetsSubsystemType()};
    Engine engine(allocator);
    REQUIRE(engine.Initialize(MakeEngineConfig(&settings, subsystems, nullptr)));
    EngineAssetsSubsystem* assets = engine.GetSubsystem<EngineAssetsSubsystem>();
    REQUIRE(engine.RunFrame());
    CHECK(assets->Stats().pumpFailures == 0);
    REQUIRE(assets->Manager().Shutdown());
    REQUIRE(engine.RunFrame());
    REQUIRE(engine.RunFrame());
    CHECK(assets->Stats().pumpFailures == 2);
    logs.Pump();
    CHECK(CapturedLogs.Count(LogLevel::Error, PumpFailedLog) == 1);
    CHECK(strcmp(CapturedLogs.Message(LogLevel::Error, PumpFailedLog),
                 "Asset pump failed: ShuttingDown") == 0);
    CHECK(assets->CookedRoot() == disk.Root());
    CHECK(engine.Shutdown());
}

// 초기화 할당 실패(11.8절 4): 엔진 allocator의 모든 할당 지점에서 한 번 실패해도 엔진 초기화가
// 실패하거나(그 오류) 성공하고, 실패면 Failed 상태로 정리되며, 성공이면 프레임과 종료가 정상이다.
TEST(EngineAssets_InitializationAllocationFailures) {
    ManualEngineAssetsLogs logs;
    LogInstallScope installed(logs.logs);
    DiskScene disk("alloc");
    REQUIRE(disk.Write());
    // 봉투 읽기·검증 상태의 할당 실패가 그 단계 이름으로 기록되는지 본다(ContentSigning 7절 4).
    uint32 envelopeReadFailures = 0;
    uint32 envelopeVerifyFailures = 0;
    SystemAllocator system;
    test::NFailAllocator failing(system);
    const EngineSubsystemType subsystems[] = {EngineAssetsSubsystemType()};
    uint32 failures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        ConfigFixture fixture(DevelopmentRules());
        REQUIRE(PrepareSettings(fixture.Context(), disk.Root()));
        bool failed = false;
        logs.Pump();
        CapturedLogs.Reset();
        {
            Engine engine(failing);
            failing.FailOnAttempt(attempt);
            const auto initialized =
                engine.Initialize(MakeEngineConfig(&fixture.Context(), subsystems, nullptr));
            failed = failing.Attempts() >= attempt;
            failing.DisableFailure();
            if (initialized) {
                CHECK(engine.RunFrame());
                CHECK(engine.Shutdown());
            } else {
                CHECK(initialized.Error() == ErrorCode::OutOfMemory);
                CHECK(engine.State() == EngineState::Failed);
            }
        }

        logs.Pump();
        const char* message = CapturedLogs.Message(LogLevel::Error, InitFailedLog);
        auto startsWith = [message](const char* prefix) noexcept {
            return strncmp(message, prefix, strlen(prefix)) == 0;
        };
        envelopeReadFailures +=
            startsWith("EngineAssets initialization failed at content signature read: "
                       "OutOfMemory")
                ? 1
                : 0;
        envelopeVerifyFailures += startsWith("EngineAssets initialization failed at content "
                                             "signature (None): OutOfMemory")
                                      ? 1
                                      : 0;
        if (!failed) {
            break;
        }

        ++failures;
    }

    CHECK(failures > 0);
    CHECK(envelopeReadFailures > 0);
    CHECK(envelopeVerifyFailures > 0);
}

// 프레임 할당 실패(11.8절 5): Discard가 일어나는 프레임과 그다음 프레임의 모든 할당 지점에서 한 번
// 실패해도 그 프레임만 실패하고(GC 조각 실패는 매번 Error로 기록 — 로그 수가 실패 수와 같다),
// 이어지는 프레임이 GC cycle을 끝낸다. 끝난 상태는 실패를 넣지 않은 기준 실행과 같다(남은
// 회수·Discard 요청 없음, 재시도 대기 없음, 살아 있는 객체 수 같음).
TEST(EngineAssets_FrameAllocationFailures) {
    DiskScene disk("frame-alloc");
    REQUIRE(disk.Write());
    ManualEngineAssetsLogs logs;
    LogInstallScope installed(logs.logs);
    SystemAllocator system;
    test::NFailAllocator failing(system);
    const EngineSubsystemType subsystems[] = {EngineAssetsSubsystemType()};
    uint32 failures = 0;
    uint64 gcFailures = 0;
    uint32 baselineLive = 0;
    // attempt 0은 실패를 넣지 않는 기준 실행이다.
    for (uint64 attempt = 0;; ++attempt) {
        ConfigFixture fixture(DevelopmentRules());
        REQUIRE(PrepareSettings(fixture.Context(), disk.Root()));
        bool failed = false;
        {
            FrameClient client;
            client.release = true;
            Engine engine(failing);
            REQUIRE(engine.Initialize(MakeEngineConfig(&fixture.Context(), subsystems, &client)));
            EngineAssetsSubsystem* assets = engine.GetSubsystem<EngineAssetsSubsystem>();
            auto preloaded =
                assets->Manager().LoadBlocking(RequestOf(2), "EngineAssetsTests.Preload");
            REQUIRE(preloaded);
            client.preloaded = Move(preloaded.Value());
            for (uint32 frame = 0; frame < 3; ++frame) {
                REQUIRE(engine.RunFrame());
            }

            logs.Pump();
            CapturedLogs.Reset();
            // Discard 프레임과 cycle이 이어지는 프레임에 걸쳐 한 번 실패시킨다.
            if (attempt != 0) {
                failing.FailOnAttempt(attempt);
            }

            (void)engine.RunFrame();
            (void)engine.RunFrame();
            failed = attempt != 0 && failing.Attempts() >= attempt;
            failing.DisableFailure();
            constexpr uint32 FrameLimit = 64;
            for (uint32 frame = 0; frame < FrameLimit && assets->Stats().gcCycles == 0; ++frame) {
                REQUIRE(engine.RunFrame());
            }

            CHECK(assets->Stats().gcCycles >= 1);
            // 몇 프레임 더 돌려 남은 회수가 없음을 본다.
            constexpr uint32 DrainFrames = 4;
            for (uint32 frame = 0; frame < DrainFrames; ++frame) {
                REQUIRE(engine.RunFrame());
            }

            ObjectRegistry& registry = assets->Manager().Registry();
            CHECK(!registry.HasPendingReclaim());
            CHECK(registry.Stats().destroyRequested == 0);
            CHECK(!assets->Stats().lingering && !assets->Stats().gcPending);
            if (attempt == 0) {
                baselineLive = registry.Stats().liveObjects;
                CHECK(assets->Stats().gcFailures == 0);
            } else {
                CHECK(registry.Stats().liveObjects == baselineLive);
            }

            logs.Pump();
            CHECK(CapturedLogs.Count(LogLevel::Error, GcFailedLog) == assets->Stats().gcFailures);
            gcFailures += assets->Stats().gcFailures;
            CHECK(engine.Shutdown());
        }

        if (attempt != 0 && !failed) {
            break;
        }

        failures += failed ? 1 : 0;
    }

    CHECK(failures > 0);
    CHECK(gcFailures > 0);
}

// 설정 등록(11.8절 3): 등록 함수는 영역 `asset`을 더한 뒤 변수를 등록하고, 두 번째 호출도 성공한다
// (영역 AlreadyExists를 그대로 쓰고 같은 핸들 등록은 멱등). 소유 스레드 밖 호출은 WrongThread다.
TEST(EngineAssets_RegisterSettings) {
    ConfigFixture fixture(DevelopmentRules());
    config::ConfigContext& context = fixture.Context();
    REQUIRE(RegisterEngineAssetsSettings(context));
    CHECK(RegisterEngineAssetsSettings(context));
    ConfigFixture other(DevelopmentRules());
    ErrorCode offThread = ErrorCode::None;
    struct Call {
        config::ConfigContext* context;
        ErrorCode* error;
    } call{&other.Context(), &offThread};
    platform::Thread thread;
    REQUIRE(thread.Start(
                [](void* raw) noexcept {
                    auto* state = static_cast<Call*>(raw);
                    auto registered = RegisterEngineAssetsSettings(*state->context);
                    *state->error = registered ? ErrorCode::None : registered.Error();
                },
                &call) == platform::ThreadStatus::Success);
    REQUIRE(thread.Join() == platform::ThreadStatus::Success);
    CHECK(offThread == ErrorCode::WrongThread);
}

// 쿠킹 루트 기본값과 매니페스트 읽기 오류(11.8절 3·4): 설정이 비면 진입점이 캡처한 실행 파일 경로의
// 디렉터리 + /Cooked를 찾고(시험 실행 파일 옆에는 없다 — NotFound), 매니페스트 자리가 디렉터리면
// 파일 존재 확인이 거짓이라 NotFound, 빈 파일이면 읽기는 되지만 매니페스트 검증이 거부한다
// (InvalidArgument). 모두 엔진 초기화 실패다.
TEST(EngineAssets_ManifestLocationFailures) {
    SystemAllocator allocator;
    const EngineSubsystemType subsystems[] = {EngineAssetsSubsystemType()};
    {
        ConfigFixture fixture(DevelopmentRules());
        REQUIRE(PrepareSettings(fixture.Context(), ""));
        Engine engine(allocator);
        CHECK(
            engine.Initialize(MakeEngineConfig(&fixture.Context(), subsystems, nullptr)).Error() ==
            ErrorCode::NotFound);
    }

    {
        DiskScene disk("manifest-dir");
        REQUIRE(disk.Write(false));
        StringBuilder<160> path(disk.env.allocator);
        REQUIRE(path.TryAppend(disk.Root()));
        REQUIRE(path.TryAppend("/"));
        REQUIRE(path.TryAppend(CookedManifestFileName));
        REQUIRE(platform::MakeDirectoryTree(platform::PhysicalFileSystem(), path.View().Data(),
                                            path.View().ByteCount()));
        ConfigFixture fixture(DevelopmentRules());
        REQUIRE(PrepareSettings(fixture.Context(), disk.Root()));
        Engine engine(allocator);
        const auto initialized =
            engine.Initialize(MakeEngineConfig(&fixture.Context(), subsystems, nullptr));
        CHECK(initialized.Error() == ErrorCode::NotFound);
        CHECK(engine.State() == EngineState::Failed);
    }

    {
        DiskScene disk("manifest-empty");
        REQUIRE(disk.Write(false));
        REQUIRE(disk.WriteManifestBytes({}));
        ConfigFixture fixture(DevelopmentRules());
        REQUIRE(PrepareSettings(fixture.Context(), disk.Root()));
        Engine engine(allocator);
        const auto initialized =
            engine.Initialize(MakeEngineConfig(&fixture.Context(), subsystems, nullptr));
        CHECK(initialized.Error() == ErrorCode::InvalidArgument);
        CHECK(engine.State() == EngineState::Failed);
    }
}

// Pump 예산 CVar(11.8절 3·5): asset.pump.max_discards 1이면 같은 프레임에 놓은 두 단위(2·3) 가운데
// 한 Pump가 하나만 Discard하고 다음 Pump가 나머지를 한다. 나머지 개수 예산(1)도 Pump가 받는다.
TEST(EngineAssets_PumpBudgetVariables) {
    DiskScene disk("budget");
    REQUIRE(disk.Write());
    ConfigFixture fixture(DevelopmentRules());
    config::ConfigContext& settings = fixture.Context();
    REQUIRE(PrepareSettings(settings, disk.Root()));
    REQUIRE(settings.AddAssignment(config::ConfigLayer::Console, "test", "asset.pump.max_discards",
                                   "1"));
    REQUIRE(
        settings.AddAssignment(config::ConfigLayer::Console, "test", "asset.pump.max_units", "1"));
    REQUIRE(settings.AddAssignment(config::ConfigLayer::Console, "test",
                                   "asset.pump.max_post_loads", "1"));
    SystemAllocator allocator;
    const EngineSubsystemType subsystems[] = {EngineAssetsSubsystemType()};
    Engine engine(allocator);
    REQUIRE(engine.Initialize(MakeEngineConfig(&settings, subsystems, nullptr)));
    AssetManager& assets = engine.GetSubsystem<EngineAssetsSubsystem>()->Manager();
    auto two = assets.LoadBlocking(RequestOf(2), "EngineAssetsTests.Budget");
    auto three = assets.LoadBlocking(RequestOf(3), "EngineAssetsTests.Budget");
    REQUIRE(two && three);
    REQUIRE(engine.RunFrame());
    two.Value() = AssetLease();
    three.Value() = AssetLease();
    auto loaded = [&](uint64 id) noexcept {
        return assets.Loader().FindPackage(FixedId(id)).IsOk();
    };
    REQUIRE(engine.RunFrame());
    CHECK(loaded(2) != loaded(3));
    REQUIRE(engine.RunFrame());
    CHECK(!loaded(2) && !loaded(3));
    CHECK(engine.Shutdown());
}

// 모듈 진입점(11.8절 1·3): 실행 파일 모듈 표의 EngineAssets 행에 RegisterSettings가
// 있고(HostSettings가 부르는 경로), 그것이 asset.* CVar를 등록한다 — 등록 뒤 asset.cooked_root
// 대입을 받는다. 표는 프로세스 로더의 것이다(Modular에는 정적 표가 없다 — 진입 기술은 EntryAt,
// Docs/Design/ModuleLoader 3.14절).
TEST(EngineAssets_ModuleEntryRegistersSettings) {
    const ModuleEntry* found = nullptr;
    ModuleLoader& modules = test::ProcessModules();
    const ModuleTable table = modules.Table();
    for (usize index = 0; index < table.Size(); ++index) {
        if (strcmp(table[index].name, "EngineAssets") == 0) {
            found = &modules.EntryAt(index);
        }
    }

    REQUIRE(found != nullptr && found->registerSettings != nullptr);
    ConfigFixture fixture(DevelopmentRules());
    config::ConfigContext& context = fixture.Context();
    REQUIRE(found->registerSettings(context));
    CHECK(context.RegisterConsoleRegion("asset").Error() == ErrorCode::AlreadyExists);
    CHECK(context.AddAssignment(config::ConfigLayer::Console, "test", "asset.pump.time_ms", "1"));
}

// 쥔 Discard 객체의 재시도(11.8절 5): 게임 코드가 Discard될 에셋 객체를 RootRef로 쥐고 있으면
// cycle이 그 객체를 회수하지 못하고 재시도 대기(lingering)가 된다. 놓은 뒤 엔진 프레임 시간이
// asset.gc.retry_seconds를 넘으면 다음 cycle이 회수한다(고정 delta — 결정적). 쥔 채로는 재시도
// cycle도 회수하지 못해 다시 기다린다.
TEST(EngineAssets_HeldDiscardedObjectsRetry) {
    DiskScene disk("retry");
    REQUIRE(disk.Write());
    ConfigFixture fixture(DevelopmentRules());
    config::ConfigContext& settings = fixture.Context();
    REQUIRE(PrepareSettings(settings, disk.Root()));
    // 고정 delta 1/60 s의 6 프레임이다.
    REQUIRE(settings.AddAssignment(config::ConfigLayer::Console, "test", "asset.gc.retry_seconds",
                                   "0.1"));
    SystemAllocator allocator;
    const EngineSubsystemType subsystems[] = {EngineAssetsSubsystemType()};
    Engine engine(allocator);
    REQUIRE(engine.Initialize(MakeEngineConfig(&settings, subsystems, nullptr)));
    EngineAssetsSubsystem* assets = engine.GetSubsystem<EngineAssetsSubsystem>();
    ObjectRegistry& registry = assets->Manager().Registry();
    auto lease = assets->Manager().LoadBlocking(RequestOf(2), "EngineAssetsTests.Retry");
    REQUIRE(lease);
    auto pinned = registry.TryPin(registry.OwnerAccess(), lease.Value().Object());
    REQUIRE(pinned);
    lease.Value() = AssetLease();
    constexpr uint32 FrameLimit = 64;
    auto runUntilCycles = [&](uint64 cycles) noexcept {
        uint32 frames = 0;
        while (assets->Stats().gcCycles < cycles && frames < FrameLimit) {
            if (!engine.RunFrame()) {
                return FrameLimit;
            }

            ++frames;
        }

        return frames;
    };
    REQUIRE(runUntilCycles(1) < FrameLimit);
    CHECK(assets->Stats().lingering && registry.Stats().destroyRequested > 0);
    // 쥔 채로 재시도 cycle이 돌아도 회수하지 못하고 다시 기다린다.
    const uint32 heldFrames = runUntilCycles(2);
    CHECK(heldFrames >= 6 && heldFrames < FrameLimit);
    CHECK(assets->Stats().lingering && registry.Stats().destroyRequested > 0);
    pinned.Value().Reset();
    REQUIRE(runUntilCycles(3) < FrameLimit);
    CHECK(!assets->Stats().lingering && registry.Stats().destroyRequested == 0);
    CHECK(assets->Stats().gcFailures == 0);
    CHECK(engine.Shutdown());
}

} // namespace engine_assets_tests_file
