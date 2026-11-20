#include "LoaderTestSupport.h"

#include <AssetTestFixtures/TestAssets.generated.h>

// 로더 단계 조합 시험이다(Docs/Design/AssetSystem.ko.md 11.6절 6·7, 14절 완료 조건 2): 요청 단위
// 모양(단일·둘 순환) × 의존 상태(없음·새로·진행 중 — 먼저 요청해 IO 보류·공개됨) × 결함(없음·요청
// 패키지 손상·의존 손상) × 합류(없음·같은 단위 두 번째 요청) × 취소 시점(없음·허가
// 전·IO·worker·Ready) × 완료 순서(앞부터·뒤부터)의 허가 조합 전부. 취소 시점이 Ready이고 결함이
// 있으면 같은 Pump에서 실패가 먼저 확정된다(실패·취소 겹침). 조합마다 요청 상태(합류·진행 중 의존
// 요청 포함), 모든 구성· 의존 패키지의 공개 여부를 단언하고, 끝나면 단위·IO·임시 byte가 남지 않는지
// 본다. 번호 ↔ 조합 일대일과 실행 수로 빠짐을 본다.
using namespace rocks;
namespace loader_staged_combination_tests_file {

using namespace rocks::asset_test;

namespace {

enum class StagedShape : uint32 {
    Single,
    Pair,
    Count
};

enum class StagedDependency : uint32 {
    None,
    New,
    InFlight,
    Published,
    Count
};

enum class StagedFault : uint32 {
    None,
    Requested,
    Dependency,
    Count
};

enum class StagedJoin : uint32 {
    None,
    Second,
    Count
};

enum class StagedCancel : uint32 {
    None,
    BeforeAdmission,
    Io,
    Worker,
    Ready,
    Count
};

enum class StagedOrder : uint32 {
    Fifo,
    Lifo,
    Count
};

constexpr uint32 StagedShapes = static_cast<uint32>(StagedShape::Count);
constexpr uint32 StagedDependencies = static_cast<uint32>(StagedDependency::Count);
constexpr uint32 StagedFaults = static_cast<uint32>(StagedFault::Count);
constexpr uint32 StagedJoins = static_cast<uint32>(StagedJoin::Count);
constexpr uint32 StagedCancels = static_cast<uint32>(StagedCancel::Count);
constexpr uint32 StagedOrders = static_cast<uint32>(StagedOrder::Count);
// 장면 축(모양·의존·결함·합류)의 곱과 금지 조합(의존 없음·공개된 의존의 의존 손상) 수다.
constexpr uint32 StagedSceneProduct =
    StagedShapes * StagedDependencies * StagedFaults * StagedJoins;
constexpr uint32 StagedForbidden = 2 * StagedShapes * StagedJoins;
constexpr uint32 StagedSceneCombinations = StagedSceneProduct - StagedForbidden;
constexpr uint32 StagedRuns = StagedCancels * StagedOrders;
constexpr uint32 StagedAllowed = StagedSceneCombinations * StagedRuns;
constexpr uint64 StagedBase = 1000;
constexpr uint64 StagedStride = 10;
constexpr uint64 StagedDependencyOffset = 5;
// 모든 단위를 한 번에 허가하는 상한이다(조합 40개 × 패키지 셋 이하).
constexpr uint32 StagedInFlight = 4096;

struct StagedScene {
    StagedShape shape;
    StagedDependency dependency;
    StagedFault fault;
    StagedJoin join;
};

StagedScene DecodeScene(uint32 index) noexcept {
    const uint32 join = index % StagedJoins;
    index /= StagedJoins;
    const uint32 fault = index % StagedFaults;
    index /= StagedFaults;
    const uint32 dependency = index % StagedDependencies;
    index /= StagedDependencies;
    return StagedScene{static_cast<StagedShape>(index), static_cast<StagedDependency>(dependency),
                       static_cast<StagedFault>(fault), static_cast<StagedJoin>(join)};
}

uint32 EncodeScene(const StagedScene& scene) noexcept {
    return ((static_cast<uint32>(scene.shape) * StagedDependencies +
             static_cast<uint32>(scene.dependency)) *
                StagedFaults +
            static_cast<uint32>(scene.fault)) *
               StagedJoins +
           static_cast<uint32>(scene.join);
}

bool StagedAllowedScene(const StagedScene& scene) noexcept {
    return !(scene.fault == StagedFault::Dependency &&
             (scene.dependency == StagedDependency::None ||
              scene.dependency == StagedDependency::Published));
}

uint64 BaseOf(uint32 index) noexcept {
    return StagedBase + index * StagedStride;
}

void AddStagedScene(LoaderScene& scene, uint32 index, const StagedScene& combination) noexcept {
    const uint64 base = BaseOf(index);
    scene.Add(base);
    if (combination.shape == StagedShape::Pair) {
        scene.Add(base + 1);
        scene.Hard(base, base + 1);
        scene.Hard(base + 1, base);
    }

    if (combination.dependency != StagedDependency::None) {
        scene.Add(base + StagedDependencyOffset);
        scene.Hard(base, base + StagedDependencyOffset);
    }
}

// 결함이 없을 때(취소 없음)의 요청 결과가 기대와 같은지다.
bool UncancelledOutcome(const AssetLoadStatus& status, uint64 base,
                        const StagedScene& combination) noexcept {
    switch (combination.fault) {
    case StagedFault::None:
        return status.state == AssetLoadState::Succeeded;
    case StagedFault::Requested:
        return status.state == AssetLoadState::Failed &&
               status.diagnostic.code == AssetDiagnosticCode::CorruptPackage &&
               status.diagnostic.asset == FixedId(base);
    case StagedFault::Dependency:
        return status.state == AssetLoadState::Failed &&
               status.error == ErrorCode::DependencyFailed &&
               status.diagnostic.related == FixedId(base + StagedDependencyOffset);
    case StagedFault::Count:
        break;
    }

    return false;
}

bool FirstOutcome(const AssetLoadStatus& status, uint64 base, const StagedScene& combination,
                  StagedCancel cancel) noexcept {
    if (cancel == StagedCancel::None ||
        (cancel == StagedCancel::Ready && combination.fault != StagedFault::None)) {
        return UncancelledOutcome(status, base, combination);
    }

    return status.state == AssetLoadState::Cancelled && status.error == ErrorCode::Cancelled;
}

struct StagedHandles {
    AssetLoadHandle first;
    AssetLoadHandle second;
    AssetLoadHandle dependency;
};

} // namespace

// 단계 조합 전부(14절 완료 조건 2): 허가 조합 40개 × 취소 시점 5 × 완료 순서 2 = 400개를 돌린다.
TEST(AssetLoader_StagedCombinations) {
    for (uint32 index = 0; index < StagedSceneProduct; ++index) {
        CHECK(EncodeScene(DecodeScene(index)) == index);
    }

    AssetEnv env;
    REQUIRE(env.Ready());
    LoaderRig rig(env);
    uint32 allowed = 0;
    for (uint32 index = 0; index < StagedSceneProduct; ++index) {
        const StagedScene combination = DecodeScene(index);
        if (StagedAllowedScene(combination)) {
            AddStagedScene(rig.scene, index, combination);
            ++allowed;
        }
    }

    REQUIRE(allowed == StagedSceneCombinations);
    REQUIRE(rig.Cook());
    for (uint32 index = 0; index < StagedSceneProduct; ++index) {
        const StagedScene combination = DecodeScene(index);
        if (!StagedAllowedScene(combination) || combination.fault == StagedFault::None) {
            continue;
        }

        const uint64 corrupt = combination.fault == StagedFault::Requested
                                   ? BaseOf(index)
                                   : BaseOf(index) + StagedDependencyOffset;
        rig.scene.FileOf(corrupt)[70] ^= 0x01;
    }

    rig.config.limits.maxPackagesInFlight = StagedInFlight;
    rig.config.limits.maxTempBytes = NumericLimits<uint64>::Max();
    uint32 executed = 0;
    bool seen[StagedSceneProduct * StagedRuns] = {};
    for (uint32 run = 0; run < StagedRuns; ++run) {
        const auto cancel = static_cast<StagedCancel>(run % StagedCancels);
        const bool reverse = static_cast<StagedOrder>(run / StagedCancels) == StagedOrder::Lifo;
        REQUIRE(rig.Start());
        rig.io.hold = true;
        rig.worker.hold = true;
        StagedHandles handles[StagedSceneProduct] = {};
        CancelSource sources[StagedSceneProduct];
        // 공개된 의존: 먼저 로드한다.
        for (uint32 index = 0; index < StagedSceneProduct; ++index) {
            const StagedScene combination = DecodeScene(index);
            if (StagedAllowedScene(combination) &&
                combination.dependency == StagedDependency::Published) {
                (void)rig.Request(BaseOf(index) + StagedDependencyOffset);
            }
        }

        REQUIRE(rig.SettleHeld(reverse));
        // 진행 중 의존: 요청해 허가(IO 보류)까지 진행한다.
        for (uint32 index = 0; index < StagedSceneProduct; ++index) {
            const StagedScene combination = DecodeScene(index);
            if (StagedAllowedScene(combination) &&
                combination.dependency == StagedDependency::InFlight) {
                handles[index].dependency = rig.Request(BaseOf(index) + StagedDependencyOffset);
            }
        }

        REQUIRE(rig.loader->Pump());
        for (uint32 index = 0; index < StagedSceneProduct; ++index) {
            const StagedScene combination = DecodeScene(index);
            if (!StagedAllowedScene(combination)) {
                continue;
            }

            CancelToken token;
            if (cancel != StagedCancel::None) {
                auto made = CancelSource::Create(env.allocator);
                REQUIRE(made);
                sources[index] = Move(made.Value());
                token = sources[index].Token();
            }

            handles[index].first = rig.Request(BaseOf(index), LoadPriority::Normal, Move(token));
            if (combination.join == StagedJoin::Second) {
                handles[index].second = rig.Request(BaseOf(index));
            }
        }

        auto cancelAll = [&]() noexcept {
            for (CancelSource& source : sources) {
                source.Cancel();
            }
        };
        if (cancel == StagedCancel::BeforeAdmission) {
            cancelAll();
        }

        REQUIRE(rig.loader->Pump());
        if (cancel == StagedCancel::Io) {
            cancelAll();
        }

        if (reverse) {
            rig.io.CompleteReverse();
        } else {
            rig.io.Complete();
        }

        REQUIRE(rig.loader->Pump());
        if (cancel == StagedCancel::Worker) {
            cancelAll();
            REQUIRE(rig.loader->Pump());
        }

        if (reverse) {
            rig.worker.RunAllReverse();
        } else {
            rig.worker.RunAll();
        }

        if (cancel == StagedCancel::Ready) {
            cancelAll();
        }

        REQUIRE(rig.SettleHeld(reverse));
        for (uint32 index = 0; index < StagedSceneProduct; ++index) {
            const StagedScene combination = DecodeScene(index);
            if (!StagedAllowedScene(combination)) {
                continue;
            }

            const uint64 base = BaseOf(index);
            const AssetLoadStatus first = rig.StatusOf(handles[index].first);
            bool ok = FirstOutcome(first, base, combination, cancel);
            bool membersLoaded = first.state == AssetLoadState::Succeeded;
            if (combination.join == StagedJoin::Second) {
                const AssetLoadStatus second = rig.StatusOf(handles[index].second);
                ok = ok && UncancelledOutcome(second, base, combination);
                membersLoaded = membersLoaded || second.state == AssetLoadState::Succeeded;
            }

            ok = ok && rig.IsLoaded(base) == membersLoaded;
            if (combination.shape == StagedShape::Pair) {
                ok = ok && rig.IsLoaded(base + 1) == membersLoaded;
            }

            const uint64 dependency = base + StagedDependencyOffset;
            switch (combination.dependency) {
            case StagedDependency::None:
                break;
            case StagedDependency::New:
                ok = ok && rig.IsLoaded(dependency) == membersLoaded;
                break;
            case StagedDependency::InFlight: {
                const bool healthy = combination.fault != StagedFault::Dependency;
                ok = ok && rig.IsLoaded(dependency) == healthy &&
                     (rig.StateOf(handles[index].dependency) == AssetLoadState::Succeeded) ==
                         healthy;
                break;
            }
            case StagedDependency::Published:
                ok = ok && rig.IsLoaded(dependency);
                break;
            case StagedDependency::Count:
                ok = false;
                break;
            }

            if (!ok) {
                test::RecordFailure("staged loader combination", __FILE__, __LINE__);
            }

            const uint32 slot = EncodeScene(combination) * StagedRuns + run;
            CHECK(!seen[slot]);
            seen[slot] = true;
            ++executed;
        }

        AssetLoaderSnapshot snapshot(env.allocator);
        REQUIRE(rig.loader->Describe(snapshot));
        CHECK(snapshot.units.IsEmpty() && rig.io.active == 0 && snapshot.tempBytesInFlight == 0 &&
              snapshot.packagesInFlight == 0);
        REQUIRE(rig.loader->Shutdown());
        rig.loader.Reset();
    }

    CHECK(executed == StagedAllowed);
}

} // namespace loader_staged_combination_tests_file
