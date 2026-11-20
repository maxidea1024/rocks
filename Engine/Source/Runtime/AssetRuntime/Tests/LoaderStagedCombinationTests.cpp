#include "LoaderTestSupport.h"

#include <AssetTestFixtures/TestAssets.generated.h>

// 로더 단계 조합 시험이다(Docs/Design/AssetSystem.ko.md 11.6절 6·7·16, 14절 완료 조건 2): 요청 단위
// 모양(단일·둘 순환) × 의존 상태(없음·새로·진행 중 — 먼저 요청해 IO 보류·PostLoad 대기 — 먼저
// 공개하고 PostLoad를 Critical 감시 단위들이 가져가 대기에 둔다·공개됨) × 결함(없음·요청 패키지
// 손상·의존 손상) × 합류(없음·처음에 두 번째 요청·PostLoad 대기 때 요청) × 취소 시점(없음·허가
// 전·IO·worker·Ready· PostLoad 대기) × 완료 순서(앞부터·뒤부터) × PostLoad handler(없음·기록)의
// 허가 조합 전부.
//
// 취소 시점이 Ready·PostLoad 대기이고 결함이 있으면 실패가 먼저 확정된다(실패·취소 겹침). PostLoad
// 대기 중 취소는 그 요청만 Cancelled이고 공개된 단위는 PostLoad를 마친다. 조합마다 요청
// 상태(합류·진행 중 의존 요청 포함), 모든 구성·의존 패키지의 공개 여부, handler가 있으면 공개된
// 패키지마다 PostLoad가 정확히 한 번이고 의존이 먼저인지를 단언하고, 끝나면 단위·IO·임시 byte가
// 남지 않는지 본다. 번호 ↔ 조합 일대일과 실행 수로 빠짐을 본다.
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
    PostLoading,
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
    AtStart,
    AtPostLoading,
    Count
};

enum class StagedCancel : uint32 {
    None,
    BeforeAdmission,
    Io,
    Worker,
    Ready,
    PostLoading,
    Count
};

enum class StagedOrder : uint32 {
    Fifo,
    Lifo,
    Count
};

enum class StagedHandler : uint32 {
    None,
    Recording,
    Count
};

constexpr uint32 StagedShapes = static_cast<uint32>(StagedShape::Count);
constexpr uint32 StagedDependencies = static_cast<uint32>(StagedDependency::Count);
constexpr uint32 StagedFaults = static_cast<uint32>(StagedFault::Count);
constexpr uint32 StagedJoins = static_cast<uint32>(StagedJoin::Count);
constexpr uint32 StagedCancels = static_cast<uint32>(StagedCancel::Count);
constexpr uint32 StagedOrders = static_cast<uint32>(StagedOrder::Count);
constexpr uint32 StagedHandlers = static_cast<uint32>(StagedHandler::Count);
constexpr uint32 StagedSceneProduct =
    StagedShapes * StagedDependencies * StagedFaults * StagedJoins;
constexpr uint32 StagedRunProduct = StagedCancels * StagedOrders * StagedHandlers;
// 허가 조합 수다(금지: 의존 없음·PostLoad 대기·공개된 의존의 의존 손상, handler 없이 PostLoad 대기
// 의존·PostLoad 대기 때 합류·PostLoad 대기 때 취소).
//   handler 있음: (2 × 5 × 3 × 3 − 3 × 2 × 3) 장면 × 6 취소 × 2 순서 = 72 × 12 = 864
//   handler 없음: (2 × 4 × 3 × 2 − 2 × 2 × 2) 장면 × 5 취소 × 2 순서 = 40 × 10 = 400
constexpr uint32 StagedAllowed = 864 + 400;
constexpr uint64 StagedBase = 1000;
constexpr uint64 StagedStride = 10;
constexpr uint64 StagedDependencyOffset = 5;
// PostLoad를 가져가는 Critical 감시 단위다(handler가 있을 때 PostLoad 대기 상태를 만든다).
constexpr uint64 SentinelBase = 900;
constexpr uint32 Sentinels = 16;
constexpr uint32 StagedInFlight = 4096;

struct StagedScene {
    StagedShape shape;
    StagedDependency dependency;
    StagedFault fault;
    StagedJoin join;
};

struct StagedRun {
    StagedCancel cancel;
    StagedOrder order;
    StagedHandler handler;
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

StagedRun DecodeRun(uint32 index) noexcept {
    const uint32 cancel = index % StagedCancels;
    index /= StagedCancels;
    const uint32 order = index % StagedOrders;
    index /= StagedOrders;
    return StagedRun{static_cast<StagedCancel>(cancel), static_cast<StagedOrder>(order),
                     static_cast<StagedHandler>(index)};
}

uint32 EncodeRun(const StagedRun& run) noexcept {
    return (static_cast<uint32>(run.handler) * StagedOrders + static_cast<uint32>(run.order)) *
               StagedCancels +
           static_cast<uint32>(run.cancel);
}

bool AllowedScene(const StagedScene& scene) noexcept {
    return !(scene.fault == StagedFault::Dependency &&
             (scene.dependency == StagedDependency::None ||
              scene.dependency == StagedDependency::PostLoading ||
              scene.dependency == StagedDependency::Published));
}

bool AllowedRun(const StagedRun& run) noexcept {
    return !(run.handler == StagedHandler::None && run.cancel == StagedCancel::PostLoading);
}

bool Allowed(const StagedScene& scene, const StagedRun& run) noexcept {
    if (!AllowedScene(scene) || !AllowedRun(run)) {
        return false;
    }

    return run.handler == StagedHandler::Recording ||
           (scene.dependency != StagedDependency::PostLoading &&
            scene.join != StagedJoin::AtPostLoading);
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

// 취소가 닿지 않은 요청 결과가 기대와 같은지다.
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
               status.diagnostic.code == AssetDiagnosticCode::MissingDependency &&
               status.diagnostic.related == FixedId(base + StagedDependencyOffset);
    case StagedFault::Count:
        break;
    }

    return false;
}

// 첫 요청(취소 token 있음)의 기대 결과다. 결함이 있으면 Ready·PostLoad 대기 취소보다 실패가 먼저다.
bool FirstOutcome(const AssetLoadStatus& status, uint64 base, const StagedScene& combination,
                  StagedCancel cancel) noexcept {
    const bool lateCancel = cancel == StagedCancel::Ready || cancel == StagedCancel::PostLoading;
    if (cancel == StagedCancel::None || (lateCancel && combination.fault != StagedFault::None)) {
        return UncancelledOutcome(status, base, combination);
    }

    return status.state == AssetLoadState::Cancelled && status.error == ErrorCode::Cancelled;
}

struct StagedHandles {
    AssetLoadHandle first;
    AssetLoadHandle start;
    AssetLoadHandle late;
    AssetLoadHandle dependency;
};

} // namespace

// 단계 조합 전부(14절 완료 조건 2): 허가 조합 1264개(handler 있음 864, 없음 400)를 실행 22번으로
// 돈다.
TEST(AssetLoader_StagedCombinations) {
    for (uint32 index = 0; index < StagedSceneProduct; ++index) {
        CHECK(EncodeScene(DecodeScene(index)) == index);
    }

    for (uint32 index = 0; index < StagedRunProduct; ++index) {
        CHECK(EncodeRun(DecodeRun(index)) == index);
    }

    AssetEnv env;
    REQUIRE(env.Ready());
    LoaderRig rig(env);
    for (uint32 index = 0; index < StagedSceneProduct; ++index) {
        const StagedScene combination = DecodeScene(index);
        if (AllowedScene(combination)) {
            AddStagedScene(rig.scene, index, combination);
        }
    }

    for (uint32 sentinel = 0; sentinel < Sentinels; ++sentinel) {
        rig.scene.Add(SentinelBase + sentinel);
    }

    REQUIRE(rig.Cook());
    for (uint32 index = 0; index < StagedSceneProduct; ++index) {
        const StagedScene combination = DecodeScene(index);
        if (!AllowedScene(combination) || combination.fault == StagedFault::None) {
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
    bool seen[StagedSceneProduct * StagedRunProduct] = {};
    for (uint32 runIndex = 0; runIndex < StagedRunProduct; ++runIndex) {
        const StagedRun run = DecodeRun(runIndex);
        if (!AllowedRun(run)) {
            continue;
        }

        const bool handler = run.handler == StagedHandler::Recording;
        const bool reverse = run.order == StagedOrder::Lifo;
        const StagedCancel cancel = run.cancel;
        auto allowedHere = [&](const StagedScene& combination) noexcept {
            return Allowed(combination, run);
        };
        RecordingPostLoad recording(env.allocator);
        rig.config.postLoad = handler ? &recording : nullptr;
        REQUIRE(rig.Start());
        rig.io.hold = true;
        rig.worker.hold = true;
        // handler가 있으면 Pump마다 PostLoad 하나(감시 단위가 가져간다)로 PostLoad 대기를 만든다.
        AssetLoaderPumpBudget budget;
        if (handler) {
            budget.maxPostLoads = 0;
        }

        StagedHandles handles[StagedSceneProduct] = {};
        CancelSource sources[StagedSceneProduct];
        auto pump = [&]() noexcept { return rig.loader->Pump(budget).IsOk(); };
        auto complete = [&]() noexcept {
            if (reverse) {
                rig.io.CompleteReverse();
            } else {
                rig.io.Complete();
            }
        };
        auto runWorkers = [&]() noexcept {
            if (reverse) {
                rig.worker.RunAllReverse();
            } else {
                rig.worker.RunAll();
            }
        };
        // 공개된 의존: 먼저 로드한다(PostLoad까지).
        for (uint32 index = 0; index < StagedSceneProduct; ++index) {
            const StagedScene combination = DecodeScene(index);
            if (allowedHere(combination) && combination.dependency == StagedDependency::Published) {
                (void)rig.Request(BaseOf(index) + StagedDependencyOffset);
            }
        }

        REQUIRE(rig.SettleHeld(reverse));
        // PostLoad 대기 의존: 감시 단위와 함께 공개하고 PostLoad는 감시 단위가 가져간다.
        if (handler) {
            for (uint32 sentinel = 0; sentinel < Sentinels; ++sentinel) {
                (void)rig.Request(SentinelBase + sentinel, LoadPriority::Critical);
            }

            for (uint32 index = 0; index < StagedSceneProduct; ++index) {
                const StagedScene combination = DecodeScene(index);
                if (allowedHere(combination) &&
                    combination.dependency == StagedDependency::PostLoading) {
                    (void)rig.Request(BaseOf(index) + StagedDependencyOffset);
                }
            }

            REQUIRE(pump());
            complete();
            REQUIRE(pump());
            runWorkers();
            REQUIRE(pump());
        }

        // 진행 중 의존: 요청해 허가(IO 보류)까지 진행한다.
        for (uint32 index = 0; index < StagedSceneProduct; ++index) {
            const StagedScene combination = DecodeScene(index);
            if (allowedHere(combination) && combination.dependency == StagedDependency::InFlight) {
                handles[index].dependency = rig.Request(BaseOf(index) + StagedDependencyOffset);
            }
        }

        REQUIRE(pump());
        for (uint32 index = 0; index < StagedSceneProduct; ++index) {
            const StagedScene combination = DecodeScene(index);
            if (!allowedHere(combination)) {
                continue;
            }

            if (combination.dependency == StagedDependency::PostLoading) {
                // 의존은 공개됐고 아직 PostLoad 전이다.
                const uint64 dependency = BaseOf(index) + StagedDependencyOffset;
                CHECK(rig.IsLoaded(dependency) && recording.CountOf(FixedId(dependency)) == 0);
            }

            CancelToken token;
            if (cancel != StagedCancel::None) {
                auto made = CancelSource::Create(env.allocator);
                REQUIRE(made);
                sources[index] = Move(made.Value());
                token = sources[index].Token();
            }

            handles[index].first = rig.Request(BaseOf(index), LoadPriority::Normal, Move(token));
            if (combination.join == StagedJoin::AtStart) {
                handles[index].start = rig.Request(BaseOf(index));
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

        REQUIRE(pump());
        if (cancel == StagedCancel::Io) {
            cancelAll();
        }

        complete();
        REQUIRE(pump());
        if (cancel == StagedCancel::Worker) {
            cancelAll();
            REQUIRE(pump());
        }

        runWorkers();
        if (cancel == StagedCancel::Ready) {
            cancelAll();
        }

        if (handler) {
            // 공개까지 하고 PostLoad는 감시 단위가 가져간다 — 조합 단위는 PostLoad 대기다.
            REQUIRE(pump());
            for (uint32 index = 0; index < StagedSceneProduct; ++index) {
                const StagedScene combination = DecodeScene(index);
                if (allowedHere(combination) && combination.join == StagedJoin::AtPostLoading) {
                    handles[index].late = rig.Request(BaseOf(index));
                }
            }

            if (cancel == StagedCancel::PostLoading) {
                cancelAll();
            }
        }

        REQUIRE(rig.SettleHeld(reverse));
        for (uint32 index = 0; index < StagedSceneProduct; ++index) {
            const StagedScene combination = DecodeScene(index);
            if (!allowedHere(combination)) {
                continue;
            }

            const uint64 base = BaseOf(index);
            const AssetLoadStatus first = rig.StatusOf(handles[index].first);
            bool ok = FirstOutcome(first, base, combination, cancel);
            bool membersLoaded =
                first.state == AssetLoadState::Succeeded ||
                (cancel == StagedCancel::PostLoading && combination.fault == StagedFault::None);
            if (combination.join == StagedJoin::AtStart) {
                const AssetLoadStatus second = rig.StatusOf(handles[index].start);
                ok = ok && UncancelledOutcome(second, base, combination);
                membersLoaded = membersLoaded || second.state == AssetLoadState::Succeeded;
            } else if (combination.join == StagedJoin::AtPostLoading) {
                const AssetLoadStatus late = rig.StatusOf(handles[index].late);
                ok = ok && UncancelledOutcome(late, base, combination);
                membersLoaded = membersLoaded || late.state == AssetLoadState::Succeeded;
            }

            const uint32 members = combination.shape == StagedShape::Pair ? 2 : 1;
            for (uint32 member = 0; member < members; ++member) {
                ok = ok && rig.IsLoaded(base + member) == membersLoaded;
                if (handler) {
                    ok = ok &&
                         recording.CountOf(FixedId(base + member)) == (membersLoaded ? 1u : 0u);
                }
            }

            const uint64 dependency = base + StagedDependencyOffset;
            bool dependencyLoaded = false;
            switch (combination.dependency) {
            case StagedDependency::None:
                break;
            case StagedDependency::New:
                dependencyLoaded = membersLoaded;
                break;
            case StagedDependency::InFlight:
                dependencyLoaded = combination.fault != StagedFault::Dependency;
                ok = ok && (rig.StateOf(handles[index].dependency) == AssetLoadState::Succeeded) ==
                               dependencyLoaded;
                break;
            case StagedDependency::PostLoading:
            case StagedDependency::Published:
                dependencyLoaded = true;
                break;
            case StagedDependency::Count:
                ok = false;
                break;
            }

            if (combination.dependency != StagedDependency::None) {
                ok = ok && rig.IsLoaded(dependency) == dependencyLoaded;
                if (handler) {
                    ok = ok &&
                         recording.CountOf(FixedId(dependency)) == (dependencyLoaded ? 1u : 0u);
                    // 의존의 PostLoad가 구성 패키지보다 먼저다.
                    ok = ok && (!membersLoaded || recording.IndexOf(FixedId(dependency)) <
                                                      recording.IndexOf(FixedId(base)));
                }
            }

            if (!ok) {
                test::RecordFailure("staged loader combination", __FILE__, __LINE__);
            }

            const uint32 slot = EncodeScene(combination) * StagedRunProduct + runIndex;
            CHECK(!seen[slot]);
            seen[slot] = true;
            ++executed;
        }

        AssetLoaderSnapshot snapshot(env.allocator);
        REQUIRE(rig.loader->Describe(snapshot));
        CHECK(snapshot.units.IsEmpty() && rig.io.active == 0 && snapshot.tempBytesInFlight == 0 &&
              snapshot.packagesInFlight == 0);
        if (handler) {
            CHECK(recording.PackagesMatch(*rig.loader));
        }

        REQUIRE(rig.loader->Shutdown());
        rig.loader.Reset();
    }

    CHECK(executed == StagedAllowed);
}

} // namespace loader_staged_combination_tests_file
