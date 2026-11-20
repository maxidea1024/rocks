#include "LoaderTestSupport.h"

#include <AssetTestFixtures/TestAssets.generated.h>

// 로더 단계 경계 시험이다(Docs/Design/AssetSystem.ko.md 11.6절 A1c): 실패와 취소의 겹침(실패가
// 이긴다), 정리 중 재요청, Queued 단위의 의존 실패, 의존 단위를 직접 요청한 쪽만 취소, 단계별 합류,
// 공개된 의존, 비ASCII 경로 색인, 임시 byte 포화, 실패 단위의 상태 관측, 최근 동기 대기, 매니페스트
// 검증, PostLoad 단계(순서·예산·합류·취소·종료·PostLoad 중인 의존).
using namespace rocks;
namespace loader_stage_tests_file {

using namespace rocks::asset_test;

namespace {

const AssetLoaderUnitInfo* StageUnit(const AssetLoaderSnapshot& snapshot, uint64 id) noexcept {
    for (const AssetLoaderUnitInfo& unit : snapshot.units) {
        if (unit.asset == FixedId(id)) {
            return &unit;
        }
    }

    return nullptr;
}

usize StageBegunCount(LoaderRig& rig, uint64 id) noexcept {
    usize count = 0;
    for (const String& begun : rig.io.begun) {
        count += begun.View() == rig.scene.PathOf(id) ? 1 : 0;
    }

    return count;
}

CancelSource StageSource(AssetEnv& env) noexcept {
    auto made = CancelSource::Create(env.allocator);
    return made ? Move(made.Value()) : CancelSource();
}

bool StageDependencyFailed(const AssetLoadStatus& status, uint64 asset, uint64 related) noexcept {
    return status.state == AssetLoadState::Failed && status.error == ErrorCode::DependencyFailed &&
           status.diagnostic.code == AssetDiagnosticCode::MissingDependency &&
           status.diagnostic.asset == FixedId(asset) &&
           status.diagnostic.related == FixedId(related);
}

AssetLoaderSnapshot StageSnapshot(LoaderRig& rig) noexcept {
    AssetLoaderSnapshot snapshot(rig.env.allocator);
    (void)rig.loader->Describe(snapshot);
    return snapshot;
}

} // namespace

// T1 실패·취소 겹침(11.6절 7): 1 → 2에서 둘 다 worker에 넣고 2를 손상시킨 뒤 요청 1에 취소를
// 표시하고 worker를 모두 실행한 다음 Pump 한 번 — 같은 Pump에서 완료 처리(2 실패 → 1 실패)가 취소
// 확인보다 먼저라 요청 1은 Failed / DependencyFailed(related 2)이고 단위가 남지 않는다.
TEST(AssetLoader_FailureWinsOverCancelInOnePump) {
    AssetEnv env;
    REQUIRE(env.Ready());
    LoaderRig rig(env);
    rig.scene.Add(1);
    rig.scene.Add(2);
    rig.scene.Hard(1, 2);
    REQUIRE(rig.Cook());
    rig.scene.FileOf(2)[70] ^= 0x01;
    REQUIRE(rig.Start());
    rig.worker.hold = true;
    CancelSource source = StageSource(env);
    const AssetLoadHandle handle = rig.Request(1, LoadPriority::Normal, source.Token());
    REQUIRE(rig.loader->Pump());
    REQUIRE(rig.loader->Pump());
    REQUIRE(rig.worker.pending.Size() == 2);
    source.Cancel();
    rig.worker.RunAll();
    REQUIRE(rig.loader->Pump());
    CHECK(StageDependencyFailed(rig.StatusOf(handle), 1, 2));
    CHECK(StageSnapshot(rig).units.IsEmpty());
}

// T2 정리 중 재요청(11.6절 7): 1 → 2를 IO 중에 취소해 단위를 버린 뒤(옛 IO 남음) 곧바로 다시
// 요청하면 새 단위 둘이 생기고(버린 둘 + Queued 둘) 경로 1을 두 번 읽는다. 옛 완료를 먼저 내보내도
// 새 요청은 Succeeded이고 IO·임시 byte가 모두 돌아온다. 순환 4 ↔ 5에서 5의 Begin이 실패해 4의 IO가
// 남은 채 실패한 단위도, 오류를 풀고 다시 요청하면 성공한다.
TEST(AssetLoader_RequestAgainWhileRetiredUnitDrains) {
    AssetEnv env;
    REQUIRE(env.Ready());
    LoaderRig rig(env);
    for (uint64 id = 1; id <= 5; ++id) {
        rig.scene.Add(id);
    }

    rig.scene.Hard(1, 2);
    rig.scene.Hard(4, 5);
    rig.scene.Hard(5, 4);
    REQUIRE(rig.Cook());
    REQUIRE(rig.Start());
    rig.io.hold = true;
    CancelSource source = StageSource(env);
    (void)rig.Request(1, LoadPriority::Normal, source.Token());
    REQUIRE(rig.loader->Pump());
    REQUIRE(rig.io.held.Size() == 2);
    source.Cancel();
    REQUIRE(rig.loader->Pump());
    const AssetLoadHandle again = rig.Request(1);
    AssetLoaderSnapshot snapshot = StageSnapshot(rig);
    CHECK(snapshot.units.Size() == 4);
    REQUIRE(rig.loader->Pump());
    CHECK(StageBegunCount(rig, 1) == 2 && StageBegunCount(rig, 2) == 2);
    // 옛 완료 둘을 먼저 내보낸다.
    rig.io.Complete(2);
    REQUIRE(rig.loader->Pump());
    REQUIRE(rig.SettleHeld(false));
    CHECK(rig.StateOf(again) == AssetLoadState::Succeeded && rig.IsLoaded(1) && rig.IsLoaded(2));
    snapshot = StageSnapshot(rig);
    CHECK(rig.io.active == 0 && snapshot.packagesInFlight == 0 && snapshot.tempBytesInFlight == 0);

    TestPackageIo::File* five = rig.io.FindFile(rig.scene.PathOf(5));
    five->beginError = ErrorCode::Busy;
    const AssetLoadHandle failed = rig.Request(4);
    REQUIRE(rig.loader->Pump());
    CHECK(rig.StateOf(failed) == AssetLoadState::Failed);
    five->beginError = ErrorCode::None;
    const AssetLoadHandle retry = rig.Request(4);
    REQUIRE(rig.SettleHeld(false));
    CHECK(rig.StateOf(retry) == AssetLoadState::Succeeded && rig.IsLoaded(4) && rig.IsLoaded(5));
}

// T3 Queued 단위의 의존 실패(11.6절 6·9): 동시 진행 1에서 사슬 1 → 2 → 3의 3이 손상이면 1·2는 Begin
// 되지 않은 채 DependencyFailed(related 3)다.
TEST(AssetLoader_QueuedUnitsFailWithTheirDependency) {
    AssetEnv env;
    REQUIRE(env.Ready());
    LoaderRig rig(env);
    for (uint64 id = 1; id <= 3; ++id) {
        rig.scene.Add(id);
    }

    rig.scene.Hard(1, 2);
    rig.scene.Hard(2, 3);
    REQUIRE(rig.Cook());
    rig.scene.FileOf(3)[70] ^= 0x01;
    rig.config.limits.maxPackagesInFlight = 1;
    REQUIRE(rig.Start());
    const AssetLoadHandle one = rig.Request(1);
    const AssetLoadHandle two = rig.Request(2);
    REQUIRE(rig.PumpUntilSettled());
    CHECK(StageBegunCount(rig, 1) == 0 && StageBegunCount(rig, 2) == 0 &&
          StageBegunCount(rig, 3) == 1);
    CHECK(StageDependencyFailed(rig.StatusOf(one), 1, 3));
    CHECK(StageDependencyFailed(rig.StatusOf(two), 2, 3));
}

// T4 의존 단위 직접 요청 취소(11.6절 7): 요청 3과 요청 1(→ 3)에서 3의 요청만 취소하면 3은
// Cancelled, 1은 Succeeded이고 3은 1의 의존으로 공개된다.
TEST(AssetLoader_CancelDirectRequestOfSharedDependency) {
    AssetEnv env;
    REQUIRE(env.Ready());
    LoaderRig rig(env);
    rig.scene.Add(1);
    rig.scene.Add(3);
    rig.scene.Hard(1, 3);
    REQUIRE(rig.Cook());
    REQUIRE(rig.Start());
    CancelSource source = StageSource(env);
    const AssetLoadHandle three = rig.Request(3, LoadPriority::Normal, source.Token());
    const AssetLoadHandle one = rig.Request(1);
    source.Cancel();
    REQUIRE(rig.PumpUntilSettled());
    CHECK(rig.StateOf(three) == AssetLoadState::Cancelled);
    CHECK(rig.StateOf(one) == AssetLoadState::Succeeded && rig.IsLoaded(3));
}

// T5 단계별 합류(11.6절 7): 요청 1이 IO 중·worker 중·Pump 대기(Ready) 중일 때 낸 같은 요청은 같은
// 단위에 합류하고(단위 수 그대로, 관심 수 증가) 모두 같은 패키지 handle로 끝난다.
TEST(AssetLoader_JoinAtEachStage) {
    AssetEnv env;
    REQUIRE(env.Ready());
    LoaderRig rig(env);
    rig.scene.Add(1);
    rig.scene.Add(9);
    REQUIRE(rig.Cook());
    REQUIRE(rig.Start());
    rig.io.hold = true;
    rig.worker.hold = true;
    Array<AssetLoadHandle> handles(env.allocator);
    REQUIRE(handles.TryPushBack(rig.Request(1, LoadPriority::Low)));
    REQUIRE(handles.TryPushBack(rig.Request(9, LoadPriority::High)));
    REQUIRE(rig.loader->Pump());
    // IO 중.
    REQUIRE(handles.TryPushBack(rig.Request(1, LoadPriority::Low)));
    AssetLoaderSnapshot snapshot = StageSnapshot(rig);
    CHECK(snapshot.units.Size() == 2 && StageUnit(snapshot, 1)->interest == 2 &&
          StageUnit(snapshot, 1)->waiting == AssetLoaderWait::Io);
    rig.io.Complete();
    REQUIRE(rig.loader->Pump());
    // worker 중.
    REQUIRE(handles.TryPushBack(rig.Request(1, LoadPriority::Low)));
    snapshot = StageSnapshot(rig);
    CHECK(snapshot.units.Size() == 2 && StageUnit(snapshot, 1)->interest == 3 &&
          StageUnit(snapshot, 1)->waiting == AssetLoaderWait::Worker);
    rig.worker.RunAll();
    AssetLoaderPumpBudget one;
    one.maxUnits = 1;
    REQUIRE(rig.loader->Pump(one));
    // Ready(9가 먼저 공개되고 1은 Pump를 기다린다).
    REQUIRE(handles.TryPushBack(rig.Request(1, LoadPriority::Low)));
    snapshot = StageSnapshot(rig);
    CHECK(snapshot.units.Size() == 1 && StageUnit(snapshot, 1)->interest == 4 &&
          StageUnit(snapshot, 1)->waiting == AssetLoaderWait::Pump);
    REQUIRE(rig.loader->Pump());
    const ObjectHandle package = rig.loader->FindPackage(FixedId(1)).Value();
    for (usize index = 0; index < handles.Size(); ++index) {
        if (index == 1) {
            continue;
        }

        CHECK(rig.StateOf(handles[index]) == AssetLoadState::Succeeded &&
              rig.StatusOf(handles[index]).package == package);
    }
}

// T6 공개된 의존(11.6절 2): 2를 공개한 뒤 요청 1(→ 2)은 단위 하나(대기 이유 Admission — 의존 허가
// 대기가 아니다)이고 성공하며 Hard 참조가 공개된 2를 가리킨다.
TEST(AssetLoader_PublishedDependencyIsResolved) {
    AssetEnv env;
    REQUIRE(env.Ready());
    LoaderRig rig(env);
    rig.scene.Add(1);
    rig.scene.Add(2);
    rig.scene.Hard(1, 2);
    REQUIRE(rig.Cook());
    REQUIRE(rig.Start());
    (void)rig.Request(2);
    REQUIRE(rig.PumpUntilSettled());
    const AssetLoadHandle one = rig.Request(1);
    const AssetLoaderSnapshot snapshot = StageSnapshot(rig);
    REQUIRE(snapshot.units.Size() == 1);
    CHECK(snapshot.units[0].waiting == AssetLoaderWait::Admission);
    REQUIRE(rig.PumpUntilSettled());
    CHECK(rig.StateOf(one) == AssetLoadState::Succeeded);
    TestAsset* asset = Pointer<TestAsset>(*env.target, rig.AssetOf(1));
    REQUIRE(asset != nullptr && asset->hardList.Size() == 1);
    CHECK(asset->hardList[0] == rig.AssetOf(2));
}

// T9 비ASCII 경로 색인(11.6절 2): NFC 경로 "/Game/Pạ́"(U+1EA1 U+0301)를 대문자 Ạ(U+1EA0)·소문자
// 마운트로 찾아도 그 에셋이다.
TEST(AssetLoader_FindByPathNonAscii) {
    AssetEnv env;
    REQUIRE(env.Ready());
    LoaderRig rig(env);
    rig.scene.Add(1);
    rig.scene.Add(2);
    REQUIRE(rig.Cook());
    CookedManifestEntry& entry = rig.scene.EntryOf(2);
    entry.pathHint.Clear();
    REQUIRE(entry.pathHint.TryAppend("/Game/P\xE1\xBA\xA1\xCC\x81"));
    REQUIRE(rig.Start());
    auto found = rig.loader->FindByPath("/game/p\xE1\xBA\xA0\xCC\x81");
    CHECK(found && found.Value() == FixedId(2));
    auto exact = rig.loader->FindByPath("/Game/P\xE1\xBA\xA1\xCC\x81");
    CHECK(exact && exact.Value() == FixedId(2));
}

// T11 임시 byte 포화(11.6절 9): 항목 2의 bulkRawBytes가 u64 최댓값 − 파일 크기 + 1이라 합이 2⁶⁴면
// (감기면 0) 1이 진행 중인 동안 2는 허가 대기이고, 1이 끝나면 혼자 허가된다(진행 중 임시 byte는
// 최댓값으로 포화). 끝나면 임시 byte가 0으로 돌아온다.
TEST(AssetLoader_TempBytesSaturate) {
    AssetEnv env;
    REQUIRE(env.Ready());
    LoaderRig rig(env);
    rig.scene.Add(1);
    rig.scene.Add(2);
    REQUIRE(rig.Cook());
    CookedManifestEntry& two = rig.scene.EntryOf(2);
    two.bulkRawBytes = NumericLimits<uint64>::Max() - two.packageBytes + 1;
    constexpr uint64 Budget = uint64{1} << 20;
    rig.config.limits.maxTempBytes = Budget;
    REQUIRE(rig.Start());
    rig.io.hold = true;
    (void)rig.Request(1);
    REQUIRE(rig.loader->Pump());
    const AssetLoadHandle big = rig.Request(2);
    REQUIRE(rig.loader->Pump());
    AssetLoaderSnapshot snapshot = StageSnapshot(rig);
    CHECK(StageBegunCount(rig, 2) == 0 &&
          StageUnit(snapshot, 2)->waiting == AssetLoaderWait::Admission);
    rig.io.Complete();
    for (uint32 round = 0; round < 10 && StageBegunCount(rig, 2) == 0; ++round) {
        REQUIRE(rig.loader->Pump());
    }

    REQUIRE(StageBegunCount(rig, 2) == 1);
    snapshot = StageSnapshot(rig);
    CHECK(rig.IsLoaded(1) && snapshot.tempBytesInFlight == NumericLimits<uint64>::Max() &&
          StageUnit(snapshot, 2)->tempBytes == NumericLimits<uint64>::Max());
    rig.io.hold = false;
    rig.io.Complete();
    REQUIRE(rig.PumpUntilSettled());
    // 매니페스트 bulkRawBytes가 실제와 달라 내용 대조에서 실패한다.
    CHECK(rig.StateOf(big) == AssetLoadState::Failed);
    snapshot = StageSnapshot(rig);
    CHECK(snapshot.tempBytesInFlight == 0 && snapshot.packagesInFlight == 0);
}

// 실패 단위의 상태 관측(11.6절 12): 순환 4 ↔ 5에서 5의 Begin이 실패하고 4가 IO 중이면 상태 표의 그
// 단위는 Failed이고 실패 원인(패키지 5·Busy·진단 asset 5)을 보인다.
TEST(AssetLoader_DescribeShowsFailedUnitCause) {
    AssetEnv env;
    REQUIRE(env.Ready());
    LoaderRig rig(env);
    rig.scene.Add(4);
    rig.scene.Add(5);
    rig.scene.Hard(4, 5);
    rig.scene.Hard(5, 4);
    REQUIRE(rig.Cook());
    rig.io.FindFile(rig.scene.PathOf(5))->beginError = ErrorCode::Busy;
    REQUIRE(rig.Start());
    rig.io.hold = true;
    (void)rig.Request(4);
    REQUIRE(rig.loader->Pump());
    const AssetLoaderSnapshot snapshot = StageSnapshot(rig);
    const AssetLoaderUnitInfo* unit = StageUnit(snapshot, 4);
    REQUIRE(unit != nullptr);
    CHECK(unit->phase == AssetLoaderUnitPhase::Failed && unit->failedAsset == FixedId(5) &&
          unit->failError == ErrorCode::Busy && unit->failDiagnostic.asset == FixedId(5));
    rig.io.hold = false;
    rig.io.Complete();
    REQUIRE(rig.PumpUntilSettled());
}

// 최근 동기 대기(11.6절 12, 11.5절 3): WaitFor 10번 뒤 상태 표는 횟수 10과 최근 8개(오래된 것부터 —
// 셋째부터 열째)의 호출 지점·대상을 보인다.
TEST(AssetLoader_RecentSyncWaitsAreKept) {
    AssetEnv env;
    REQUIRE(env.Ready());
    LoaderRig rig(env);
    rig.scene.Add(1);
    REQUIRE(rig.Cook());
    REQUIRE(rig.Start());
    const AssetLoadHandle handle = rig.Request(1);
    static const char* const Sites[] = {"s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "s8", "s9"};
    for (const char* site : Sites) {
        auto waited = rig.loader->WaitFor(handle, site);
        REQUIRE(waited && waited.Value().state == AssetLoadState::Succeeded);
    }

    const AssetLoaderSnapshot snapshot = StageSnapshot(rig);
    REQUIRE(snapshot.syncWaits == 10 &&
            snapshot.recentSyncWaitCount == AssetLoaderSnapshot::RecentSyncWaitCapacity);
    for (uint32 index = 0; index < snapshot.recentSyncWaitCount; ++index) {
        CHECK(snapshot.recentSyncWaits[index].site == Sites[index + 2] &&
              snapshot.recentSyncWaits[index].asset == FixedId(1));
    }
}

// 매니페스트 검증(11.6절 1): 검증하지 않은 모델의 Hard 대상이 매니페스트에 없으면 Initialize가
// ValidateCookedManifest 진단(CorruptPackage, Dependency 칸)으로 거부한다.
TEST(AssetLoader_InitializeValidatesManifest) {
    AssetEnv env;
    REQUIRE(env.Ready());
    LoaderRig rig(env);
    rig.scene.Add(1);
    REQUIRE(rig.Cook());
    REQUIRE(rig.scene.EntryOf(1).dependencies.TryPushBack(
        CookedManifestDependency{FixedId(999), PackageDependencyKind::Hard}));
    AssetLoader loader(rig.config);
    AssetDiagnostic failure;
    CHECK(loader.Initialize(&failure).Error() == ErrorCode::InvalidArgument);
    CHECK(failure.code == AssetDiagnosticCode::CorruptPackage &&
          failure.section == static_cast<uint32>(CookedManifestField::Dependency) &&
          failure.related == FixedId(999));
}

// PostLoad 순서·예산(11.6절 16): 1 → 2·4, 2 ↔ 3. PostLoad는 의존 단위가 먼저(순환 단위 안은 AssetId
// 순)라 2·3·4·1 순이고, maxPostLoads 1이면 Pump마다 하나다(0이어도 하나 — 최소 진행). 요청 1은 자기
// PostLoad가 끝나야 Succeeded이고 그 전에는 단위가 PostLoading(의존 PostLoad 대기 → 자기 차례)이다.
TEST(AssetLoader_PostLoadOrderAndBudget) {
    AssetEnv env;
    REQUIRE(env.Ready());
    LoaderRig rig(env);
    for (uint64 id = 1; id <= 4; ++id) {
        rig.scene.Add(id);
    }

    rig.scene.Hard(1, 2);
    rig.scene.Hard(1, 4);
    rig.scene.Hard(2, 3);
    rig.scene.Hard(3, 2);
    REQUIRE(rig.Cook());
    RecordingPostLoad postLoad(env.allocator);
    rig.config.postLoad = &postLoad;
    REQUIRE(rig.Start());
    const AssetLoadHandle handle = rig.Request(1);
    AssetLoaderPumpBudget budget;
    budget.maxPostLoads = 0;
    bool sawDependencyWait = false;
    bool sawOwnWait = false;
    for (uint32 round = 0; round < 40 && rig.StateOf(handle) == AssetLoadState::Pending; ++round) {
        const usize before = postLoad.order.Size();
        auto pumped = rig.loader->Pump(budget);
        REQUIRE(pumped);
        CHECK(postLoad.order.Size() - before == pumped.Value().postLoads &&
              pumped.Value().postLoads <= 1);
        const AssetLoaderSnapshot snapshot = StageSnapshot(rig);
        const AssetLoaderUnitInfo* one = StageUnit(snapshot, 1);
        if (one != nullptr && one->phase == AssetLoaderUnitPhase::PostLoading) {
            sawDependencyWait =
                sawDependencyWait || one->waiting == AssetLoaderWait::PostLoadDependency;
            sawOwnWait = sawOwnWait || one->waiting == AssetLoaderWait::PostLoad;
            CHECK(rig.StateOf(handle) == AssetLoadState::Pending);
        }
    }

    CHECK(rig.StateOf(handle) == AssetLoadState::Succeeded);
    CHECK(sawDependencyWait && sawOwnWait);
    REQUIRE(postLoad.order.Size() == 4);
    const uint64 expected[] = {2, 3, 4, 1};
    for (usize index = 0; index < 4; ++index) {
        CHECK(postLoad.order[index] == FixedId(expected[index]));
    }
}

// PostLoad 합류·취소·종료(11.6절 16): maxPostLoads 0(Pump마다 하나)에서 1 → 2를 공개하면 같은
// Pump에서 2의 PostLoad만 불려 1은 PostLoading이다. 그때 낸 요청 1은 합류해 PostLoad 뒤
// Succeeded이고, 그 사이 취소한 첫 요청은 Cancelled지만 1의 PostLoad는 불린다. 독립 8·3을 함께
// 공개한 Pump는 8만 PostLoad해 3이 남고, 종료는 3의 PostLoad를 부르지 않으며 그 요청은
// ShuttingDown이다.
TEST(AssetLoader_PostLoadJoinCancelAndShutdown) {
    AssetEnv env;
    REQUIRE(env.Ready());
    LoaderRig rig(env);
    for (uint64 id = 1; id <= 8; ++id) {
        rig.scene.Add(id);
    }

    rig.scene.Hard(1, 2);
    rig.scene.Hard(5, 1);
    REQUIRE(rig.Cook());
    RecordingPostLoad postLoad(env.allocator);
    rig.config.postLoad = &postLoad;
    REQUIRE(rig.Start());
    CancelSource source = StageSource(env);
    const AssetLoadHandle one = rig.Request(1, LoadPriority::Normal, source.Token());
    AssetLoaderPumpBudget single;
    single.maxPostLoads = 0;
    for (uint32 round = 0; round < 3; ++round) {
        REQUIRE(rig.loader->Pump(single));
    }

    REQUIRE(rig.IsLoaded(1) && postLoad.order.Size() == 1 && postLoad.order[0] == FixedId(2));
    const AssetLoaderSnapshot snapshot = StageSnapshot(rig);
    REQUIRE(StageUnit(snapshot, 1) != nullptr &&
            StageUnit(snapshot, 1)->phase == AssetLoaderUnitPhase::PostLoading &&
            StageUnit(snapshot, 1)->waiting == AssetLoaderWait::PostLoad);
    const AssetLoadHandle joined = rig.Request(1);
    CHECK(rig.StateOf(joined) == AssetLoadState::Pending);
    // PostLoad 중인 1에 기대는 새 요청 5: 공개 대기는 없고(바로 허가 후보) PostLoad는 1 뒤다.
    const AssetLoadHandle five = rig.Request(5);
    {
        const AssetLoaderSnapshot fresh = StageSnapshot(rig);
        CHECK(StageUnit(fresh, 5) != nullptr &&
              StageUnit(fresh, 5)->waiting == AssetLoaderWait::Admission);
    }
    source.Cancel();
    REQUIRE(rig.loader->Pump(single));
    CHECK(rig.StateOf(one) == AssetLoadState::Cancelled);
    CHECK(rig.StateOf(joined) == AssetLoadState::Succeeded);
    REQUIRE(postLoad.order.Size() == 2 && postLoad.order[1] == FixedId(1));
    for (uint32 round = 0; round < 3; ++round) {
        REQUIRE(rig.loader->Pump(single));
    }

    REQUIRE(postLoad.order.Size() == 3 && postLoad.order[2] == FixedId(5));
    CHECK(rig.StateOf(five) == AssetLoadState::Succeeded);

    const AssetLoadHandle eight = rig.Request(8);
    const AssetLoadHandle three = rig.Request(3);
    for (uint32 round = 0; round < 3; ++round) {
        REQUIRE(rig.loader->Pump(single));
    }

    REQUIRE(postLoad.order.Size() == 4 && postLoad.order[3] == FixedId(8));
    CHECK(rig.StateOf(eight) == AssetLoadState::Succeeded &&
          rig.StateOf(three) == AssetLoadState::Pending && rig.IsLoaded(3));
    REQUIRE(rig.loader->Shutdown());
    CHECK(postLoad.order.Size() == 4);
    CHECK(rig.StatusOf(three).error == ErrorCode::ShuttingDown);
}

// PostLoad 시간 예산(11.6절 16): 단계마다 10 ns 가는 시계에서 PostLoad 준비 패키지 셋을
// maxNanoseconds 10으로 Pump하면 첫 호출 뒤 예산이 다해 하나만 부르고(소진 표시), 다음 Pump가
// 나머지를 부른다.
TEST(AssetLoader_PostLoadTimeBudget) {
    AssetEnv env;
    REQUIRE(env.Ready());
    LoaderRig rig(env);
    for (uint64 id = 1; id <= 3; ++id) {
        rig.scene.Add(id);
    }

    REQUIRE(rig.Cook());
    RecordingPostLoad postLoad(env.allocator);
    rig.config.postLoad = &postLoad;
    LoaderStepClock clock;
    constexpr uint64 Step = 10;
    clock.step = Step;
    rig.config.clock = LoaderStepClock::Read;
    rig.config.clockContext = &clock;
    REQUIRE(rig.Start());
    for (uint64 id = 1; id <= 3; ++id) {
        (void)rig.Request(id);
    }

    AssetLoaderPumpBudget single;
    single.maxPostLoads = 0;
    REQUIRE(rig.loader->Pump(single));
    REQUIRE(rig.loader->Pump(single));
    // 셋을 공개하고(시간 예산 무제한·단위 수 무제한) PostLoad는 하나만 한다.
    REQUIRE(rig.loader->Pump(single));
    REQUIRE(postLoad.order.Size() == 1);
    AssetLoaderPumpBudget timed;
    timed.maxNanoseconds = Step;
    auto pumped = rig.loader->Pump(timed);
    REQUIRE(pumped);
    CHECK(pumped.Value().postLoads == 1 && pumped.Value().budgetExhausted);
    pumped = rig.loader->Pump();
    REQUIRE(pumped);
    CHECK(pumped.Value().postLoads == 1 && !pumped.Value().budgetExhausted);
    CHECK(postLoad.order.Size() == 3);
}

// PostLoad 중인 의존(11.6절 16·12): 6 → 2·7에서 2가 공개된 채 PostLoad를 기다리고(앞선 Critical
// 10·11이 Pump마다 하나씩 PostLoad를 가져간다) 7은 아직 worker에 있으면, Prepared인 6의 대기 대상은
// PostLoad 중인 2가 아니라 공개 안 된 7이다. 6의 PostLoad는 2 뒤다.
TEST(AssetLoader_DependencyWaitSkipsPostLoadingUnits) {
    AssetEnv env;
    REQUIRE(env.Ready());
    LoaderRig rig(env);
    const uint64 ids[] = {2, 6, 7, 10, 11};
    for (uint64 id : ids) {
        rig.scene.Add(id);
    }

    rig.scene.Hard(6, 2);
    rig.scene.Hard(6, 7);
    REQUIRE(rig.Cook());
    RecordingPostLoad postLoad(env.allocator);
    rig.config.postLoad = &postLoad;
    REQUIRE(rig.Start());
    rig.worker.hold = true;
    (void)rig.Request(10, LoadPriority::Critical);
    (void)rig.Request(11, LoadPriority::Critical);
    const AssetLoadHandle six = rig.Request(6);
    AssetLoaderPumpBudget single;
    single.maxPostLoads = 0;
    REQUIRE(rig.loader->Pump(single));
    REQUIRE(rig.loader->Pump(single));
    // worker 작업은 허가 순서(10·11·2·7·6)다: 10·11·2만 끝낸다.
    REQUIRE(rig.worker.pending.Size() == 5);
    rig.worker.RunFirst();
    rig.worker.RunFirst();
    rig.worker.RunFirst();
    REQUIRE(rig.loader->Pump(single));
    REQUIRE(postLoad.order.Size() == 1 && postLoad.order[0] == FixedId(10));
    // 6을 끝낸다(7은 남는다).
    rig.worker.RunAt(1);
    REQUIRE(rig.loader->Pump(single));
    REQUIRE(postLoad.order.Size() == 2 && postLoad.order[1] == FixedId(11));
    const AssetLoaderSnapshot snapshot = StageSnapshot(rig);
    const AssetLoaderUnitInfo* unit = StageUnit(snapshot, 6);
    REQUIRE(unit != nullptr && StageUnit(snapshot, 2) != nullptr);
    CHECK(StageUnit(snapshot, 2)->phase == AssetLoaderUnitPhase::PostLoading);
    CHECK(unit->phase == AssetLoaderUnitPhase::Loading &&
          unit->waiting == AssetLoaderWait::Dependency && unit->waitingOn == FixedId(7));
    rig.worker.hold = false;
    rig.worker.RunAll();
    REQUIRE(rig.PumpUntilSettled());
    CHECK(rig.StateOf(six) == AssetLoadState::Succeeded);
    REQUIRE(postLoad.order.Size() == 5);
    CHECK(postLoad.order[2] == FixedId(2));
}

} // namespace loader_stage_tests_file
