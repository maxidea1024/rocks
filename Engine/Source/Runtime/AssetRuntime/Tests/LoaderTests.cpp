#include "LoaderTestSupport.h"

#include <AssetTestFixtures/TestAssets.generated.h>
#include <Platform/Thread.h>
#include <TestSupport/NFailAllocator.h>

// 로더 시험이다(Docs/Design/AssetSystem.ko.md 11.6절 A1c): Hard 폐포·SCC 단위·합류, SCC 모양(단일·
// 둘·셋 순환·사슬·다이아몬드·큰 그래프), 우선순위·aging·승격, 동시 진행·임시 메모리 상한, Pump
// 예산, 상태 관측, 경로 색인, 초기화·인자·스레드 오류. IO와 worker는 시험이 내보내는 구현이라
// 순서가 결정적이다.
using namespace rocks;
namespace loader_tests_file {

using namespace rocks::asset_test;

namespace {

const AssetLoaderUnitInfo* UnitOf(const AssetLoaderSnapshot& snapshot, uint64 id) noexcept {
    for (const AssetLoaderUnitInfo& unit : snapshot.units) {
        if (unit.asset == FixedId(id)) {
            return &unit;
        }
    }

    return nullptr;
}

bool Snapshot(LoaderRig& rig, AssetLoaderSnapshot& snapshot) noexcept {
    return rig.loader->Describe(snapshot).IsOk();
}

// begun 경로 목록에서 id 패키지가 몇 번째로 시작됐는지다(없으면 begun 크기).
usize BegunAt(LoaderRig& rig, uint64 id) noexcept {
    const StringView path = rig.scene.PathOf(id);
    for (usize index = 0; index < rig.io.begun.Size(); ++index) {
        if (rig.io.begun[index].View() == path) {
            return index;
        }
    }

    return rig.io.begun.Size();
}

bool AllSucceeded(LoaderRig& rig, Span<const AssetLoadHandle> handles) noexcept {
    for (const AssetLoadHandle& handle : handles) {
        if (rig.StateOf(handle) != AssetLoadState::Succeeded) {
            return false;
        }
    }

    return true;
}

} // namespace

// Hard 폐포·합류(11.6절 1·2·7): 사슬 1 → 2 → 3은 단위 셋(의존 먼저 생성)이고 대기 이유가 허가·의존
// 허가다. 같은 에셋·진행 중 의존 에셋 요청은 합류한다(단위 수 그대로, 관심 수 증가). 끝나면 모두
// Succeeded, 공개한 객체의 Hard 참조가 의존 패키지의 대표 에셋을 가리킨다. 공개한 패키지 요청은 그
// 자리에서 Succeeded이고 다시 읽지 않는다. Soft 대상(쿠킹되지 않은 99, 공개된 3)은 따라가지 않는다.
TEST(AssetLoader_LoadsChainAndJoins) {
    AssetEnv env;
    REQUIRE(env.Ready());
    LoaderRig rig(env);
    for (uint64 id = 1; id <= 5; ++id) {
        rig.scene.Add(id);
    }

    rig.scene.Hard(1, 2);
    rig.scene.Hard(2, 3);
    rig.scene.Soft(4, 99);
    rig.scene.Hard(5, 3);
    REQUIRE(rig.Cook());
    REQUIRE(rig.Start());

    const AssetLoadHandle first = rig.Request(1);
    REQUIRE(first.IsValid());
    AssetLoaderSnapshot snapshot(env.allocator);
    REQUIRE(Snapshot(rig, snapshot));
    REQUIRE(snapshot.units.Size() == 3);
    const AssetLoaderUnitInfo* three = UnitOf(snapshot, 3);
    const AssetLoaderUnitInfo* two = UnitOf(snapshot, 2);
    const AssetLoaderUnitInfo* one = UnitOf(snapshot, 1);
    REQUIRE(three != nullptr && two != nullptr && one != nullptr);
    CHECK(three->phase == AssetLoaderUnitPhase::Queued &&
          three->waiting == AssetLoaderWait::Admission && three->interest == 1);
    CHECK(two->waiting == AssetLoaderWait::DependencyAdmission && two->waitingOn == FixedId(3));
    CHECK(one->waiting == AssetLoaderWait::DependencyAdmission && one->waitingOn == FixedId(2));
    CHECK(one->interest == 1 && one->packages == 1);

    const AssetLoadHandle again = rig.Request(1);
    const AssetLoadHandle middle = rig.Request(2, LoadPriority::High);
    REQUIRE(again.IsValid() && middle.IsValid());
    REQUIRE(Snapshot(rig, snapshot));
    CHECK(snapshot.units.Size() == 3 && snapshot.requests.Size() == 3);
    CHECK(UnitOf(snapshot, 1)->interest == 2 && UnitOf(snapshot, 2)->interest == 2);
    // 합류 요청의 High가 의존 단위로 전파된다.
    CHECK(UnitOf(snapshot, 3)->priority == LoadPriority::High &&
          UnitOf(snapshot, 1)->priority == LoadPriority::Normal);

    // 첫 Pump로 3이 허가된 뒤의 요청 5(→ 3)는 허가된 의존 단위에 기댄다(미허가 의존 0).
    REQUIRE(rig.loader->Pump());
    const AssetLoadHandle late = rig.Request(5);
    REQUIRE(Snapshot(rig, snapshot));
    CHECK(UnitOf(snapshot, 5)->waiting == AssetLoaderWait::Admission);
    REQUIRE(rig.PumpUntilSettled());
    CHECK(rig.StateOf(late) == AssetLoadState::Succeeded);
    const AssetLoadHandle handles[] = {first, again, middle};
    CHECK(AllSucceeded(rig, handles));
    CHECK(rig.IsLoaded(1) && rig.IsLoaded(2) && rig.IsLoaded(3) && !rig.IsLoaded(4));
    CHECK(rig.StatusOf(first).package == rig.loader->FindPackage(FixedId(1)).Value());
    TestAsset* asset = Pointer<TestAsset>(*env.target, rig.AssetOf(1));
    REQUIRE(asset != nullptr && asset->hardList.Size() == 1);
    CHECK(asset->hardList[0] == rig.AssetOf(2) && asset->value == 1);

    const AssetLoadHandle loaded = rig.Request(3);
    CHECK(rig.StateOf(loaded) == AssetLoadState::Succeeded);
    CHECK(rig.StatusOf(loaded).package == rig.loader->FindPackage(FixedId(3)).Value());
    const AssetLoadHandle soft = rig.Request(4);
    REQUIRE(rig.PumpUntilSettled());
    CHECK(rig.StateOf(soft) == AssetLoadState::Succeeded);
    CHECK(rig.io.begun.Size() == 5 && rig.io.released == 5 && rig.io.active == 0);
    REQUIRE(Snapshot(rig, snapshot));
    CHECK(snapshot.loadedPackages == 5 && snapshot.packagesInFlight == 0 &&
          snapshot.tempBytesInFlight == 0 && snapshot.units.IsEmpty());
    const AssetLoadHandle released[] = {first, again, middle, loaded, soft};
    for (AssetLoadHandle handle : released) {
        CHECK(rig.loader->Release(handle));
    }

    CHECK(rig.loader->Status(first).Error() == ErrorCode::StaleHandle);
    // 돌려준 칸을 새 요청이 다시 쓰면 옛 handle은 세대가 달라 StaleHandle이다.
    const AssetLoadHandle reused = rig.Request(1);
    CHECK(reused.index == soft.index && reused.generation != soft.generation);
    CHECK(rig.loader->Status(soft).Error() == ErrorCode::StaleHandle);
    CHECK(rig.StateOf(reused) == AssetLoadState::Succeeded);
}

// SCC 모양(11.6절 2·10): 단일(10), 둘 순환(20 ↔ 21), 셋 순환(30 → 31 → 32 → 30), 다이아몬드(40 →
// 41·42 → 43), 진행 중 순환에 기대는 사슬(51 → 50 → 20), 꼬리가 나가는 순환(60 ↔ 61 → 10). 요청
// 직후 단위 수·구성 수가 SCC와 같고, Pump마다 한 단위만 공개해도 SCC 구성은 함께 공개되며(같은
// 트랜잭션), 순환 참조가 서로의 대표 에셋을 가리킨다.
TEST(AssetLoader_HardSccShapes) {
    AssetEnv env;
    REQUIRE(env.Ready());
    LoaderRig rig(env);
    const uint64 ids[] = {10, 20, 21, 30, 31, 32, 40, 41, 42, 43, 50, 51, 60, 61};
    for (uint64 id : ids) {
        rig.scene.Add(id);
    }

    rig.scene.Hard(20, 21);
    rig.scene.Hard(21, 20);
    rig.scene.Hard(30, 31);
    rig.scene.Hard(31, 32);
    rig.scene.Hard(32, 30);
    rig.scene.Hard(32, 31);
    rig.scene.Hard(40, 41);
    rig.scene.Hard(40, 42);
    rig.scene.Hard(41, 43);
    rig.scene.Hard(42, 43);
    rig.scene.Hard(50, 20);
    rig.scene.Hard(50, 21);
    rig.scene.Hard(51, 50);
    rig.scene.Hard(60, 61);
    rig.scene.Hard(61, 60);
    rig.scene.Hard(61, 10);
    REQUIRE(rig.Cook());
    REQUIRE(rig.Start());

    AssetLoaderSnapshot snapshot(env.allocator);
    Array<AssetLoadHandle> handles(env.allocator);
    REQUIRE(handles.TryPushBack(rig.Request(21)));
    REQUIRE(Snapshot(rig, snapshot));
    REQUIRE(snapshot.units.Size() == 1);
    CHECK(UnitOf(snapshot, 20) != nullptr && UnitOf(snapshot, 20)->packages == 2);
    REQUIRE(handles.TryPushBack(rig.Request(32)));
    REQUIRE(Snapshot(rig, snapshot));
    CHECK(snapshot.units.Size() == 2 && UnitOf(snapshot, 30)->packages == 3);
    REQUIRE(handles.TryPushBack(rig.Request(40)));
    REQUIRE(Snapshot(rig, snapshot));
    CHECK(snapshot.units.Size() == 6 && UnitOf(snapshot, 43)->interest == 2 &&
          UnitOf(snapshot, 40)->packages == 1);
    // 51 → 50 → (진행 중 단위 20·21): 새 단위는 둘이고 기존 순환 단위에 합류한다.
    REQUIRE(handles.TryPushBack(rig.Request(51)));
    REQUIRE(Snapshot(rig, snapshot));
    CHECK(snapshot.units.Size() == 8 && UnitOf(snapshot, 20)->interest == 2);
    REQUIRE(handles.TryPushBack(rig.Request(60)));
    REQUIRE(Snapshot(rig, snapshot));
    CHECK(snapshot.units.Size() == 10 && UnitOf(snapshot, 60)->packages == 2 &&
          UnitOf(snapshot, 10)->packages == 1);

    // 한 Pump에 한 단위만 공개해도 순환 구성은 함께 공개된다.
    const uint64 groups[][3] = {{20, 21, 0}, {30, 31, 32}, {60, 61, 0}};
    bool atomic = true;
    AssetLoaderPumpBudget one;
    one.maxUnits = 1;
    for (uint32 round = 0; round < 200; ++round) {
        REQUIRE(rig.loader->Pump(one));
        for (const auto& group : groups) {
            uint32 loaded = 0;
            uint32 members = 0;
            for (uint64 id : group) {
                if (id != 0) {
                    ++members;
                    loaded += rig.IsLoaded(id) ? 1 : 0;
                }
            }

            atomic = atomic && (loaded == 0 || loaded == members);
        }
    }

    CHECK(atomic);
    CHECK(AllSucceeded(rig, handles.View()));
    for (uint64 id : ids) {
        CHECK(rig.IsLoaded(id));
    }

    TestAsset* twenty = Pointer<TestAsset>(*env.target, rig.AssetOf(20));
    TestAsset* twentyOne = Pointer<TestAsset>(*env.target, rig.AssetOf(21));
    REQUIRE(twenty != nullptr && twentyOne != nullptr);
    CHECK(twenty->hardList[0] == rig.AssetOf(21) && twentyOne->hardList[0] == rig.AssetOf(20));
    TestAsset* forty = Pointer<TestAsset>(*env.target, rig.AssetOf(40));
    REQUIRE(forty != nullptr && forty->hardList.Size() == 2);
    CHECK(forty->hardList[0] == rig.AssetOf(41) && forty->hardList[1] == rig.AssetOf(42));
}

// 큰 그래프(11.6절 2): 10개 단위 순환 블록 40개가 사슬로 이어진 400 패키지(단위 40개)와 깊이 1500
// Hard 사슬(반복 Tarjan — 재귀 깊이가 없다)을 기본 상한으로 끝까지 로드한다.
TEST(AssetLoader_LargeGraph) {
    AssetEnv env;
    REQUIRE(env.Ready());
    LoaderRig rig(env);
    constexpr uint64 BlockBase = 1000;
    constexpr uint32 BlockSize = 10;
    constexpr uint32 Blocks = 40;
    constexpr uint64 ChainBase = 5000;
    constexpr uint32 ChainLength = 1500;
    for (uint32 index = 0; index < BlockSize * Blocks; ++index) {
        rig.scene.Add(BlockBase + index);
    }

    for (uint32 index = 0; index < ChainLength; ++index) {
        rig.scene.Add(ChainBase + index);
    }

    for (uint32 index = 0; index + 1 < BlockSize * Blocks; ++index) {
        rig.scene.Hard(BlockBase + index, BlockBase + index + 1);
        if (index % BlockSize == BlockSize - 1) {
            // 블록 끝이 블록 처음을 가리켜 블록이 SCC가 된다.
            rig.scene.Hard(BlockBase + index, BlockBase + index - (BlockSize - 1));
        }
    }

    rig.scene.Hard(BlockBase + BlockSize * Blocks - 1, BlockBase + BlockSize * (Blocks - 1));
    for (uint32 index = 0; index + 1 < ChainLength; ++index) {
        rig.scene.Hard(ChainBase + index, ChainBase + index + 1);
    }

    REQUIRE(rig.Cook());
    REQUIRE(rig.Start());
    const AssetLoadHandle blocks = rig.Request(BlockBase);
    const AssetLoadHandle chain = rig.Request(ChainBase);
    AssetLoaderSnapshot snapshot(env.allocator);
    REQUIRE(Snapshot(rig, snapshot));
    CHECK(snapshot.units.Size() == Blocks + ChainLength);
    CHECK(UnitOf(snapshot, BlockBase)->packages == BlockSize);
    REQUIRE(rig.PumpUntilSettled(100000));
    CHECK(rig.StateOf(blocks) == AssetLoadState::Succeeded &&
          rig.StateOf(chain) == AssetLoadState::Succeeded);
    REQUIRE(Snapshot(rig, snapshot));
    CHECK(snapshot.loadedPackages == BlockSize * Blocks + ChainLength);
}

// 우선순위(11.6절 8): 동시 진행 1에서 허가 순서는 실효 우선순위 → 생성 순서다(Critical·High·
// Normal·Low). SetPriority 승격은 의존 단위로 전파되고(UE UpdatePackagePriorityRecursive) 내리는
// 값은 단위를 바꾸지 않는다. Pump는 공개 준비 단위를 우선순위 순으로 공개한다.
TEST(AssetLoader_PriorityOrder) {
    AssetEnv env;
    REQUIRE(env.Ready());
    LoaderRig rig(env);
    for (uint64 id = 1; id <= 9; ++id) {
        rig.scene.Add(id);
    }

    rig.scene.Hard(7, 8);
    REQUIRE(rig.Cook());
    rig.config.limits.maxPackagesInFlight = 1;
    REQUIRE(rig.Start());
    rig.io.hold = true;
    const AssetLoadHandle first = rig.Request(1, LoadPriority::Low);
    REQUIRE(rig.loader->Pump());
    REQUIRE(rig.io.begun.Size() == 1);
    const AssetLoadHandle low = rig.Request(2, LoadPriority::Low);
    const AssetLoadHandle high = rig.Request(3, LoadPriority::High);
    const AssetLoadHandle normal = rig.Request(4, LoadPriority::Normal);
    const AssetLoadHandle critical = rig.Request(5, LoadPriority::Critical);
    // 승격 전파: 7(Low) → 8(Low)을 Critical로 올리면 8이 Normal 9보다 먼저다.
    const AssetLoadHandle nine = rig.Request(9, LoadPriority::Normal);
    const AssetLoadHandle seven = rig.Request(7, LoadPriority::Low);
    REQUIRE(rig.loader->SetPriority(seven, LoadPriority::Critical));
    AssetLoaderSnapshot snapshot(env.allocator);
    REQUIRE(Snapshot(rig, snapshot));
    CHECK(UnitOf(snapshot, 8)->priority == LoadPriority::Critical);
    // 내리는 값은 요청만 바꾼다.
    REQUIRE(rig.loader->SetPriority(seven, LoadPriority::Low));
    REQUIRE(Snapshot(rig, snapshot));
    CHECK(UnitOf(snapshot, 7)->priority == LoadPriority::Critical);
    // 요청한 패키지는 1·2·3·4·5·7·8·9 여덟이다.
    for (uint32 round = 0; round < 64 && rig.io.begun.Size() < 8; ++round) {
        rig.io.Complete();
        REQUIRE(rig.loader->Pump());
    }

    REQUIRE(rig.io.begun.Size() == 8);
    // 5(Critical, 먼저 생성) → 8 → 7(승격 Critical) → 3(High) → 4·9(Normal, 생성 순) → 2(Low).
    const uint64 expected[] = {1, 5, 8, 7, 3, 4, 9, 2};
    for (usize index = 0; index < 8; ++index) {
        CHECK(BegunAt(rig, expected[index]) == index);
    }

    rig.io.hold = false;
    rig.io.Complete();
    REQUIRE(rig.PumpUntilSettled());
    const AssetLoadHandle all[] = {first, low, high, normal, critical, nine, seven};
    CHECK(AllSucceeded(rig, all));
    // 끝난 요청의 SetPriority는 아무것도 하지 않는다.
    CHECK(rig.loader->SetPriority(seven, LoadPriority::High));
    CHECK(rig.loader->SetPriority(seven, static_cast<LoadPriority>(LoadPriorityCount)).Error() ==
          ErrorCode::InvalidArgument);
}

// 공개 순서(11.6절 10): 준비된 두 단위(먼저 만든 Low, 나중 High)를 단위 하나 예산으로 Pump하면
// High를 먼저 공개한다.
TEST(AssetLoader_InstantiateOrderFollowsPriority) {
    AssetEnv env;
    REQUIRE(env.Ready());
    LoaderRig rig(env);
    rig.scene.Add(1);
    rig.scene.Add(2);
    REQUIRE(rig.Cook());
    REQUIRE(rig.Start());
    rig.io.hold = true;
    const AssetLoadHandle low = rig.Request(1, LoadPriority::Low);
    const AssetLoadHandle high = rig.Request(2, LoadPriority::High);
    REQUIRE(rig.loader->Pump());
    rig.io.Complete();
    REQUIRE(rig.loader->Pump());
    AssetLoaderPumpBudget one;
    one.maxUnits = 1;
    auto pumped = rig.loader->Pump(one);
    REQUIRE(pumped);
    CHECK(pumped.Value().instantiatedUnits == 1 && pumped.Value().budgetExhausted);
    CHECK(rig.IsLoaded(2) && !rig.IsLoaded(1));
    CHECK(rig.StateOf(high) == AssetLoadState::Succeeded &&
          rig.StateOf(low) == AssetLoadState::Pending);
    REQUIRE(rig.loader->Pump(one));
    CHECK(rig.StateOf(low) == AssetLoadState::Succeeded);
}

// aging(11.6절 9): 허가 횟수로 실효 우선순위가 오른다. agingAdmissions 1이면 첫 허가 전에 만든
// Low가 그 뒤에 만든 High 셋 가운데 둘이 허가된 뒤 셋째 High와 실효 우선순위가 같아지고(둘 다
// Critical 상한) 생성 순서로 먼저다. aging이 없으면(큰 값) Low는 맨 뒤다.
TEST(AssetLoader_AgingPreventsStarvation) {
    const uint32 agings[] = {1, 1000};
    for (uint32 aging : agings) {
        AssetEnv env;
        REQUIRE(env.Ready());
        LoaderRig rig(env);
        for (uint64 id = 1; id <= 5; ++id) {
            rig.scene.Add(id);
        }

        REQUIRE(rig.Cook());
        rig.config.limits.maxPackagesInFlight = 1;
        rig.config.limits.agingAdmissions = aging;
        REQUIRE(rig.Start());
        rig.io.hold = true;
        // Low(2)는 첫 허가 전에 만들어 High들보다 오래 기다린다.
        (void)rig.Request(1, LoadPriority::Normal);
        (void)rig.Request(2, LoadPriority::Low);
        REQUIRE(rig.loader->Pump());
        (void)rig.Request(3, LoadPriority::High);
        (void)rig.Request(4, LoadPriority::High);
        (void)rig.Request(5, LoadPriority::High);
        for (uint32 round = 0; round < 64 && rig.io.begun.Size() < 5; ++round) {
            rig.io.Complete();
            REQUIRE(rig.loader->Pump());
        }

        REQUIRE(rig.io.begun.Size() == 5);
        if (aging == 1) {
            CHECK(BegunAt(rig, 3) == 1 && BegunAt(rig, 4) == 2 && BegunAt(rig, 2) == 3 &&
                  BegunAt(rig, 5) == 4);
        } else {
            CHECK(BegunAt(rig, 3) == 1 && BegunAt(rig, 4) == 2 && BegunAt(rig, 5) == 3 &&
                  BegunAt(rig, 2) == 4);
        }

        rig.io.hold = false;
        rig.io.Complete();
        REQUIRE(rig.PumpUntilSettled());
    }
}

// 동시 진행·임시 메모리 상한(11.6절 9): maxPackagesInFlight 2면 IO에 동시에 있는 패키지가 2를 넘지
// 않는다. 상한보다 큰 단위(셋 순환)는 앞에 허가된 것이 있으면 기다리고(뒤 후보도 건너뛰지 않는다 —
// 머리 막힘), 진행 중이 없으면 혼자 허가된다. maxTempBytes가 패키지 하나보다 작아도 하나씩
// 진행한다.
TEST(AssetLoader_AdmissionLimits) {
    AssetEnv env;
    REQUIRE(env.Ready());
    LoaderRig rig(env);
    for (uint64 id = 1; id <= 8; ++id) {
        rig.scene.Add(id, id == 1 ? 4096 : 0);
    }

    rig.scene.Hard(5, 6);
    rig.scene.Hard(6, 7);
    rig.scene.Hard(7, 5);
    REQUIRE(rig.Cook());
    rig.config.limits.maxPackagesInFlight = 2;
    REQUIRE(rig.Start());
    rig.io.hold = true;
    for (uint64 id = 1; id <= 4; ++id) {
        (void)rig.Request(id);
    }

    REQUIRE(rig.loader->Pump());
    CHECK(rig.io.begun.Size() == 2 && rig.io.active == 2);
    AssetLoaderSnapshot snapshot(env.allocator);
    REQUIRE(Snapshot(rig, snapshot));
    CHECK(snapshot.packagesInFlight == 2);
    CHECK(UnitOf(snapshot, 3)->waiting == AssetLoaderWait::Admission);
    const CookedManifestEntry& big = rig.scene.EntryOf(1);
    CHECK(snapshot.tempBytesInFlight ==
          big.packageBytes + big.bulkRawBytes + rig.scene.EntryOf(2).packageBytes);
    // 셋 순환(5·6·7)은 상한 2보다 크다: 앞이 진행 중이면 기다리고, 그 뒤 8도 허가하지 않는다.
    (void)rig.Request(5, LoadPriority::Critical);
    (void)rig.Request(8, LoadPriority::Low);
    uint32 peak = 0;
    bool cycleAlone = false;
    for (uint32 round = 0; round < 200; ++round) {
        rig.io.Complete(1);
        REQUIRE(rig.loader->Pump());
        peak = rig.io.active > peak ? rig.io.active : peak;
        if (rig.io.active == 3) {
            cycleAlone = BegunAt(rig, 8) == rig.io.begun.Size();
        }

        if (rig.io.begun.Size() == 8 && rig.io.held.IsEmpty()) {
            break;
        }
    }

    CHECK(peak == 3 && cycleAlone);
    CHECK(BegunAt(rig, 5) < BegunAt(rig, 8));
    rig.io.hold = false;
    REQUIRE(rig.PumpUntilSettled());
    for (uint64 id = 1; id <= 8; ++id) {
        CHECK(rig.IsLoaded(id));
    }

    // 임시 메모리 상한이 패키지 하나보다 작아도 하나씩 진행한다.
    AssetEnv small;
    REQUIRE(small.Ready());
    LoaderRig tight(small);
    tight.scene.Add(1, 4096);
    tight.scene.Add(2, 4096);
    REQUIRE(tight.Cook());
    tight.config.limits.maxTempBytes = 1;
    REQUIRE(tight.Start());
    tight.io.hold = true;
    (void)tight.Request(1);
    (void)tight.Request(2);
    REQUIRE(tight.loader->Pump());
    CHECK(tight.io.begun.Size() == 1);
    tight.io.hold = false;
    tight.io.Complete();
    REQUIRE(tight.PumpUntilSettled());
    CHECK(tight.IsLoaded(1) && tight.IsLoaded(2));
}

// Pump 예산(11.6절 10): 공개 준비 단위 넷에서 maxUnits 2는 둘(예산 소진 표시), 0은 하나(최소 진행),
// 무제한은 남은 것 전부(소진 아님). 시간 예산은 단계마다 10 ns 가는 시계로 경계를 본다: 10 → 1,
// 11 → 2, 20 → 2, 21 → 3, 31 → 4(준비 단위가 없어 시계를 더 읽지 않는다).
TEST(AssetLoader_PumpBudgetBoundaries) {
    struct Case {
        uint32 maxUnits;
        uint64 maxNanoseconds;
        uint32 expected;
        bool exhausted;
    };
    constexpr uint64 Step = 10;
    const Case cases[] = {
        {2, AssetLoaderPumpBudget::UnlimitedNanoseconds, 2, true},
        {0, AssetLoaderPumpBudget::UnlimitedNanoseconds, 1, true},
        {AssetLoaderPumpBudget::UnlimitedUnits, AssetLoaderPumpBudget::UnlimitedNanoseconds, 4,
         false},
        {AssetLoaderPumpBudget::UnlimitedUnits, Step, 1, true},
        {AssetLoaderPumpBudget::UnlimitedUnits, Step + 1, 2, true},
        {AssetLoaderPumpBudget::UnlimitedUnits, 2 * Step, 2, true},
        {AssetLoaderPumpBudget::UnlimitedUnits, 2 * Step + 1, 3, true},
        {AssetLoaderPumpBudget::UnlimitedUnits, 3 * Step + 1, 4, false},
    };
    for (const Case& item : cases) {
        AssetEnv env;
        REQUIRE(env.Ready());
        LoaderRig rig(env);
        for (uint64 id = 1; id <= 4; ++id) {
            rig.scene.Add(id);
        }

        REQUIRE(rig.Cook());
        LoaderStepClock clock;
        clock.step = Step;
        rig.config.clock = LoaderStepClock::Read;
        rig.config.clockContext = &clock;
        REQUIRE(rig.Start());
        rig.io.hold = true;
        for (uint64 id = 1; id <= 4; ++id) {
            (void)rig.Request(id);
        }

        REQUIRE(rig.loader->Pump());
        rig.io.Complete();
        // IO 완료 → worker(바로 실행) 게시까지.
        auto drained = rig.loader->Pump();
        REQUIRE(drained && drained.Value().instantiatedUnits == 0);
        AssetLoaderPumpBudget budget;
        budget.maxUnits = item.maxUnits;
        budget.maxNanoseconds = item.maxNanoseconds;
        auto pumped = rig.loader->Pump(budget);
        REQUIRE(pumped);
        CHECK(pumped.Value().completions == 4);
        CHECK(pumped.Value().instantiatedUnits == item.expected);
        CHECK(pumped.Value().budgetExhausted == item.exhausted);
        AssetLoaderSnapshot snapshot(env.allocator);
        REQUIRE(Snapshot(rig, snapshot));
        CHECK(snapshot.loadedPackages == item.expected);
        if (item.exhausted) {
            CHECK(snapshot.units[0].waiting == AssetLoaderWait::Pump &&
                  snapshot.units[0].phase == AssetLoaderUnitPhase::Ready);
        }
    }
}

// 상태 관측(11.6절 12): 단계마다 단위·패키지 표가 지금 단계와 기다리는 것을 보이고(허가 → IO →
// worker → 의존 → Pump), 단계별 누적 시간이 정해진 시계로 쌓이며 임시 byte가 매니페스트 값과 같다.
TEST(AssetLoader_DescribeTracksStages) {
    AssetEnv env;
    REQUIRE(env.Ready());
    LoaderRig rig(env);
    rig.scene.Add(1);
    rig.scene.Add(2, 2048);
    rig.scene.Hard(1, 2);
    REQUIRE(rig.Cook());
    LoaderStepClock clock;
    clock.step = 1;
    rig.config.clock = LoaderStepClock::Read;
    rig.config.clockContext = &clock;
    REQUIRE(rig.Start());
    rig.io.hold = true;
    rig.worker.hold = true;
    const AssetLoadHandle handle = rig.Request(1, LoadPriority::High);
    AssetLoaderSnapshot snapshot(env.allocator);
    REQUIRE(Snapshot(rig, snapshot));
    REQUIRE(snapshot.requests.Size() == 1);
    CHECK(snapshot.requests[0].asset == FixedId(1) &&
          snapshot.requests[0].state == AssetLoadState::Pending &&
          snapshot.requests[0].priority == LoadPriority::High &&
          snapshot.requests[0].unit == UnitOf(snapshot, 1)->unit);
    CHECK(UnitOf(snapshot, 2)->waiting == AssetLoaderWait::Admission &&
          UnitOf(snapshot, 2)->effectivePriority == LoadPriority::High);
    CHECK(snapshot.packages.IsEmpty());

    REQUIRE(rig.loader->Pump());
    REQUIRE(Snapshot(rig, snapshot));
    CHECK(UnitOf(snapshot, 2)->phase == AssetLoaderUnitPhase::Loading &&
          UnitOf(snapshot, 2)->waiting == AssetLoaderWait::Io);
    const CookedManifestEntry& two = rig.scene.EntryOf(2);
    CHECK(UnitOf(snapshot, 2)->tempBytes == two.packageBytes + two.bulkRawBytes);
    REQUIRE(snapshot.packages.Size() == 2);
    CHECK(snapshot.packages[0].phase == AssetLoaderPackagePhase::Reading);

    rig.io.Complete();
    REQUIRE(rig.loader->Pump());
    REQUIRE(Snapshot(rig, snapshot));
    CHECK(UnitOf(snapshot, 2)->waiting == AssetLoaderWait::Worker);
    // worker 둘 중 1만 끝내면 1은 Prepared, 2를 기다린다.
    REQUIRE(rig.worker.pending.Size() == 2);
    const TestLoaderWorker::Pending second = rig.worker.pending[1];
    (void)rig.worker.pending.TryRemoveAt(1);
    second.job(second.context);
    REQUIRE(rig.loader->Pump());
    REQUIRE(Snapshot(rig, snapshot));
    CHECK(UnitOf(snapshot, 1)->preparedPackages == 1 &&
          UnitOf(snapshot, 1)->waiting == AssetLoaderWait::Dependency &&
          UnitOf(snapshot, 1)->waitingOn == FixedId(2));
    bool timed = false;
    for (const AssetLoaderPackageInfo& package : snapshot.packages) {
        if (package.asset == FixedId(1)) {
            CHECK(package.phase == AssetLoaderPackagePhase::Prepared);
            timed =
                package.phaseNanoseconds[static_cast<uint32>(AssetLoaderPackagePhase::Reading)] >
                    0 &&
                package.phaseNanoseconds[static_cast<uint32>(AssetLoaderPackagePhase::Preparing)] >
                    0;
        }
    }

    CHECK(timed);
    rig.worker.RunAll();
    REQUIRE(rig.PumpUntilSettled());
    CHECK(rig.StateOf(handle) == AssetLoadState::Succeeded);
    const AssetLoaderWait waits[] = {AssetLoaderWait::None,
                                     AssetLoaderWait::Admission,
                                     AssetLoaderWait::DependencyAdmission,
                                     AssetLoaderWait::Dependency,
                                     AssetLoaderWait::Io,
                                     AssetLoaderWait::Worker,
                                     AssetLoaderWait::Pump};
    for (AssetLoaderWait wait : waits) {
        CHECK(StringView(AssetLoaderWaitName(wait)) != StringView("Unknown"));
    }

    CHECK(StringView(AssetLoaderWaitName(static_cast<AssetLoaderWait>(99))) ==
          StringView("Unknown"));
    CHECK(StringView(LoadPriorityName(LoadPriority::Critical)) == StringView("Critical"));
    CHECK(StringView(LoadPriorityName(LoadPriority::Low)) == StringView("Low"));
    CHECK(StringView(LoadPriorityName(LoadPriority::Normal)) == StringView("Normal"));
    CHECK(StringView(LoadPriorityName(LoadPriority::High)) == StringView("High"));
    CHECK(StringView(LoadPriorityName(static_cast<LoadPriority>(9))) == StringView("Unknown"));
    const AssetLoadState states[] = {AssetLoadState::Pending, AssetLoadState::Succeeded,
                                     AssetLoadState::Failed, AssetLoadState::Cancelled};
    for (AssetLoadState state : states) {
        CHECK(StringView(AssetLoadStateName(state)) != StringView("Unknown"));
    }

    CHECK(StringView(AssetLoadStateName(static_cast<AssetLoadState>(9))) == StringView("Unknown"));
    const AssetLoaderUnitPhase phases[] = {
        AssetLoaderUnitPhase::Queued, AssetLoaderUnitPhase::Loading, AssetLoaderUnitPhase::Ready,
        AssetLoaderUnitPhase::Abandoned, AssetLoaderUnitPhase::Failed};
    for (AssetLoaderUnitPhase phase : phases) {
        CHECK(StringView(AssetLoaderUnitPhaseName(phase)) != StringView("Unknown"));
    }

    CHECK(StringView(AssetLoaderUnitPhaseName(static_cast<AssetLoaderUnitPhase>(9))) ==
          StringView("Unknown"));
    for (uint32 phase = 0; phase < AssetLoaderPackagePhaseCount; ++phase) {
        CHECK(StringView(AssetLoaderPackagePhaseName(
                  static_cast<AssetLoaderPackagePhase>(phase))) != StringView("Unknown"));
    }

    CHECK(StringView(AssetLoaderPackagePhaseName(AssetLoaderPackagePhase::Count)) ==
          StringView("Unknown"));
    CHECK(StringView(AssetLoaderPackagePhaseName(static_cast<AssetLoaderPackagePhase>(99))) ==
          StringView("Unknown"));
}

// 경로 색인(11.6절 2): caseless 키로 찾고(대소문자만 다른 경로도 같은 에셋), 없는 경로는 NotFound,
// 경로 규칙 위반은 InvalidArgument다.
TEST(AssetLoader_FindByPath) {
    AssetEnv env;
    REQUIRE(env.Ready());
    LoaderRig rig(env);
    for (uint64 id = 1; id <= 12; ++id) {
        rig.scene.Add(id);
    }

    REQUIRE(rig.Cook());
    REQUIRE(rig.Start());
    for (uint64 id = 1; id <= 12; ++id) {
        auto found = rig.loader->FindByPath(rig.scene.EntryOf(id).pathHint.View());
        CHECK(found && found.Value() == FixedId(id));
    }

    auto lower = rig.loader->FindByPath("/game/p7");
    CHECK(lower && lower.Value() == FixedId(7));
    CHECK(rig.loader->FindByPath("/Game/P13").Error() == ErrorCode::NotFound);
    CHECK(rig.loader->FindByPath("/Game/P0").Error() == ErrorCode::NotFound);
    CHECK(rig.loader->FindByPath("/Zzz/P1").Error() == ErrorCode::NotFound);
    CHECK(rig.loader->FindByPath("Game/P1").Error() == ErrorCode::InvalidArgument);
    CHECK(rig.loader->FindByPath("/Game").Error() == ErrorCode::InvalidArgument);
}

// 초기화·인자(11.6절 1·15): 설정 누락·0 한도·다른 TypeRegistry는 InvalidArgument, 프로파일이 다르면
// IncompatibleCook, 두 번째 Initialize는 InvalidArgument다. 초기화 전 호출은 InvalidArgument(조회는
// NotFound, Shutdown은 성공)이고, 매니페스트에 없는 id는 NotFound(MissingDependency), 범위 밖
// 우선순위는 InvalidArgument, 돌려준 handle은 StaleHandle이다.
TEST(AssetLoader_InitializeAndArguments) {
    AssetEnv env;
    REQUIRE(env.Ready());
    LoaderRig rig(env);
    rig.scene.Add(1);
    REQUIRE(rig.Cook());

    auto expectInvalid = [&](auto&& change) {
        AssetLoaderConfig config = rig.config;
        change(config);
        AssetLoader loader(config);
        AssetDiagnostic failure;
        return loader.Initialize(&failure).Error() == ErrorCode::InvalidArgument &&
               failure.error == ErrorCode::InvalidArgument;
    };
    CHECK(expectInvalid([](AssetLoaderConfig& config) { config.allocator = nullptr; }));
    CHECK(expectInvalid([](AssetLoaderConfig& config) { config.registry = nullptr; }));
    CHECK(expectInvalid([](AssetLoaderConfig& config) { config.types = nullptr; }));
    CHECK(expectInvalid([](AssetLoaderConfig& config) { config.manifest = nullptr; }));
    CHECK(expectInvalid([](AssetLoaderConfig& config) { config.io = nullptr; }));
    CHECK(expectInvalid([](AssetLoaderConfig& config) { config.worker = nullptr; }));
    CHECK(expectInvalid([](AssetLoaderConfig& config) { config.limits.maxPackagesInFlight = 0; }));
    CHECK(expectInvalid([](AssetLoaderConfig& config) { config.limits.maxTempBytes = 0; }));
    CHECK(expectInvalid([](AssetLoaderConfig& config) { config.limits.agingAdmissions = 0; }));
    TypeRegistry otherTypes(env.allocator);
    CHECK(expectInvalid([&](AssetLoaderConfig& config) { config.types = &otherTypes; }));
    {
        AssetLoaderConfig config = rig.config;
        config.io = nullptr;
        AssetLoader loader(config);
        CHECK(loader.Initialize().Error() == ErrorCode::InvalidArgument);
    }

    {
        AssetLoaderConfig config = rig.config;
        config.runtimeProfile = "Android";
        AssetLoader loader(config);
        AssetDiagnostic failure;
        CHECK(loader.Initialize(&failure).Error() == ErrorCode::Unsupported);
        CHECK(failure.code == AssetDiagnosticCode::IncompatibleCook &&
              failure.section == static_cast<uint32>(CookedManifestField::ProfileId));
    }

    // 로더 상태 할당이 실패하면 진단 out 인자 없이도 OutOfMemory다.
    {
        test::NFailAllocator failing(env.allocator);
        AssetLoaderConfig config = rig.config;
        config.allocator = &failing;
        AssetLoader loader(config);
        failing.FailOnAttempt(1);
        CHECK(loader.Initialize().Error() == ErrorCode::OutOfMemory);
        failing.DisableFailure();
    }

    AssetLoader idle(rig.config);
    const AssetLoadHandle none{};
    CHECK(idle.Request(LoaderRequestOf(1)).Error() == ErrorCode::InvalidArgument);
    CHECK(idle.FindByPath("/Game/P1").Error() == ErrorCode::InvalidArgument);
    CHECK(idle.Status(none).Error() == ErrorCode::InvalidArgument);
    CHECK(idle.SetPriority(none, LoadPriority::High).Error() == ErrorCode::InvalidArgument);
    CHECK(idle.Release(none).Error() == ErrorCode::InvalidArgument);
    CHECK(idle.Pump().Error() == ErrorCode::InvalidArgument);
    CHECK(idle.WaitForCompletions().Error() == ErrorCode::InvalidArgument);
    CHECK(idle.WaitFor(none, "test").Error() == ErrorCode::InvalidArgument);
    AssetLoaderSnapshot snapshot(env.allocator);
    CHECK(idle.Describe(snapshot).Error() == ErrorCode::InvalidArgument);
    CHECK(idle.FindPackage(FixedId(1)).Error() == ErrorCode::NotFound);
    CHECK(idle.Shutdown());

    REQUIRE(rig.Start());
    CHECK(rig.loader->Initialize().Error() == ErrorCode::InvalidArgument);
    AssetDiagnostic failure;
    CHECK(rig.loader->Request(LoaderRequestOf(77), &failure).Error() == ErrorCode::NotFound);
    CHECK(failure.code == AssetDiagnosticCode::MissingDependency && failure.asset == FixedId(77));
    AssetLoadRequest bad;
    bad.id = FixedId(1);
    bad.priority = static_cast<LoadPriority>(LoadPriorityCount);
    CHECK(rig.loader->Request(bad).Error() == ErrorCode::InvalidArgument);
    CHECK(rig.loader->FindPackage(FixedId(77)).Error() == ErrorCode::NotFound);
    CHECK(rig.loader->FindPackage(FixedId(1)).Error() == ErrorCode::NotFound);
    const AssetLoadHandle stale{5, 1};
    CHECK(rig.loader->Status(stale).Error() == ErrorCode::StaleHandle);
    CHECK(rig.loader->SetPriority(stale, LoadPriority::High).Error() == ErrorCode::StaleHandle);
    CHECK(rig.loader->Release(stale).Error() == ErrorCode::StaleHandle);
    CHECK(rig.loader->WaitFor(stale, "test").Error() == ErrorCode::StaleHandle);
    // 아무 일도 없으면 완료 대기는 바로 돌아온다.
    CHECK(rig.loader->WaitForCompletions());
}

namespace {

// 다른 스레드에서 부른 결과다.
struct ThreadCalls {
    static constexpr usize Count = 9;

    AssetLoader* loader = nullptr;
    Allocator* allocator = nullptr;
    AssetLoadHandle handle;
    ErrorCode results[Count] = {};
};

void CallFromOtherThread(void* context) noexcept {
    auto& calls = *static_cast<ThreadCalls*>(context);
    AssetLoader& loader = *calls.loader;
    AssetLoaderSnapshot snapshot(*calls.allocator);
    calls.results[0] = loader.Request(LoaderRequestOf(1)).Error();
    calls.results[1] = loader.FindByPath("/Game/P1").Error();
    calls.results[2] = loader.Status(calls.handle).Error();
    calls.results[3] = loader.SetPriority(calls.handle, LoadPriority::High).Error();
    calls.results[4] = loader.Release(calls.handle).Error();
    calls.results[5] = loader.Pump().Error();
    calls.results[6] = loader.WaitForCompletions().Error();
    calls.results[7] = loader.WaitFor(calls.handle, "thread").Error();
    calls.results[8] = loader.Describe(snapshot).Error();
}

} // namespace

// 스레드(11.6절 15): Initialize를 부른 스레드 밖에서 부른 공개 함수는 모두 WrongThread이고 상태를
// 바꾸지 않는다(Shutdown 포함).
TEST(AssetLoader_RejectsOtherThreads) {
    AssetEnv env;
    REQUIRE(env.Ready());
    LoaderRig rig(env);
    rig.scene.Add(1);
    REQUIRE(rig.Cook());
    REQUIRE(rig.Start());
    const AssetLoadHandle handle = rig.Request(1);
    ThreadCalls calls;
    calls.loader = rig.loader.Get();
    calls.allocator = &env.allocator;
    calls.handle = handle;
    platform::Thread thread;
    REQUIRE(thread.Start(CallFromOtherThread, &calls) == platform::ThreadStatus::Success);
    REQUIRE(thread.Join() == platform::ThreadStatus::Success);
    for (ErrorCode result : calls.results) {
        CHECK(result == ErrorCode::WrongThread);
    }

    struct ShutdownCall {
        AssetLoader* loader = nullptr;
        ErrorCode result = ErrorCode::None;
    } shutdown{rig.loader.Get()};
    platform::Thread other;
    REQUIRE(other.Start(
                [](void* context) noexcept {
                    auto& call = *static_cast<ShutdownCall*>(context);
                    call.result = call.loader->Shutdown().Error();
                },
                &shutdown) == platform::ThreadStatus::Success);
    REQUIRE(other.Join() == platform::ThreadStatus::Success);
    CHECK(shutdown.result == ErrorCode::WrongThread);
    REQUIRE(rig.PumpUntilSettled());
    CHECK(rig.StateOf(handle) == AssetLoadState::Succeeded);
}

} // namespace loader_tests_file
