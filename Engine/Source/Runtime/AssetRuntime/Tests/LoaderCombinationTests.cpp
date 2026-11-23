#include "LoaderTestSupport.h"

#include <AssetTestFixtures/TestAssets.generated.h>

// 로더 조합 시험이다(Docs/Design/AssetSystem 11.6절, 14절 완료 조건 2): 요청 단위
// 모양(단일·둘 순환·셋 순환) × 의존(없음·Hard 단일·Hard 순환·Soft) × 결함(없음·요청 패키지
// 손상·같은 단위 형제 손상·의존 손상·취소) × 진행 방식(바로 완료 — 취소는 첫 Pump 전, 붙잡음 —
// IO·worker를 시험이 내보내고 취소는 IO 중)의 허가 조합 전부를 한 매니페스트에 담아 돌리고,
// 조합마다 기대 결과를 단언한다. 번호 ↔ 조합 일대일과 실행한 조합 수로 빠짐을 본다.
using namespace rocks;
namespace loader_combination_tests_file {

using namespace rocks::asset_test;

namespace {

enum class UnitShape : uint32 {
    Single,
    Pair,
    Triple,
    Count
};

enum class DependencyShape : uint32 {
    None,
    HardSingle,
    HardCycle,
    Soft,
    Count
};

enum class Fault : uint32 {
    None,
    Requested,
    Sibling,
    Dependency,
    Cancel,
    Count
};

enum class Mode : uint32 {
    Immediate,
    Held,
    Count
};

constexpr uint32 Shapes = static_cast<uint32>(UnitShape::Count);
constexpr uint32 Dependencies = static_cast<uint32>(DependencyShape::Count);
constexpr uint32 Faults = static_cast<uint32>(Fault::Count);
constexpr uint32 Modes = static_cast<uint32>(Mode::Count);
// 금지 조합: 단일 단위의 형제 손상(형제가 없다), 의존 없음의 의존 손상(의존이 없다).
constexpr uint32 SceneCombinations = Shapes * Dependencies * Faults - Dependencies - Shapes;
constexpr uint32 AllowedCombinations = SceneCombinations * Modes;
// 조합 하나의 id 간격이다(요청 단위 base + 0..2, 의존 base + 5·6).
constexpr uint64 ComboStride = 10;
constexpr uint64 ComboBase = 100;
constexpr uint64 DependencyOffset = 5;

struct Combination {
    UnitShape shape;
    DependencyShape dependency;
    Fault fault;
};

bool Allowed(const Combination& combination) noexcept {
    return !(combination.shape == UnitShape::Single && combination.fault == Fault::Sibling) &&
           !(combination.dependency == DependencyShape::None &&
             combination.fault == Fault::Dependency);
}

// 전체 곱의 번호 ↔ 조합이다.
Combination Decode(uint32 index) noexcept {
    const uint32 fault = index % Faults;
    index /= Faults;
    const uint32 dependency = index % Dependencies;
    index /= Dependencies;
    return Combination{static_cast<UnitShape>(index), static_cast<DependencyShape>(dependency),
                       static_cast<Fault>(fault)};
}

uint32 Encode(const Combination& combination) noexcept {
    return (static_cast<uint32>(combination.shape) * Dependencies +
            static_cast<uint32>(combination.dependency)) *
               Faults +
           static_cast<uint32>(combination.fault);
}

uint32 MemberCount(UnitShape shape) noexcept {
    return shape == UnitShape::Single ? 1 : shape == UnitShape::Pair ? 2 : 3;
}

// 조합 하나를 장면에 더하고 손상할 패키지 id를 준다(없으면 0).
uint64 AddCombination(LoaderScene& scene, uint64 base, const Combination& combination) noexcept {
    const uint32 members = MemberCount(combination.shape);
    for (uint32 member = 0; member < members; ++member) {
        scene.Add(base + member);
    }

    for (uint32 member = 0; member + 1 < members; ++member) {
        scene.Hard(base + member, base + member + 1);
    }

    if (members > 1) {
        scene.Hard(base + members - 1, base);
    }

    const uint64 dependency = base + DependencyOffset;
    if (combination.dependency == DependencyShape::HardSingle) {
        scene.Add(dependency);
        scene.Hard(base, dependency);
    } else if (combination.dependency == DependencyShape::HardCycle) {
        scene.Add(dependency);
        scene.Add(dependency + 1);
        scene.Hard(dependency, dependency + 1);
        scene.Hard(dependency + 1, dependency);
        scene.Hard(base, dependency);
    } else if (combination.dependency == DependencyShape::Soft) {
        scene.Add(dependency);
        scene.Soft(base, dependency);
    }

    switch (combination.fault) {
    case Fault::Requested:
        return base;
    case Fault::Sibling:
        return base + 1;
    case Fault::Dependency:
        return combination.dependency == DependencyShape::HardCycle ? dependency + 1 : dependency;
    case Fault::None:
    case Fault::Cancel:
    case Fault::Count:
        break;
    }

    return 0;
}

// 조합의 기대 결과를 단언한다.
bool Expected(const AssetLoadStatus& status, uint64 base, const Combination& combination,
              uint64 corrupt) noexcept {
    const bool succeeded = status.state == AssetLoadState::Succeeded;
    switch (combination.fault) {
    case Fault::None:
        return succeeded;
    case Fault::Cancel:
        return status.state == AssetLoadState::Cancelled && status.error == ErrorCode::Cancelled;
    case Fault::Requested:
        return status.state == AssetLoadState::Failed &&
               status.diagnostic.code == AssetDiagnosticCode::CorruptPackage &&
               status.diagnostic.asset == FixedId(base) &&
               status.diagnostic.section == static_cast<uint32>(CookedManifestField::PackageSha256);
    case Fault::Sibling:
    case Fault::Dependency:
        if (combination.dependency == DependencyShape::Soft &&
            combination.fault == Fault::Dependency) {
            return succeeded;
        }

        return status.state == AssetLoadState::Failed &&
               status.error == ErrorCode::DependencyFailed &&
               status.diagnostic.code == AssetDiagnosticCode::MissingDependency &&
               status.diagnostic.asset == FixedId(base) &&
               status.diagnostic.related == FixedId(corrupt);
    case Fault::Count:
        break;
    }

    return false;
}

} // namespace

// 조합 전부(14절 완료 조건 2): 허가 조합 53개 × 진행 방식 2를 돌려 조합마다 기대
// 결과(성공·취소·자기 손상·DependencyFailed(related = 손상 패키지)·Soft 의존 손상은 성공)를 보고,
// 번호 ↔ 조합이 일대일이며 실행한 조합 수가 AllowedCombinations와 같은지 본다.
TEST(AssetLoader_AllCombinations) {
    constexpr uint32 Product = Shapes * Dependencies * Faults;
    for (uint32 index = 0; index < Product; ++index) {
        CHECK(Encode(Decode(index)) == index);
    }

    AssetEnv env;
    REQUIRE(env.Ready());
    LoaderRig rig(env);
    uint64 corrupt[Product] = {};
    uint32 allowed = 0;
    for (uint32 index = 0; index < Product; ++index) {
        const Combination combination = Decode(index);
        if (!Allowed(combination)) {
            continue;
        }

        corrupt[index] = AddCombination(rig.scene, ComboBase + index * ComboStride, combination);
        ++allowed;
    }

    REQUIRE(allowed == SceneCombinations);
    REQUIRE(rig.Cook());
    for (uint32 index = 0; index < Product; ++index) {
        if (corrupt[index] != 0) {
            rig.scene.FileOf(corrupt[index])[70] ^= 0x01;
        }
    }

    uint32 executed = 0;
    bool seen[Product * Modes] = {};
    for (uint32 mode = 0; mode < Modes; ++mode) {
        const bool held = static_cast<Mode>(mode) == Mode::Held;
        REQUIRE(rig.Start());
        rig.io.hold = held;
        rig.worker.hold = held;
        AssetLoadHandle handles[Product] = {};
        CancelSource sources[Product];
        for (uint32 index = 0; index < Product; ++index) {
            const Combination combination = Decode(index);
            if (!Allowed(combination)) {
                continue;
            }

            CancelToken token;
            if (combination.fault == Fault::Cancel) {
                auto made = CancelSource::Create(env.allocator);
                REQUIRE(made);
                sources[index] = Move(made.Value());
                token = sources[index].Token();
            }

            handles[index] =
                rig.Request(ComboBase + index * ComboStride, LoadPriority::Normal, Move(token));
            REQUIRE(handles[index].IsValid());
            if (!held && combination.fault == Fault::Cancel) {
                sources[index].Cancel();
            }
        }

        if (held) {
            // 첫 Pump로 IO에 넣은 뒤 취소한다.
            REQUIRE(rig.loader->Pump());
            for (CancelSource& source : sources) {
                source.Cancel();
            }

            for (uint32 round = 0; round < 1000; ++round) {
                rig.io.Complete();
                rig.worker.RunAll();
                REQUIRE(rig.loader->Pump());
                if (rig.io.held.IsEmpty() && rig.worker.pending.IsEmpty() && rig.io.active == 0) {
                    AssetLoaderSnapshot snapshot(env.allocator);
                    REQUIRE(rig.loader->Describe(snapshot));
                    if (snapshot.units.IsEmpty()) {
                        break;
                    }
                }
            }
        } else {
            REQUIRE(rig.PumpUntilSettled());
        }

        for (uint32 index = 0; index < Product; ++index) {
            const Combination combination = Decode(index);
            if (!Allowed(combination)) {
                continue;
            }

            const AssetLoadStatus status = rig.StatusOf(handles[index]);
            if (!Expected(status, ComboBase + index * ComboStride, combination, corrupt[index])) {
                test::RecordFailure("loader combination result", __FILE__, __LINE__);
            }

            const uint32 slot = Encode(combination) * Modes + mode;
            CHECK(!seen[slot]);
            seen[slot] = true;
            ++executed;
        }

        REQUIRE(rig.loader->Shutdown());
        rig.loader.Reset();
    }

    CHECK(executed == AllowedCombinations);
}

} // namespace loader_combination_tests_file
