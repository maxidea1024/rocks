#include "LoaderTestSupport.h"

#include <AssetTestFixtures/TestAssets.generated.h>

// 로더 결함 단계·종료 시점 조합 시험이다(Docs/Design/AssetSystem 11.6절 4·6·9·14·16, 14절
// 완료 조건 2): 결함 단계(없음·IO 완료 오류·Begin 즉시 실패·worker 손상(SHA-256)·공개 실패(오래된
// 의존 쿠킹 결과의 대표 타입 — TypeMismatch)) × 의존(없음·Hard) × 종료 시점(없음·IO·worker·Ready·
// PostLoad 대기) × 허가 상한(무제한·1) × 종료 시점의 단위(요청 패키지·의존)의 허가 조합 전부.
// PostLoad handler는 늘 있다(PostLoad 대기 종료 시점).
//
// 금지 조합: 의존 없는 공개 실패(오래된 의존이 없다), 의존 없는 조합의 단위 의존, 무제한이나 종료
// 없음의 단위 의존(무제한은 두 단위가 같은 단계로 함께 가고, 종료가 없으면 시점이 없다 — 요청
// 패키지 조합과 같다).
//
// 무제한은 한 로더에서 모든 장면을 함께 진행한다. 상한 1은 단위를 하나씩 진행하므로 조합마다 로더를
// 따로 띄운다(장면 파일은 함께 쓴다): Critical 순환 감시 단위(10개 — 진행 중이 없을 때 혼자 허가)를
// 먼저 공개해 두면 Pump마다 그 PostLoad 하나가 불려 의존은 공개된 채 PostLoad 대기에 머문다. 단계는
// 의존(있으면)이 먼저 IO·worker·Ready·공개를 지나고 요청 패키지가 그 뒤 같은 단계를 지난다.
// 조합마다 종료 전 상태(요청 단위의 대기 이유 Admission·의존 공개 여부·PostLoad 안 됨)와 요청
// 결과(결함별 진단, 종료면 그 시점까지 확정된 실패 또는 ShuttingDown), 패키지 공개 여부, PostLoad
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

// 종료 시점이 어느 단위의 단계인지다(상한 1에서만 다르다).
enum class FaultTarget : uint32 {
    Requested,
    Dependency,
    Count
};

constexpr uint32 FaultStages = static_cast<uint32>(FaultStage::Count);
constexpr uint32 FaultDependencies = static_cast<uint32>(FaultDependency::Count);
constexpr uint32 FaultShutdowns = static_cast<uint32>(FaultShutdown::Count);
constexpr uint32 FaultCaps = static_cast<uint32>(FaultCap::Count);
constexpr uint32 FaultTargets = static_cast<uint32>(FaultTarget::Count);
constexpr uint32 FaultSceneProduct = FaultStages * FaultDependencies;
constexpr uint32 FaultRunProduct = FaultShutdowns * FaultCaps * FaultTargets;
// 허가 조합: 장면 9개(공개 실패는 의존이 있을 때만) × (무제한 5 + 상한 1 종료 없음 1 + 상한 1 요청
// 패키지 단계 4) = 90, 의존 있는 장면 5개 × 상한 1 의존 단계 4 = 20, 합 110.
constexpr uint32 FaultScenesAllowed = FaultSceneProduct - 1;
constexpr uint32 FaultScenesWithDependency = FaultStages;
constexpr uint32 FaultStagedShutdowns = FaultShutdowns - 1;
constexpr uint32 FaultAllowed = FaultScenesAllowed * (FaultShutdowns + 1 + FaultStagedShutdowns) +
                                FaultScenesWithDependency * FaultStagedShutdowns;
constexpr uint64 FaultBase = 2000;
constexpr uint64 FaultStride = 10;
constexpr uint64 FaultDependencyOffset = 5;
constexpr uint64 FaultSentinelBase = 1900;
constexpr uint32 FaultSentinels = 8;
// 상한 1 실행의 Critical 순환 감시 단위다(구성 10개 — 공개 뒤 Pump마다 PostLoad 하나를 가져간다,
// 상한 1 단계 진행은 공개 뒤 Pump 7번이다).
constexpr uint64 FaultCycleBase = 1950;
constexpr uint32 FaultCycleMembers = 10;
constexpr uint32 FaultCapOneInFlight = 1;
constexpr uint32 FaultUnlimitedInFlight = 4096;
// 손상 위치(파일 앞 헤더 뒤 — SHA-256만 바뀐다)다.
constexpr usize CorruptOffset = 70;

struct FaultScene {
    FaultStage stage;
    FaultDependency dependency;
};

struct FaultRun {
    FaultShutdown shutdown;
    FaultCap cap;
    FaultTarget target;
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
    const uint32 shutdown = index % FaultShutdowns;
    index /= FaultShutdowns;
    return FaultRun{static_cast<FaultShutdown>(shutdown), static_cast<FaultCap>(index % FaultCaps),
                    static_cast<FaultTarget>(index / FaultCaps)};
}

uint32 EncodeFaultRun(const FaultRun& run) noexcept {
    return (static_cast<uint32>(run.target) * FaultCaps + static_cast<uint32>(run.cap)) *
               FaultShutdowns +
           static_cast<uint32>(run.shutdown);
}

bool AllowedFaultScene(const FaultScene& scene) noexcept {
    return !(scene.stage == FaultStage::Instantiate && scene.dependency == FaultDependency::None);
}

bool AllowedFaultRun(const FaultRun& run) noexcept {
    return run.target == FaultTarget::Requested ||
           (run.cap == FaultCap::One && run.shutdown != FaultShutdown::None);
}

bool AllowedFault(const FaultScene& scene, const FaultRun& run) noexcept {
    return AllowedFaultScene(scene) && AllowedFaultRun(run) &&
           (run.target == FaultTarget::Requested || scene.dependency == FaultDependency::Hard);
}

// 상한 1을 조합마다 따로 띄우는지다(종료 시점이 있는 상한 1).
bool SeparateFaultRun(const FaultRun& run) noexcept {
    return run.cap == FaultCap::One && run.shutdown != FaultShutdown::None;
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

// 상한 1·종료 시점 조합 하나를 자기 로더로 돌리고 기대와 맞는지 돌려준다: 순환 감시 단위를 먼저
// 공개하고(PostLoad 하나를 가져간 채 나머지 구성은 PostLoad 대기), 의존(있으면) → 요청 패키지
// 순으로 IO·worker·Ready·공개를 지나다가 run의 단위·시점에서 종료한다.
bool RunCapOneShutdown(LoaderRig& rig, const FaultScene& scene, const FaultRun& run,
                       uint64 base) noexcept {
    AssetEnv& env = rig.env;
    const uint64 dependency = base + FaultDependencyOffset;
    const bool hasDependency = scene.dependency == FaultDependency::Hard;
    RecordingPostLoad recording(env.allocator);
    rig.config.postLoad = &recording;
    rig.config.limits.maxPackagesInFlight = FaultCapOneInFlight;
    rig.config.limits.maxTempBytes = NumericLimits<uint64>::Max();
    if (!rig.Start()) {
        return false;
    }

    rig.io.hold = true;
    rig.worker.hold = true;
    AssetLoaderPumpBudget budget;
    budget.maxPostLoads = 0;
    auto pump = [&]() noexcept { return rig.loader->Pump(budget).IsOk(); };
    auto unitOf = [&](uint64 id) noexcept {
        AssetLoaderUnitInfo found;
        AssetLoaderSnapshot snapshot(env.allocator);
        if (rig.loader->Describe(snapshot)) {
            for (const AssetLoaderUnitInfo& unit : snapshot.units) {
                found = unit.asset == FixedId(id) ? unit : found;
            }
        }

        return found;
    };
    (void)rig.Request(FaultCycleBase, LoadPriority::Critical);
    const AssetLoadHandle handle = rig.Request(base);
    bool ok = pump();
    rig.io.Complete();
    ok = ok && pump();
    rig.worker.RunAll();
    // 순환 단위 공개(진행 중이 없을 때 혼자 허가됐다). PostLoad는 그 구성 하나만 불렸다.
    ok = ok && pump() && rig.IsLoaded(FaultCycleBase) && recording.order.Size() == 1;
    // 단계 하나를 지나고 종료 시점이면 종료한다(돌려준 값: 종료했는지).
    bool stopped = false;
    auto stage = [&](FaultTarget owner) noexcept {
        auto stopHere = [&](FaultShutdown point) noexcept {
            if (stopped || run.target != owner || run.shutdown != point) {
                return false;
            }

            // 종료 전 상태: 의존 단계면 요청 단위는 허가를 기다리고(Queued) 의존은 PostLoad 대기
            // 시점에만 공개돼 있다. 요청 패키지 단계면 의존은 공개된 채 PostLoad를 기다린다.
            if (owner == FaultTarget::Dependency) {
                const AssetLoaderUnitInfo waiting = unitOf(base);
                ok = ok && waiting.asset == FixedId(base) &&
                     waiting.phase == AssetLoaderUnitPhase::Queued &&
                     waiting.waiting == AssetLoaderWait::Admission &&
                     rig.IsLoaded(dependency) == (point == FaultShutdown::PostLoading);
            } else if (hasDependency) {
                ok = ok && rig.IsLoaded(dependency) &&
                     unitOf(dependency).phase == AssetLoaderUnitPhase::PostLoading;
            }

            ok = ok && recording.CountOf(FixedId(dependency)) == 0 &&
                 recording.CountOf(FixedId(base)) == 0;
            // 진행 중 일을 내보내 둔다(종료가 꺼내 버린다 — 상태는 종료 시점 그대로).
            rig.io.Complete();
            rig.worker.RunAll();
            stopped = true;
            ok = ok && rig.loader->Shutdown().IsOk();
            return true;
        };
        ok = ok && pump();
        if (stopHere(FaultShutdown::Io)) {
            return;
        }

        rig.io.Complete();
        ok = ok && pump();
        if (stopHere(FaultShutdown::Worker)) {
            return;
        }

        rig.worker.RunAll();
        if (stopHere(FaultShutdown::Ready)) {
            return;
        }

        ok = ok && pump();
        (void)stopHere(FaultShutdown::PostLoading);
    };
    if (hasDependency) {
        stage(FaultTarget::Dependency);
    }

    if (!stopped) {
        stage(FaultTarget::Requested);
    }

    const AssetLoadStatus status = rig.StatusOf(handle);
    const bool failedFirst =
        run.target == FaultTarget::Requested && FailedBefore(scene.stage, run.shutdown);
    ok = ok && stopped &&
         (failedFirst
              ? FinalOutcome(status, base, scene.stage)
              : status.state == AssetLoadState::Failed && status.error == ErrorCode::ShuttingDown);
    // 종료는 공개한 패키지를 놓고 남은 PostLoad를 부르지 않는다.
    ok = ok && !rig.IsLoaded(base) && !rig.IsLoaded(dependency) && !rig.IsLoaded(FaultCycleBase) &&
         recording.CountOf(FixedId(base)) == 0 && recording.CountOf(FixedId(dependency)) == 0;
    AssetLoaderSnapshot snapshot(env.allocator);
    ok = ok && rig.loader->Describe(snapshot).IsOk() && snapshot.units.IsEmpty() &&
         rig.io.active == 0 && snapshot.tempBytesInFlight == 0 && snapshot.packagesInFlight == 0;
    rig.loader.Reset();
    rig.io.hold = false;
    rig.worker.hold = false;
    return ok;
}

} // namespace

// 결함 단계·종료 시점 조합 전부(14절 완료 조건 2): 허가 조합 110개 — 함께 도는 실행 6번(54개)과
// 조합마다 따로 띄우는 상한 1 종료 실행 56개.
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

    for (uint32 member = 0; member < FaultCycleMembers; ++member) {
        rig.scene.Add(FaultCycleBase + member);
        rig.scene.Hard(FaultCycleBase + member, FaultCycleBase + (member + 1) % FaultCycleMembers);
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
    auto record = [&](const FaultScene& scene, uint32 runIndex) noexcept {
        const uint32 slot = EncodeFaultScene(scene) * FaultRunProduct + runIndex;
        CHECK(!seen[slot]);
        seen[slot] = true;
        ++executed;
    };
    for (uint32 runIndex = 0; runIndex < FaultRunProduct; ++runIndex) {
        const FaultRun run = DecodeFaultRun(runIndex);
        if (!AllowedFaultRun(run) || SeparateFaultRun(run)) {
            continue;
        }

        RecordingPostLoad recording(env.allocator);
        rig.config.postLoad = &recording;
        rig.config.limits.maxPackagesInFlight =
            run.cap == FaultCap::One ? FaultCapOneInFlight : FaultUnlimitedInFlight;
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

            record(scene, runIndex);
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

    // 상한 1·종료 시점: 조합마다 로더를 따로 띄운다.
    for (uint32 runIndex = 0; runIndex < FaultRunProduct; ++runIndex) {
        const FaultRun run = DecodeFaultRun(runIndex);
        if (!AllowedFaultRun(run) || !SeparateFaultRun(run)) {
            continue;
        }

        for (uint32 index = 0; index < FaultSceneProduct; ++index) {
            const FaultScene scene = DecodeFaultScene(index);
            if (!AllowedFault(scene, run)) {
                continue;
            }

            CHECK(RunCapOneShutdown(rig, scene, run, FaultBaseOf(index)));
            record(scene, runIndex);
        }
    }

    CHECK(executed == FaultAllowed);
}

} // namespace loader_fault_combination_tests_file
