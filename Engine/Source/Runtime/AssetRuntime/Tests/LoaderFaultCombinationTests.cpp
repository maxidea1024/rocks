#include "LoaderTestSupport.h"

#include <AssetTestFixtures/TestAssets.generated.h>

// 로더 결함 단계·종료 시점 조합 시험이다(Docs/Design/AssetSystem.ko.md 11.6절 4·6·14·16, 14절 완료
// 조건 2): 결함 단계(없음·IO 완료 오류·Begin 즉시 실패·worker 손상(SHA-256)·공개 실패(오래된 의존
// 쿠킹 결과의 대표 타입 — TypeMismatch)) × 의존(없음·Hard) × 종료
// 시점(없음·IO·worker·Ready·PostLoad 대기) × 허가 상한(무제한·1)의 허가 조합 전부. PostLoad
// handler는 늘 있다(PostLoad 대기 종료 시점).
//
// 금지 조합: 의존 없는 공개 실패(오래된 의존이 없다), 상한 1과 종료 시점(상한 1은 단위를 하나씩
// 진행해 조합들의 단계가 맞지 않는다 — 상한 1은 끝까지 진행한 결과만 본다). 조합마다 요청
// 결과(결함별 진단· 종료면 그 시점까지 확정된 실패 또는 ShuttingDown), 패키지 공개 여부, PostLoad
// 횟수·순서를 단언하고, 끝나면 단위·IO·임시 byte가 남지 않는지 본다. 번호 ↔ 조합 일대일과 실행 수로
// 빠짐을 본다.
using namespace rocks;
namespace loader_fault_combination_tests_file {

using namespace rocks::asset_test;

namespace {

enum class FaultStage : uint32 {
    None,
    IoComplete,
    Begin,
    Worker,
    Instantiate,
    Count
};

enum class FaultDependency : uint32 {
    None,
    Hard,
    Count
};

enum class FaultShutdown : uint32 {
    None,
    Io,
    Worker,
    Ready,
    PostLoading,
    Count
};

enum class FaultCap : uint32 {
    Unlimited,
    One,
    Count
};

constexpr uint32 FaultStages = static_cast<uint32>(FaultStage::Count);
constexpr uint32 FaultDependencies = static_cast<uint32>(FaultDependency::Count);
constexpr uint32 FaultShutdowns = static_cast<uint32>(FaultShutdown::Count);
constexpr uint32 FaultCaps = static_cast<uint32>(FaultCap::Count);
constexpr uint32 FaultSceneProduct = FaultStages * FaultDependencies;
constexpr uint32 FaultRunProduct = FaultShutdowns * FaultCaps;
// 허가 조합: (5 × 2 − 1) 장면 × (5 + 1) 실행 = 54.
constexpr uint32 FaultAllowed = (FaultSceneProduct - 1) * (FaultShutdowns + 1);
constexpr uint64 FaultBase = 2000;
constexpr uint64 FaultStride = 10;
constexpr uint64 FaultDependencyOffset = 5;
constexpr uint64 FaultSentinelBase = 1900;
constexpr uint32 FaultSentinels = 8;
// 손상 위치(파일 앞 헤더 뒤 — SHA-256만 바뀐다)다.
constexpr usize CorruptOffset = 70;

struct FaultScene {
    FaultStage stage;
    FaultDependency dependency;
};

struct FaultRun {
    FaultShutdown shutdown;
    FaultCap cap;
};

FaultScene DecodeFaultScene(uint32 index) noexcept {
    return FaultScene{static_cast<FaultStage>(index / FaultDependencies),
                      static_cast<FaultDependency>(index % FaultDependencies)};
}

uint32 EncodeFaultScene(const FaultScene& scene) noexcept {
    return static_cast<uint32>(scene.stage) * FaultDependencies +
           static_cast<uint32>(scene.dependency);
}

FaultRun DecodeFaultRun(uint32 index) noexcept {
    return FaultRun{static_cast<FaultShutdown>(index % FaultShutdowns),
                    static_cast<FaultCap>(index / FaultShutdowns)};
}

uint32 EncodeFaultRun(const FaultRun& run) noexcept {
    return static_cast<uint32>(run.cap) * FaultShutdowns + static_cast<uint32>(run.shutdown);
}

bool AllowedFaultScene(const FaultScene& scene) noexcept {
    return !(scene.stage == FaultStage::Instantiate && scene.dependency == FaultDependency::None);
}

bool AllowedFaultRun(const FaultRun& run) noexcept {
    return run.cap == FaultCap::Unlimited || run.shutdown == FaultShutdown::None;
}

uint64 FaultBaseOf(uint32 index) noexcept {
    return FaultBase + index * FaultStride;
}

// 종료 없이 끝까지 간 요청 결과가 결함과 맞는지다.
bool FinalOutcome(const AssetLoadStatus& status, uint64 base, FaultStage stage) noexcept {
    const bool failed =
        status.state == AssetLoadState::Failed && status.diagnostic.asset == FixedId(base);
    switch (stage) {
    case FaultStage::None:
        return status.state == AssetLoadState::Succeeded;
    case FaultStage::IoComplete:
        return failed && status.error == ErrorCode::Unsupported &&
               status.diagnostic.code == AssetDiagnosticCode::None;
    case FaultStage::Begin:
        return failed && status.error == ErrorCode::Busy &&
               status.diagnostic.code == AssetDiagnosticCode::None;
    case FaultStage::Worker:
        return failed && status.diagnostic.code == AssetDiagnosticCode::CorruptPackage &&
               status.diagnostic.section == static_cast<uint32>(CookedManifestField::PackageSha256);
    case FaultStage::Instantiate:
        return failed && status.error == ErrorCode::TypeMismatch &&
               status.diagnostic.related == FixedId(base + FaultDependencyOffset);
    case FaultStage::Count:
        break;
    }

    return false;
}

// 종료 시점까지 결함이 확정됐는지다(단계: Pump 1 허가 → IO 완료 처리 → worker 완료·공개).
bool FailedBefore(FaultStage stage, FaultShutdown shutdown) noexcept {
    switch (stage) {
    case FaultStage::None:
        return false;
    case FaultStage::Begin:
        return true;
    case FaultStage::IoComplete:
        return shutdown != FaultShutdown::Io;
    case FaultStage::Worker:
    case FaultStage::Instantiate:
        return shutdown == FaultShutdown::PostLoading;
    case FaultStage::Count:
        break;
    }

    return false;
}

} // namespace

// 결함 단계·종료 시점 조합 전부(14절 완료 조건 2): 허가 조합 54개를 실행 6번으로 돈다.
TEST(AssetLoader_FaultStageAndShutdownCombinations) {
    for (uint32 index = 0; index < FaultSceneProduct; ++index) {
        CHECK(EncodeFaultScene(DecodeFaultScene(index)) == index);
    }

    for (uint32 index = 0; index < FaultRunProduct; ++index) {
        CHECK(EncodeFaultRun(DecodeFaultRun(index)) == index);
    }

    AssetEnv env;
    REQUIRE(env.Ready());
    LoaderRig rig(env);
    for (uint32 index = 0; index < FaultSceneProduct; ++index) {
        const FaultScene scene = DecodeFaultScene(index);
        if (!AllowedFaultScene(scene)) {
            continue;
        }

        const uint64 base = FaultBaseOf(index);
        rig.scene.Add(base);
        if (scene.dependency == FaultDependency::Hard) {
            LoaderNode& dependency = rig.scene.Add(base + FaultDependencyOffset);
            dependency.staleType = scene.stage == FaultStage::Instantiate;
            rig.scene.Hard(base, base + FaultDependencyOffset);
        }
    }

    for (uint32 sentinel = 0; sentinel < FaultSentinels; ++sentinel) {
        rig.scene.Add(FaultSentinelBase + sentinel);
    }

    REQUIRE(rig.Cook());
    for (uint32 index = 0; index < FaultSceneProduct; ++index) {
        const FaultScene scene = DecodeFaultScene(index);
        if (!AllowedFaultScene(scene)) {
            continue;
        }

        const uint64 base = FaultBaseOf(index);
        TestPackageIo::File* file = rig.io.FindFile(rig.scene.PathOf(base));
        if (scene.stage == FaultStage::IoComplete) {
            file->completeError = ErrorCode::Unsupported;
        } else if (scene.stage == FaultStage::Begin) {
            file->beginError = ErrorCode::Busy;
        } else if (scene.stage == FaultStage::Worker) {
            rig.scene.FileOf(base)[CorruptOffset] ^= 0x01;
        }
    }

    uint32 executed = 0;
    bool seen[FaultSceneProduct * FaultRunProduct] = {};
    for (uint32 runIndex = 0; runIndex < FaultRunProduct; ++runIndex) {
        const FaultRun run = DecodeFaultRun(runIndex);
        if (!AllowedFaultRun(run)) {
            continue;
        }

        RecordingPostLoad recording(env.allocator);
        rig.config.postLoad = &recording;
        rig.config.limits.maxPackagesInFlight = run.cap == FaultCap::One ? 1 : 4096;
        rig.config.limits.maxTempBytes = NumericLimits<uint64>::Max();
        REQUIRE(rig.Start());
        AssetLoadHandle handles[FaultSceneProduct] = {};
        const bool staged = run.shutdown != FaultShutdown::None;
        AssetLoaderPumpBudget budget;
        if (staged) {
            // 감시 단위가 Pump마다 PostLoad 하나를 가져가 조합 단위는 PostLoad 대기에 머문다.
            budget.maxPostLoads = 0;
            rig.io.hold = true;
            rig.worker.hold = true;
            for (uint32 sentinel = 0; sentinel < FaultSentinels; ++sentinel) {
                (void)rig.Request(FaultSentinelBase + sentinel, LoadPriority::Critical);
            }
        }

        for (uint32 index = 0; index < FaultSceneProduct; ++index) {
            if (AllowedFaultScene(DecodeFaultScene(index))) {
                handles[index] = rig.Request(FaultBaseOf(index));
            }
        }

        if (staged) {
            auto shutdownHere = [&](FaultShutdown point) noexcept {
                if (run.shutdown != point) {
                    return false;
                }

                // 진행 중 일을 내보내 둔다(종료가 꺼내 버린다 — 상태는 종료 시점 그대로).
                rig.io.Complete();
                rig.worker.RunAll();
                return rig.loader->Shutdown().IsOk();
            };
            REQUIRE(rig.loader->Pump(budget));
            if (!shutdownHere(FaultShutdown::Io)) {
                rig.io.Complete();
                REQUIRE(rig.loader->Pump(budget));
                if (!shutdownHere(FaultShutdown::Worker)) {
                    rig.worker.RunAll();
                    if (!shutdownHere(FaultShutdown::Ready)) {
                        REQUIRE(rig.loader->Pump(budget));
                        REQUIRE(shutdownHere(FaultShutdown::PostLoading));
                    }
                }
            }
        } else {
            REQUIRE(rig.PumpUntilSettled());
        }

        for (uint32 index = 0; index < FaultSceneProduct; ++index) {
            const FaultScene scene = DecodeFaultScene(index);
            if (!AllowedFaultScene(scene)) {
                continue;
            }

            const uint64 base = FaultBaseOf(index);
            const uint64 dependency = base + FaultDependencyOffset;
            const AssetLoadStatus status = rig.StatusOf(handles[index]);
            bool ok = true;
            if (staged) {
                ok = FailedBefore(scene.stage, run.shutdown)
                         ? FinalOutcome(status, base, scene.stage)
                         : status.state == AssetLoadState::Failed &&
                               status.error == ErrorCode::ShuttingDown;
                // 종료는 공개한 패키지를 놓고 남은 PostLoad를 부르지 않는다.
                ok = ok && !rig.IsLoaded(base) && recording.CountOf(FixedId(base)) == 0 &&
                     recording.CountOf(FixedId(dependency)) == 0;
            } else {
                ok = FinalOutcome(status, base, scene.stage);
                const bool loaded = scene.stage == FaultStage::None;
                ok = ok && rig.IsLoaded(base) == loaded &&
                     recording.CountOf(FixedId(base)) == (loaded ? 1u : 0u);
                if (scene.dependency == FaultDependency::Hard) {
                    // 상한 1이면 의존이 먼저 끝까지 간다. 무제한이면 함께 진행해 요청 패키지가 공개
                    // 전에 실패하면 의존도 버린다.
                    const bool dependencyLoaded = run.cap == FaultCap::One ||
                                                  scene.stage == FaultStage::None ||
                                                  scene.stage == FaultStage::Instantiate;
                    ok = ok && rig.IsLoaded(dependency) == dependencyLoaded &&
                         recording.CountOf(FixedId(dependency)) == (dependencyLoaded ? 1u : 0u) &&
                         (!loaded || recording.IndexOf(FixedId(dependency)) <
                                         recording.IndexOf(FixedId(base)));
                }
            }

            if (!ok) {
                test::RecordFailure("loader fault combination", __FILE__, __LINE__);
            }

            const uint32 slot = EncodeFaultScene(scene) * FaultRunProduct + runIndex;
            CHECK(!seen[slot]);
            seen[slot] = true;
            ++executed;
        }

        AssetLoaderSnapshot snapshot(env.allocator);
        REQUIRE(rig.loader->Describe(snapshot));
        CHECK(snapshot.units.IsEmpty() && rig.io.active == 0 && snapshot.tempBytesInFlight == 0 &&
              snapshot.packagesInFlight == 0);
        if (!staged) {
            CHECK(recording.PackagesMatch(*rig.loader));
            REQUIRE(rig.loader->Shutdown());
        }

        rig.loader.Reset();
        rig.io.hold = false;
        rig.worker.hold = false;
    }

    CHECK(executed == FaultAllowed);
}

} // namespace loader_fault_combination_tests_file
