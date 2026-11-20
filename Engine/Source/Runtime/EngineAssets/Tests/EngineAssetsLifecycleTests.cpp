#include "../Private/EngineAssetsInternal.h"
#include "EngineAssetsTestSupport.h"

#include <Config/CVarAccess.h>
#include <Core/Memory/SystemAllocator.h>
#include <Object/ConstructionContext.h>
#include <TestSupport/Test.h>
#include <stdio.h>

// EngineAssets 수명·설정 전달 시험이다(Docs/Design/AssetSystem.ko.md 11.8절 3·5·6): IO가 진행 중인
// 종료, callback 안의 재종료, GC cycle 도중 종료, 재시도 대기 중의 새 Discard, 실행 중 CVar 변경,
// 허가 상한·worker 수 CVar 전달, 예산 변환, 두 엔진의 Registry 문맥.
using namespace rocks;

namespace engine_assets_lifecycle_tests_file {

using namespace rocks::asset_test;
using namespace rocks::engine_assets_test;
using rocks::test::config::ConfigFixture;
using rocks::test::config::DevelopmentRules;

namespace {

constexpr uint32 FrameLimit = 64;

// 첫 Tick에 상주하지 않은 3을 요청하는 client다. reenter면 callback 안에서 엔진 Shutdown을 다시
// 부르고 그 결과를 남긴다. handle은 OnEngineShutdown에서 놓는다.
class InFlightClient final : public EngineClient {
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
            [this, &engine](Result<AssetLease> lease, const AssetDiagnostic&) noexcept {
                ++calls;
                error = lease ? ErrorCode::None : lease.Error();
                if (reenter) {
                    reentered = engine.Shutdown().Error();
                }
            });
        requested =
            callback && assets.OnReady(ticket, TaskLane::Game, Move(callback.Value())).IsOk();
    }

    void OnEngineShutdown(Engine&) noexcept override {
        ticket = LoadTicket();
    }

    bool reenter = false;
    bool requested = false;
    uint32 calls = 0;
    ErrorCode error = ErrorCode::None;
    ErrorCode reentered = ErrorCode::None;
    LoadTicket ticket;
};

// 패키지 3이 허가돼 IO를 기다리는지다(Pump는 완료를 먼저 거두고 허가가 IO를 낸다 — 다음 Pump 전까지
// 완료가 처리되지 않으므로 OS의 IO 속도와 무관하다).
bool PackageReading(AssetLoader& loader, Allocator& allocator) noexcept {
    AssetLoaderSnapshot snapshot(allocator);
    if (!loader.Describe(snapshot)) {
        return false;
    }

    for (const AssetLoaderPackageInfo& package : snapshot.packages) {
        if (package.asset == FixedId(3)) {
            return package.phase == AssetLoaderPackagePhase::Reading &&
                   snapshot.packagesInFlight == 1;
        }
    }

    return false;
}

} // namespace

// 종료 조합(11.8절 6, 14 완료 조건 2): 종료 시점 2(요청만 있고 Pump 전·패키지 IO 진행 중) ×
// callback 안 재종료 2(없음·있음) = 4. 어느 경우든 callback은 엔진 종료의 CompleteInFlightWork에서
// ShuttingDown을 한 번 받고(IO는 기다려 닫는다), 그 안에서 다시 부른 엔진 Shutdown은 Busy이며, 바깥
// 종료는 성공해 Stopped가 된다.
TEST(EngineAssets_ShutdownCombinations) {
    uint32 cases = 0;
    for (uint32 reading = 0; reading < 2; ++reading) {
        for (uint32 reenter = 0; reenter < 2; ++reenter) {
            char tag[32] = {};
            snprintf(tag, sizeof(tag), "shutdown-%u%u", reading, reenter);
            DiskScene disk(tag);
            REQUIRE(disk.Write());
            ConfigFixture fixture(DevelopmentRules());
            REQUIRE(PrepareSettings(fixture.Context(), disk.Root()));
            SystemAllocator allocator;
            InFlightClient client;
            client.reenter = reenter != 0;
            const EngineSubsystemType subsystems[] = {EngineAssetsSubsystemType()};
            Engine engine(allocator);
            REQUIRE(engine.Initialize(MakeEngineConfig(&fixture.Context(), subsystems, &client)));
            AssetManager& assets = engine.GetSubsystem<EngineAssetsSubsystem>()->Manager();
            REQUIRE(engine.RunFrame());
            REQUIRE(client.requested);
            CHECK(!PackageReading(assets.Loader(), allocator));
            if (reading != 0) {
                // 다음 프레임 Pump가 단위를 허가해 IO를 낸다.
                REQUIRE(engine.RunFrame());
                CHECK(PackageReading(assets.Loader(), allocator));
            }

            CHECK(client.calls == 0);
            CHECK(engine.Shutdown());
            CHECK(engine.State() == EngineState::Stopped);
            CHECK(client.calls == 1 && client.error == ErrorCode::ShuttingDown);
            CHECK(client.reentered == (reenter != 0 ? ErrorCode::Busy : ErrorCode::None));
            ++cases;
        }
    }

    CHECK(cases == 4);
}

namespace {

// 2·3을 상주시킨 뒤 둘 다 놓은 엔진 장면이다(다음 Pump가 Discard한다).
struct ReleasedScene {
    explicit ReleasedScene(const char* tag) noexcept : disk(tag) {
    }

    bool Start(Span<const char* const> assignments) noexcept {
        if (!disk.Write() || !PrepareSettings(fixture.Context(), disk.Root())) {
            return false;
        }

        for (usize index = 0; index + 1 < assignments.Size(); index += 2) {
            if (!fixture.Context().AddAssignment(config::ConfigLayer::Console, "test",
                                                 assignments[index], assignments[index + 1])) {
                return false;
            }
        }

        if (!engine.Initialize(MakeEngineConfig(&fixture.Context(), subsystems, nullptr))) {
            return false;
        }

        assets = engine.GetSubsystem<EngineAssetsSubsystem>();
        auto loadedTwo = assets->Manager().LoadBlocking(RequestOf(2), "EngineAssetsTests.Scene");
        auto loadedThree = assets->Manager().LoadBlocking(RequestOf(3), "EngineAssetsTests.Scene");
        if (!loadedTwo || !loadedThree) {
            return false;
        }

        two = Move(loadedTwo.Value());
        three = Move(loadedThree.Value());
        return true;
    }

    bool Loaded(uint64 id) noexcept {
        return assets->Manager().Loader().FindPackage(FixedId(id)).IsOk();
    }

    // cycle 수가 cycles가 될 때까지 프레임을 돌린다(돌린 프레임 수, 한도를 넘으면 FrameLimit).
    uint32 RunUntilCycles(uint64 cycles) noexcept {
        uint32 frames = 0;
        while (assets->Stats().gcCycles < cycles && frames < FrameLimit) {
            if (!engine.RunFrame()) {
                return FrameLimit;
            }

            ++frames;
        }

        return frames;
    }

    DiskScene disk;
    ConfigFixture fixture{DevelopmentRules()};
    SystemAllocator allocator;
    const EngineSubsystemType subsystems[1] = {EngineAssetsSubsystemType()};
    Engine engine{allocator};
    EngineAssetsSubsystem* assets = nullptr;
    AssetLease two;
    AssetLease three;
};

} // namespace

// GC cycle 도중 종료(11.8절 6): 객체 수 예산 1로 cycle이 여러 프레임에 걸치는 동안 엔진을 끝내도
// 종료가 성공한다(AssetManager Shutdown이 공개를 놓고 Registry는 진행 중 cycle과 함께 정리된다).
TEST(EngineAssets_ShutdownDuringGcCycle) {
    ReleasedScene scene("shutdown-gc");
    const char* const assignments[] = {"asset.gc.max_objects", "1", "asset.gc.time_ms", "60000"};
    REQUIRE(scene.Start(assignments));
    scene.two = AssetLease();
    scene.three = AssetLease();
    REQUIRE(scene.engine.RunFrame());
    ObjectRegistry& registry = scene.assets->Manager().Registry();
    CHECK(scene.assets->Stats().gcPending);
    CHECK(registry.CollectionPhase() != GcPhase::Idle);
    CHECK(scene.assets->Stats().gcCycles == 0);
    CHECK(scene.engine.Shutdown());
    CHECK(scene.engine.State() == EngineState::Stopped);
}

// 재시도 대기 중의 새 Discard(11.8절 5): 쥔 객체 때문에 재시도를 기다리는 동안 다른 단위를 놓으면
// 재시도 시간과 무관하게 다음 Pump의 Discard가 곧바로 cycle을 시작한다. 그 cycle이 놓은 단위를
// 회수하고, 쥔 객체는 여전히 남아 다시 기다린다(재시도 시간 1000 s — 그 전에는 재시도가 없다).
TEST(EngineAssets_LingeringThenNewDiscard) {
    ReleasedScene scene("linger-discard");
    const char* const assignments[] = {"asset.gc.retry_seconds", "1000"};
    REQUIRE(scene.Start(assignments));
    ObjectRegistry& registry = scene.assets->Manager().Registry();
    auto pinned = registry.TryPin(registry.OwnerAccess(), scene.two.Object());
    REQUIRE(pinned);
    scene.two = AssetLease();
    REQUIRE(scene.RunUntilCycles(1) < FrameLimit);
    CHECK(scene.assets->Stats().lingering && !scene.assets->Stats().gcPending);
    const uint32 heldDestroyRequests = registry.Stats().destroyRequested;
    CHECK(heldDestroyRequests > 0);
    // 기다리는 동안은 cycle이 없다.
    for (uint32 frame = 0; frame < 4; ++frame) {
        REQUIRE(scene.engine.RunFrame());
    }

    CHECK(scene.assets->Stats().gcCycles == 1 && scene.assets->Stats().lingering);
    const uint32 liveBefore = registry.Stats().liveObjects;
    scene.three = AssetLease();
    REQUIRE(scene.engine.RunFrame());
    CHECK(!scene.Loaded(3));
    CHECK(scene.assets->Stats().gcPending || scene.assets->Stats().gcCycles == 2);
    const uint32 frames = scene.RunUntilCycles(2);
    CHECK(frames < FrameLimit);
    CHECK(scene.assets->Stats().gcCycles == 2);
    CHECK(registry.Stats().liveObjects < liveBefore);
    CHECK(scene.assets->Stats().lingering);
    CHECK(registry.Stats().destroyRequested == heldDestroyRequests);
    CHECK(scene.assets->Stats().gcFailures == 0);
    pinned.Value().Reset();
    CHECK(scene.engine.Shutdown());
}

// 실행 중 CVar 변경(11.8절 3·5): Pump 예산은 프레임 시작의 설정 sync 뒤 값을 쓴다. 시작 때 무제한
// Discard였다가 실행 중 asset.pump.max_discards를 1로 바꾸면 다음 프레임 Pump가 놓은 두 단위 가운데
// 하나만 Discard하고, 다시 0(무제한)으로 바꾸면 그다음 프레임이 나머지를 한다.
TEST(EngineAssets_RuntimeVariableChange) {
    ReleasedScene scene("runtime-cvar");
    REQUIRE(scene.Start({}));
    REQUIRE(scene.engine.RunFrame());
    config::ConfigContext& settings = scene.fixture.Context();
    REQUIRE(settings.AddAssignment(config::ConfigLayer::Console, "test", "asset.pump.max_discards",
                                   "1"));
    scene.two = AssetLease();
    scene.three = AssetLease();
    REQUIRE(scene.engine.RunFrame());
    CHECK(scene.Loaded(2) != scene.Loaded(3));
    REQUIRE(settings.AddAssignment(config::ConfigLayer::Console, "test", "asset.pump.max_discards",
                                   "0"));
    REQUIRE(scene.engine.RunFrame());
    CHECK(!scene.Loaded(2) && !scene.Loaded(3));
    CHECK(scene.engine.Shutdown());
}

// 허가 상한 CVar 전달(11.8절 3, 11.6절 9): 동시 진행 패키지 상한 2(기본·1) × 임시 byte 상한 2(기본·
// 1 byte) = 4. 2·3을 한 Pump에 요청하면 둘 다 기본일 때만 두 패키지가 허가되고, 어느 한쪽이 1이면
// 첫 패키지만 허가되며(진행 중인 것이 없으면 상한을 넘어도 하나는 허가한다) 다른 단위는 허가를
// 기다린다.
TEST(EngineAssets_AdmissionVariables) {
    uint32 cases = 0;
    for (uint32 countLimited = 0; countLimited < 2; ++countLimited) {
        for (uint32 bytesLimited = 0; bytesLimited < 2; ++bytesLimited) {
            char tag[32] = {};
            snprintf(tag, sizeof(tag), "admission-%u%u", countLimited, bytesLimited);
            DiskScene disk(tag);
            REQUIRE(disk.Write());
            ConfigFixture fixture(DevelopmentRules());
            config::ConfigContext& settings = fixture.Context();
            REQUIRE(RegisterEngineAssetsSettings(settings));
            REQUIRE(settings.AddAssignment(config::ConfigLayer::Session, "test",
                                           "asset.cooked_root", disk.Root()));
            if (countLimited != 0) {
                REQUIRE(settings.AddAssignment(config::ConfigLayer::Session, "test",
                                               "asset.loader.max_packages_in_flight", "1"));
            }

            if (bytesLimited != 0) {
                REQUIRE(settings.AddAssignment(config::ConfigLayer::Session, "test",
                                               "asset.loader.max_temp_bytes", "1"));
            }

            REQUIRE(settings.Sync());
            REQUIRE(settings.CompleteStartup());
            SystemAllocator allocator;
            const EngineSubsystemType subsystems[] = {EngineAssetsSubsystemType()};
            Engine engine(allocator);
            REQUIRE(engine.Initialize(MakeEngineConfig(&settings, subsystems, nullptr)));
            AssetManager& assets = engine.GetSubsystem<EngineAssetsSubsystem>()->Manager();
            auto two = assets.RequestAsset(RequestOf(2));
            auto three = assets.RequestAsset(RequestOf(3));
            REQUIRE(two && three);
            REQUIRE(engine.RunFrame());
            AssetLoaderSnapshot snapshot(allocator);
            REQUIRE(assets.Loader().Describe(snapshot));
            const bool limited = countLimited != 0 || bytesLimited != 0;
            CHECK(snapshot.packagesInFlight == (limited ? 1u : 2u));
            uint32 waitingAdmission = 0;
            for (const AssetLoaderUnitInfo& unit : snapshot.units) {
                waitingAdmission += unit.waiting == AssetLoaderWait::Admission ? 1 : 0;
            }

            CHECK(waitingAdmission == (limited ? 1u : 0u));
            two.Value() = LoadTicket();
            three.Value() = LoadTicket();
            CHECK(engine.Shutdown());
            ++cases;
        }
    }

    CHECK(cases == 4);
}

// 로더 worker 수 CVar(11.8절 3): asset.loader.workers 값이 로더 worker scheduler의 worker 수가 되고
// Stats().loaderWorkers로 보인다(기본 2, 설정 3).
TEST(EngineAssets_LoaderWorkersVariable) {
    const char* const values[] = {nullptr, "3"};
    const uint32 expected[] = {2, 3};
    for (uint32 index = 0; index < 2; ++index) {
        char tag[32] = {};
        snprintf(tag, sizeof(tag), "workers-%u", index);
        DiskScene disk(tag);
        REQUIRE(disk.Write());
        ConfigFixture fixture(DevelopmentRules());
        config::ConfigContext& settings = fixture.Context();
        REQUIRE(RegisterEngineAssetsSettings(settings));
        REQUIRE(settings.AddAssignment(config::ConfigLayer::Session, "test", "asset.cooked_root",
                                       disk.Root()));
        if (values[index] != nullptr) {
            REQUIRE(settings.AddAssignment(config::ConfigLayer::Session, "test",
                                           "asset.loader.workers", values[index]));
        }

        REQUIRE(settings.Sync());
        REQUIRE(settings.CompleteStartup());
        SystemAllocator allocator;
        const EngineSubsystemType subsystems[] = {EngineAssetsSubsystemType()};
        Engine engine(allocator);
        REQUIRE(engine.Initialize(MakeEngineConfig(&settings, subsystems, nullptr)));
        CHECK(engine.GetSubsystem<EngineAssetsSubsystem>()->Stats().loaderWorkers ==
              expected[index]);
        CHECK(engine.Shutdown());
    }
}

// 예산 변환(11.8절 3): ms 값은 ns로(소수 부분 버림), 개수 0은 무제한 표지, 0이 아닌 값은 그대로다.
// GC 객체 수는 1 이상이라 그대로 넘긴다.
TEST(EngineAssets_BudgetMapping) {
    using namespace engine_assets_detail;
    struct Case {
        const char* pumpMs;
        const char* units;
        const char* postLoads;
        const char* discards;
        const char* gcMs;
        const char* gcObjects;
        uint64 pumpNs;
        uint32 maxUnits;
        uint32 maxPostLoads;
        uint32 maxDiscards;
        uint64 gcNs;
        uint32 gcMaxObjects;
    };
    const Case cases[] = {
        {"0", "0", "0", "0", "0", "1", 0, AssetLoaderPumpBudget::UnlimitedUnits,
         AssetLoaderPumpBudget::UnlimitedUnits, AssetManagerPumpBudget::UnlimitedDiscards, 0, 1},
        {"2.5", "3", "4", "5", "1.0000019", "7", 2500000, 3, 4, 5, 1000001, 7},
    };
    for (const Case& expected : cases) {
        ConfigFixture fixture(DevelopmentRules());
        config::ConfigContext& settings = fixture.Context();
        REQUIRE(RegisterEngineAssetsSettings(settings));
        const char* const assignments[] = {"asset.pump.time_ms",        expected.pumpMs,
                                           "asset.pump.max_units",      expected.units,
                                           "asset.pump.max_post_loads", expected.postLoads,
                                           "asset.pump.max_discards",   expected.discards,
                                           "asset.gc.time_ms",          expected.gcMs,
                                           "asset.gc.max_objects",      expected.gcObjects};
        for (usize index = 0; index < 12; index += 2) {
            REQUIRE(settings.AddAssignment(config::ConfigLayer::Console, "test", assignments[index],
                                           assignments[index + 1]));
        }

        REQUIRE(settings.Sync());
        cvar::SnapshotBinding bind{settings.Acquire()};
        const AssetManagerPumpBudget pump = ReadPumpBudget();
        CHECK(pump.loader.maxNanoseconds == expected.pumpNs);
        CHECK(pump.loader.maxUnits == expected.maxUnits);
        CHECK(pump.loader.maxPostLoads == expected.maxPostLoads);
        CHECK(pump.maxDiscards == expected.maxDiscards);
        const GcBudget gc = ReadGcBudget();
        CHECK(gc.maxNanoseconds == expected.gcNs);
        CHECK(gc.maxObjects == expected.gcMaxObjects);
    }
}

// 두 엔진(11.8절 4): 한 프로세스에서 동시에 띄운 두 엔진의 에셋 Registry 문맥 id는 다르다(상위 값은
// 같고 하위 값이 프로세스 안 순번이다).
TEST(EngineAssets_EnginesHaveDistinctContexts) {
    DiskScene disk("two-engines");
    REQUIRE(disk.Write());
    ConfigFixture fixture(DevelopmentRules());
    REQUIRE(PrepareSettings(fixture.Context(), disk.Root()));
    SystemAllocator allocator;
    const EngineSubsystemType subsystems[] = {EngineAssetsSubsystemType()};
    Engine first(allocator);
    Engine second(allocator);
    REQUIRE(first.Initialize(MakeEngineConfig(&fixture.Context(), subsystems, nullptr)));
    REQUIRE(second.Initialize(MakeEngineConfig(&fixture.Context(), subsystems, nullptr)));
    const ContextId one =
        first.GetSubsystem<EngineAssetsSubsystem>()->Manager().Registry().OwnerAccess().context;
    const ContextId two =
        second.GetSubsystem<EngineAssetsSubsystem>()->Manager().Registry().OwnerAccess().context;
    CHECK(one.high == two.high);
    CHECK(one.low != two.low);
    CHECK(second.Shutdown());
    CHECK(first.Shutdown());
}

} // namespace engine_assets_lifecycle_tests_file
