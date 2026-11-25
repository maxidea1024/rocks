#include "LoaderTestSupport.h"

#include <AssetTestFixtures/TestAssets.generated.h>
#include <Platform/Thread.h>
#include <TestSupport/NFailAllocator.h>

// 로더 실패 경로 시험이다(Docs/Design/AssetSystem 11.6절 A1c): 단계마다 취소(허가 대기·IO·
// worker·의존 대기·Pump 대기·공개 뒤), 합류 요청 중 하나만 취소, 손상(크기·SHA-256·패키지 내용·bulk
// 상한·매니페스트 칸 불일치), Hard 실패 전파(사슬·SCC·다이아몬드, Soft 무영향, 다시 요청), 파일
// 없음·IO 오류·즉시 실패·worker 실패, 할당 실패 sweep, 종료.
using namespace rocks;
namespace loader_failure_tests_file {

using namespace rocks::asset_test;

namespace {

const AssetLoaderUnitInfo* FindUnit(const AssetLoaderSnapshot& snapshot, uint64 id) noexcept {
    for (const AssetLoaderUnitInfo& unit : snapshot.units) {
        if (unit.asset == FixedId(id)) {
            return &unit;
        }
    }

    return nullptr;
}

bool Begun(LoaderRig& rig, uint64 id) noexcept {
    const StringView path = rig.scene.PathOf(id);
    for (const String& begun : rig.io.begun) {
        if (begun.View() == path) {
            return true;
        }
    }

    return false;
}

usize UnitCount(LoaderRig& rig) noexcept {
    AssetLoaderSnapshot snapshot(rig.env.allocator);
    return rig.loader->Describe(snapshot) ? snapshot.units.Size() : NumericLimits<usize>::Max();
}

// 실패 진단이 DependencyFailed / MissingDependency(asset, related)인지다.
bool DependencyFailedOn(const AssetLoadStatus& status, uint64 asset, uint64 related) noexcept {
    return status.state == AssetLoadState::Failed && status.error == ErrorCode::DependencyFailed &&
           status.diagnostic.code == AssetDiagnosticCode::MissingDependency &&
           status.diagnostic.error == ErrorCode::DependencyFailed &&
           status.diagnostic.asset == FixedId(asset) &&
           status.diagnostic.related == FixedId(related);
}

bool CorruptOn(const AssetLoadStatus& status, uint64 asset, CookedManifestField field) noexcept {
    return status.state == AssetLoadState::Failed && status.error == ErrorCode::InvalidArgument &&
           status.diagnostic.code == AssetDiagnosticCode::CorruptPackage &&
           status.diagnostic.asset == FixedId(asset) &&
           status.diagnostic.section == static_cast<uint32>(field);
}

// 취소 표시 source를 만든다.
CancelSource MakeSource(AssetEnv& env) noexcept {
    auto made = CancelSource::Create(env.allocator);
    return made ? Move(made.Value()) : CancelSource();
}

} // namespace

// 단계마다 취소(11.6절 7): 1 → 2 장면에서 요청 1을 (a) 허가 대기 (b) IO 중 (c) worker 중 (d) 1은
// Prepared이고 2를 기다리는 중 (e) Pump 예산 대기 중에 취소하면 다음 Pump가 Cancelled로 끝내고
// 단위를 버린다(진행 중 IO·worker는 끝난 뒤 버리고, 버린 worker 작업은 Prepare하지 않는다). 버린 뒤
// 같은 에셋을 다시 요청하면 새로 로드해 성공한다. 공개 뒤 취소는 결과를 바꾸지 않는다.
TEST(AssetLoader_CancelAtEachStage) {
    enum class Stage : uint32 {
        Queued,
        Reading,
        Preparing,
        WaitingDependency,
        WaitingPump,
        AfterLoad,
        Count
    };
    for (uint32 value = 0; value < static_cast<uint32>(Stage::Count); ++value) {
        const auto stage = static_cast<Stage>(value);
        AssetEnv env;
        REQUIRE(env.Ready());
        LoaderRig rig(env);
        rig.scene.Add(1);
        rig.scene.Add(2, 1024);
        rig.scene.Add(3);
        rig.scene.Hard(1, 2);
        REQUIRE(rig.Cook());
        rig.config.limits.maxPackagesInFlight = 2;
        CountingAllocator counting(env.allocator);
        rig.config.allocator = &counting;
        REQUIRE(rig.Start());
        CancelSource source = MakeSource(env);
        REQUIRE(source.IsValid());
        AssetLoadHandle handle{};
        if (stage == Stage::Queued) {
            // 3이 진행 중인 채로 요청한다 — 다음 Pump는 허가보다 취소를 먼저 본다.
            rig.io.hold = true;
            (void)rig.Request(3);
            REQUIRE(rig.loader->Pump());
            handle = rig.Request(1, LoadPriority::Normal, source.Token());
            AssetLoaderSnapshot snapshot(env.allocator);
            REQUIRE(rig.loader->Describe(snapshot));
            CHECK(FindUnit(snapshot, 2) != nullptr &&
                  FindUnit(snapshot, 2)->phase == AssetLoaderUnitPhase::Queued);
        } else if (stage == Stage::Reading) {
            rig.io.hold = true;
            handle = rig.Request(1, LoadPriority::Normal, source.Token());
            REQUIRE(rig.loader->Pump());
            REQUIRE(rig.io.held.Size() == 2);
        } else if (stage == Stage::Preparing) {
            rig.worker.hold = true;
            handle = rig.Request(1, LoadPriority::Normal, source.Token());
            REQUIRE(rig.loader->Pump());
            REQUIRE(rig.loader->Pump());
            REQUIRE(rig.worker.pending.Size() == 2);
        } else if (stage == Stage::WaitingDependency) {
            rig.worker.hold = true;
            handle = rig.Request(1, LoadPriority::Normal, source.Token());
            REQUIRE(rig.loader->Pump());
            REQUIRE(rig.loader->Pump());
            REQUIRE(rig.worker.pending.Size() == 2);
            // 완료 순서는 허가 순서(2 → 1)다: 1의 작업만 끝낸다.
            const TestLoaderWorker::Pending second = rig.worker.pending[1];
            (void)rig.worker.pending.TryRemoveAt(1);
            second.job(second.context);
            REQUIRE(rig.loader->Pump());
        } else if (stage == Stage::WaitingPump) {
            rig.io.hold = true;
            (void)rig.Request(3);
            handle = rig.Request(1, LoadPriority::Low, source.Token());
            REQUIRE(rig.loader->Pump());
            rig.io.Complete();
            REQUIRE(rig.loader->Pump());
            AssetLoaderPumpBudget one;
            one.maxUnits = 1;
            // 3(Normal)과 2(Low 단위)가 준비됐다: 3을 먼저 공개하고 2는 Pump를 기다린다.
            auto pumped = rig.loader->Pump(one);
            REQUIRE(pumped && pumped.Value().budgetExhausted);
            REQUIRE(rig.IsLoaded(3) && !rig.IsLoaded(2));
        } else {
            handle = rig.Request(1, LoadPriority::Normal, source.Token());
            REQUIRE(rig.PumpUntilSettled());
            REQUIRE(rig.StateOf(handle) == AssetLoadState::Succeeded);
        }

        source.Cancel();
        REQUIRE(rig.loader->Pump());
        const AssetLoadStatus status = rig.StatusOf(handle);
        if (stage == Stage::AfterLoad) {
            CHECK(status.state == AssetLoadState::Succeeded);
            continue;
        }

        CHECK(status.state == AssetLoadState::Cancelled && status.error == ErrorCode::Cancelled &&
              status.diagnostic.asset == FixedId(1));
        CHECK(!rig.IsLoaded(1));
        // 진행 중 일을 끝내면 버린 단위가 사라진다. 버린 worker 작업은 Prepare 없이 끝난다 — 그
        // 동안 로더 allocator 할당이 없다.
        rig.io.hold = false;
        rig.io.Complete();
        rig.worker.hold = false;
        const uint64 before = counting.allocations;
        rig.worker.RunAll();
        if (stage == Stage::Preparing) {
            CHECK(counting.allocations == before);
        }
        REQUIRE(rig.PumpUntilSettled());
        CHECK(!rig.IsLoaded(1));
        if (stage == Stage::Queued) {
            CHECK(!Begun(rig, 1) && !Begun(rig, 2));
        }

        CHECK(rig.io.active == 0);
        AssetLoaderSnapshot snapshot(env.allocator);
        REQUIRE(rig.loader->Describe(snapshot));
        CHECK(snapshot.packagesInFlight == 0 && snapshot.tempBytesInFlight == 0);
        // 다시 요청하면 새로 로드한다.
        const AssetLoadHandle retry = rig.Request(1);
        REQUIRE(rig.PumpUntilSettled());
        CHECK(rig.StateOf(retry) == AssetLoadState::Succeeded && rig.IsLoaded(1) &&
              rig.IsLoaded(2));
    }
}

// 합류 취소(11.6절 7): 같은 에셋 요청 둘(다른 token)에서 하나만 취소하면 그것만 Cancelled이고 다른
// 요청은 Succeeded다. Release(Pending)는 취소와 같고 handle은 그 뒤 StaleHandle이다. 의존 단위를
// 공유하는 두 요청 중 하나를 취소해도 공유 의존은 계속 로드된다.
TEST(AssetLoader_CancelOneOfJoinedRequests) {
    AssetEnv env;
    REQUIRE(env.Ready());
    LoaderRig rig(env);
    rig.scene.Add(1);
    rig.scene.Add(2);
    rig.scene.Add(3);
    rig.scene.Hard(1, 3);
    rig.scene.Hard(2, 3);
    REQUIRE(rig.Cook());
    REQUIRE(rig.Start());
    rig.io.hold = true;
    CancelSource first = MakeSource(env);
    CancelSource second = MakeSource(env);
    const AssetLoadHandle a = rig.Request(1, LoadPriority::Normal, first.Token());
    const AssetLoadHandle b = rig.Request(1, LoadPriority::Normal, second.Token());
    const AssetLoadHandle c = rig.Request(2);
    const AssetLoadHandle d = rig.Request(2);
    REQUIRE(rig.loader->Pump());
    first.Cancel();
    REQUIRE(rig.loader->Pump());
    CHECK(rig.StateOf(a) == AssetLoadState::Cancelled);
    CHECK(rig.StateOf(b) == AssetLoadState::Pending);
    REQUIRE(rig.loader->Release(d));
    CHECK(rig.loader->Status(d).Error() == ErrorCode::StaleHandle);
    rig.io.hold = false;
    rig.io.Complete();
    REQUIRE(rig.PumpUntilSettled());
    CHECK(rig.StateOf(b) == AssetLoadState::Succeeded);
    CHECK(rig.StateOf(c) == AssetLoadState::Succeeded);
    CHECK(rig.IsLoaded(1) && rig.IsLoaded(2) && rig.IsLoaded(3));

    // 두 요청을 모두 놓으면 공유 의존까지 버린다.
    AssetEnv other;
    REQUIRE(other.Ready());
    LoaderRig dropped(other);
    dropped.scene.Add(1);
    dropped.scene.Add(2);
    dropped.scene.Add(3);
    dropped.scene.Hard(1, 3);
    dropped.scene.Hard(2, 3);
    REQUIRE(dropped.Cook());
    REQUIRE(dropped.Start());
    dropped.io.hold = true;
    const AssetLoadHandle x = dropped.Request(1);
    const AssetLoadHandle y = dropped.Request(2);
    REQUIRE(dropped.loader->Pump());
    REQUIRE(dropped.loader->Release(x));
    CHECK(UnitCount(dropped) == 3);
    REQUIRE(dropped.loader->Release(y));
    AssetLoaderSnapshot snapshot(other.allocator);
    REQUIRE(dropped.loader->Describe(snapshot));
    CHECK(snapshot.units.Size() == 3);
    for (const AssetLoaderUnitInfo& unit : snapshot.units) {
        CHECK(unit.phase == AssetLoaderUnitPhase::Abandoned);
    }

    dropped.io.Complete();
    REQUIRE(dropped.PumpUntilSettled());
    CHECK(UnitCount(dropped) == 0 && !dropped.IsLoaded(3) && dropped.io.active == 0);
}

namespace {

// 장면 하나(1 — bulk, 2 → 3)를 쿠킹하고 change로 IO 파일·매니페스트를 바꾼 뒤 id를 로드한다.
template <class Change>
AssetLoadStatus LoadChanged(uint64 id, Change&& change) noexcept {
    AssetEnv env;
    if (!env.Ready()) {
        return AssetLoadStatus{};
    }

    LoaderRig rig(env);
    rig.scene.Add(1, 3000);
    rig.scene.Add(2);
    rig.scene.Add(3);
    rig.scene.Hard(2, 3);
    if (!rig.Cook()) {
        return AssetLoadStatus{};
    }

    change(rig);
    if (!rig.Start()) {
        return AssetLoadStatus{};
    }

    const AssetLoadHandle handle = rig.Request(id);
    if (!rig.PumpUntilSettled()) {
        return AssetLoadStatus{};
    }

    return rig.StatusOf(handle);
}

// IO에 등록한 id 파일 byte를 바꾼다(span이 가리키는 장면 파일을 고친다).
void Patch(LoaderRig& rig, uint64 id, usize at, uint8 value) noexcept {
    rig.scene.FileOf(id)[at] = value;
}

} // namespace

// 손상(11.6절 3·4, AS 10.1절 8): SHA-256만 다름 → PackageSha256, 크기 다름(짧은 파일) →
// PackageBytes, 내용 손상(매니페스트 SHA를 맞춤) → 패키지 reader의 CorruptPackage, 매니페스트
// bulkRawBytes가 실제보다 작음 → maxBulkRawBytes 상한 CapacityExceeded, 큼 → BulkRawBytes,
// 매니페스트 의존·대표 타입이 다름 → PackageDependencies·TypeId.
TEST(AssetLoader_CorruptionIsDiagnosed) {
    CHECK(CorruptOn(LoadChanged(1, [](LoaderRig& rig) { Patch(rig, 1, 70, 0x5A); }), 1,
                    CookedManifestField::PackageSha256));
    CHECK(CorruptOn(LoadChanged(1,
                                [](LoaderRig& rig) {
                                    TestPackageIo::File* file =
                                        rig.io.FindFile(rig.scene.PathOf(1));
                                    file->bytes = file->bytes.Slice(0, file->bytes.Size() - 1);
                                }),
                    1, CookedManifestField::PackageBytes));
    {
        const AssetLoadStatus status = LoadChanged(1, [](LoaderRig& rig) {
            Array<uint8>& file = rig.scene.FileOf(1);
            file[file.Size() - 1] ^= 0x01;
            (void)rig.Republish(1);
        });
        CHECK(status.state == AssetLoadState::Failed &&
              status.diagnostic.code == AssetDiagnosticCode::CorruptPackage &&
              status.error == ErrorCode::InvalidArgument);
    }

    {
        const AssetLoadStatus status =
            LoadChanged(1, [](LoaderRig& rig) { rig.scene.EntryOf(1).bulkRawBytes -= 1; });
        CHECK(status.state == AssetLoadState::Failed &&
              status.error == ErrorCode::CapacityExceeded);
    }

    CHECK(CorruptOn(LoadChanged(1, [](LoaderRig& rig) { rig.scene.EntryOf(1).bulkRawBytes += 1; }),
                    1, CookedManifestField::BulkRawBytes));
    CHECK(CorruptOn(LoadChanged(1,
                                [](LoaderRig& rig) {
                                    (void)rig.scene.EntryOf(1).dependencies.TryPushBack(
                                        CookedManifestDependency{FixedId(77),
                                                                 PackageDependencyKind::Soft});
                                }),
                    1, CookedManifestField::PackageDependencies));
    CHECK(CorruptOn(
        LoadChanged(1, [](LoaderRig& rig) { rig.scene.EntryOf(1).type = TestPartType(); }), 1,
        CookedManifestField::TypeId));
}

// Hard 실패 전파(11.6절 6): 사슬 1 → 2 → 3에서 3이 손상이면 3 요청은 그 진단, 2·1 요청은
// DependencyFailed / MissingDependency(asset = 요청 에셋, related = 3)다. 순환 10 ↔ 11에서 11이
// 손상이면 10 요청도 실패(related 11)한다. Soft 대상(21)이 손상이어도 20은 성공하고 21은 읽지
// 않는다. 다이아몬드 30 → 31·32에서 32만 손상이면 31은 따로 성공한다. 파일을 고친 뒤 다시 요청하면
// 새로 로드해 성공한다.
TEST(AssetLoader_HardFailurePropagates) {
    AssetEnv env;
    REQUIRE(env.Ready());
    LoaderRig rig(env);
    const uint64 ids[] = {1, 2, 3, 10, 11, 20, 21, 30, 31, 32, 80, 81, 82};
    for (uint64 id : ids) {
        rig.scene.Add(id);
    }

    rig.scene.Hard(1, 2);
    rig.scene.Hard(2, 3);
    rig.scene.Hard(10, 11);
    rig.scene.Hard(11, 10);
    rig.scene.Soft(20, 21);
    rig.scene.Hard(30, 31);
    rig.scene.Hard(30, 32);
    rig.scene.Hard(81, 80);
    rig.scene.Hard(82, 80);
    rig.scene.Hard(82, 81);
    REQUIRE(rig.Cook());
    const uint64 corrupt[] = {3, 11, 21, 32, 80};
    for (uint64 id : corrupt) {
        rig.scene.FileOf(id)[70] ^= 0x01;
    }

    REQUIRE(rig.Start());
    const AssetLoadHandle one = rig.Request(1);
    const AssetLoadHandle two = rig.Request(2);
    const AssetLoadHandle three = rig.Request(3);
    const AssetLoadHandle ten = rig.Request(10);
    const AssetLoadHandle eleven = rig.Request(11);
    const AssetLoadHandle twenty = rig.Request(20);
    const AssetLoadHandle thirty = rig.Request(30);
    const AssetLoadHandle thirtyOne = rig.Request(31);
    // 80이 실패하면 81·82가 함께 실패 목록에 오르고, 81을 정리할 때 82에 다시 닿는다(원인 80 유지).
    const AssetLoadHandle eightyTwo = rig.Request(82);
    REQUIRE(rig.PumpUntilSettled());
    CHECK(CorruptOn(rig.StatusOf(three), 3, CookedManifestField::PackageSha256));
    CHECK(DependencyFailedOn(rig.StatusOf(two), 2, 3));
    CHECK(DependencyFailedOn(rig.StatusOf(one), 1, 3));
    CHECK(CorruptOn(rig.StatusOf(eleven), 11, CookedManifestField::PackageSha256));
    CHECK(DependencyFailedOn(rig.StatusOf(ten), 10, 11));
    CHECK(rig.StateOf(twenty) == AssetLoadState::Succeeded && !Begun(rig, 21));
    CHECK(DependencyFailedOn(rig.StatusOf(thirty), 30, 32));
    CHECK(rig.StateOf(thirtyOne) == AssetLoadState::Succeeded);
    CHECK(DependencyFailedOn(rig.StatusOf(eightyTwo), 82, 80));
    CHECK(!rig.IsLoaded(1) && !rig.IsLoaded(2) && !rig.IsLoaded(10) && !rig.IsLoaded(11) &&
          !rig.IsLoaded(30));
    CHECK(UnitCount(rig) == 0 && rig.io.active == 0);

    for (uint64 id : corrupt) {
        rig.scene.FileOf(id)[70] ^= 0x01;
    }

    const AssetLoadHandle again = rig.Request(1);
    const AssetLoadHandle cycle = rig.Request(10);
    REQUIRE(rig.PumpUntilSettled());
    CHECK(rig.StateOf(again) == AssetLoadState::Succeeded &&
          rig.StateOf(cycle) == AssetLoadState::Succeeded);
}

// IO·worker 오류(11.6절 4·5): 파일 없음 → NotFound / MissingFile, 완료 오류(Unsupported) →
// 진단 None과 그 오류, Begin 즉시 실패 → 그 오류(순환의 둘째 패키지에서 실패하면 첫째의 진행 중
// IO는 끝난 뒤 버린다), worker 제출 실패 → 그 오류다.
TEST(AssetLoader_IoAndWorkerErrors) {
    AssetEnv env;
    REQUIRE(env.Ready());
    LoaderRig rig(env);
    const uint64 ids[] = {1, 2, 3, 4, 5, 6};
    for (uint64 id : ids) {
        rig.scene.Add(id);
    }

    rig.scene.Hard(4, 5);
    rig.scene.Hard(5, 4);
    REQUIRE(rig.Cook());
    rig.io.FindFile(rig.scene.PathOf(1))->path.Clear();
    rig.io.FindFile(rig.scene.PathOf(2))->completeError = ErrorCode::Unsupported;
    rig.io.FindFile(rig.scene.PathOf(3))->beginError = ErrorCode::OutOfMemory;
    rig.io.FindFile(rig.scene.PathOf(5))->beginError = ErrorCode::Busy;
    REQUIRE(rig.Start());
    const AssetLoadHandle missing = rig.Request(1);
    const AssetLoadHandle failed = rig.Request(2);
    const AssetLoadHandle begin = rig.Request(3);
    rig.io.hold = true;
    const AssetLoadHandle cycle = rig.Request(4);
    REQUIRE(rig.loader->Pump());
    // 4는 IO 중이고 5의 Begin이 실패해 단위가 실패했다.
    CHECK(rig.io.held.Size() == 3);
    rig.io.hold = false;
    rig.io.Complete();
    REQUIRE(rig.PumpUntilSettled());
    const AssetLoadStatus notFound = rig.StatusOf(missing);
    CHECK(notFound.error == ErrorCode::NotFound &&
          notFound.diagnostic.code == AssetDiagnosticCode::MissingFile &&
          notFound.diagnostic.asset == FixedId(1));
    const AssetLoadStatus ioError = rig.StatusOf(failed);
    CHECK(ioError.error == ErrorCode::Unsupported &&
          ioError.diagnostic.code == AssetDiagnosticCode::None &&
          ioError.diagnostic.asset == FixedId(2));
    CHECK(rig.StatusOf(begin).error == ErrorCode::OutOfMemory);
    const AssetLoadStatus cycleStatus = rig.StatusOf(cycle);
    CHECK(DependencyFailedOn(cycleStatus, 4, 5));
    CHECK(UnitCount(rig) == 0 && rig.io.active == 0);

    rig.worker.failRun = ErrorCode::ShuttingDown;
    const AssetLoadHandle worker = rig.Request(6);
    REQUIRE(rig.PumpUntilSettled());
    const AssetLoadStatus workerStatus = rig.StatusOf(worker);
    CHECK(workerStatus.error == ErrorCode::ShuttingDown &&
          workerStatus.diagnostic.asset == FixedId(6));
}

// 공개 단계 실패(11.6절 3·6): 1(TestAsset — Hard → 2)을 쓸 때 2의 대표 에셋은 TestAsset이었는데
// 매니페스트의 2가 대표 에셋이 TestRequiredAsset인 다른 쿠킹 결과(→ 3)면, 2·3은 각자 매니페스트와
// 맞아 공개되고 1의 InstantiatePackages가 Hard 의존 기대 타입 불일치로 실패한다 — 1 요청은 그 진단
// (TypeMismatch, asset 1, related 2)이고 2·3은 공개된 채다.
TEST(AssetLoader_StaleDependencyTypeFailsInstantiate) {
    AssetEnv env;
    REQUIRE(env.Ready());
    AssetEnv other;
    REQUIRE(other.Ready());
    ObjectRegistry& source = *env.source;
    const ObjectAccessContext access = source.OwnerAccess();
    auto a = NewPackage(source, access, FixedId(1), PackageKind::Cooked);
    auto b = NewPackage(source, access, FixedId(2), PackageKind::Cooked);
    REQUIRE(a && b);
    const ObjectHandle aAsset = Make<TestAsset>(source, TestAssetType(), a.Value().Handle(), "");
    const ObjectHandle bAsset = Make<TestAsset>(source, TestAssetType(), b.Value().Handle(), "");
    REQUIRE(!aAsset.IsNull() && !bAsset.IsNull());
    REQUIRE(WithBorrow<TestAsset>(source, aAsset, [&](TestAsset& owner) {
        return source.AddReference(access, aAsset, owner.hardList, bAsset).IsOk();
    }));
    Array<uint8> aFile = Write(env.allocator, source, a.Value().Handle(), CookedOptions());
    REQUIRE(!aFile.IsEmpty());

    // 같은 AssetId 2를 다른 Registry에서 TestRequiredAsset(→ 3)으로 쓴다.
    ObjectRegistry& stale = *other.source;
    const ObjectAccessContext staleAccess = stale.OwnerAccess();
    auto b2 = NewPackage(stale, staleAccess, FixedId(2), PackageKind::Cooked);
    auto c = NewPackage(stale, staleAccess, FixedId(3), PackageKind::Cooked);
    REQUIRE(b2 && c);
    const ObjectHandle cAsset = Make<TestAsset>(stale, TestAssetType(), c.Value().Handle(), "");
    const ObjectHandle required =
        Make<TestRequiredAsset>(stale, TestRequiredType(), b2.Value().Handle(), "");
    REQUIRE(!cAsset.IsNull() && !required.IsNull());
    TestRequiredAsset* pointer = Pointer<TestRequiredAsset>(stale, required);
    REQUIRE(pointer != nullptr);
    REQUIRE(stale.SetReference(staleAccess, required, pointer->target, cAsset));
    Array<uint8> bFile = Write(env.allocator, stale, b2.Value().Handle(), CookedOptions());
    Array<uint8> cFile = Write(env.allocator, stale, c.Value().Handle(), CookedOptions());
    REQUIRE(!bFile.IsEmpty() && !cFile.IsEmpty());

    CookedManifest manifest(env.allocator);
    FillManifestHeader(manifest);
    Array<uint8>* files[] = {&aFile, &bFile, &cFile};
    TestPackageIo io(env.allocator);
    for (uint64 id = 1; id <= 3; ++id) {
        auto added = manifest.entries.TryEmplaceBack(env.allocator);
        REQUIRE(added && FillLoaderEntry(env.allocator, *added.Value(), *files[id - 1], id));
        StringBuilder<128> path(env.allocator);
        REQUIRE(LoaderPackagePath(path, "cooked", id, added.Value()->packageSha256));
        REQUIRE(io.Add(path.View(), files[id - 1]->View()));
    }

    REQUIRE(ValidateCookedManifest(manifest, env.allocator));
    TestLoaderWorker worker(env.allocator);
    AssetLoaderConfig config;
    config.allocator = &env.allocator;
    config.registry = env.target.Get();
    config.types = &env.types;
    config.manifest = &manifest;
    config.cookedRoot = "cooked";
    config.runtimeProfile = "Windows";
    config.io = &io;
    config.worker = &worker;
    AssetLoader loader(config);
    REQUIRE(loader.Initialize());
    auto handle = loader.Request(LoaderRequestOf(1));
    REQUIRE(handle);
    for (uint32 round = 0; round < 50; ++round) {
        REQUIRE(loader.Pump());
    }

    auto status = loader.Status(handle.Value());
    REQUIRE(status);
    CHECK(status.Value().state == AssetLoadState::Failed &&
          status.Value().error == ErrorCode::TypeMismatch &&
          status.Value().diagnostic.code == AssetDiagnosticCode::TypeMismatch &&
          status.Value().diagnostic.asset == FixedId(1) &&
          status.Value().diagnostic.related == FixedId(2));
    CHECK(loader.FindPackage(FixedId(2)) && loader.FindPackage(FixedId(3)) &&
          !loader.FindPackage(FixedId(1)));
}

// 할당 실패(11.6절 13): 로더 allocator의 모든 할당 지점에서 실패시켜도(Initialize·Request·합류·
// 허가·경로(긴 쿠킹 루트 — 경로 builder가 자란다)·완료 처리·Prepare·공개·상태 조회) 결과는 그
// 단계의 OutOfMemory이고(진단 None — 의존 단위가 할당 실패로 끝나도 OutOfMemory, 11.6절 6번), 같은
// 로더에서 실패를 끄고 다시 요청하면 성공한다(상태가 일관되다). 장면: 1 → 2(bulk)·3, 3 ↔ 4.
TEST(AssetLoader_AllocationFailures) {
    AssetEnv env;
    REQUIRE(env.Ready());
    test::NFailAllocator failing(env.allocator);
    uint32 failures = 0;
    uint32 requestFailures = 0;
    uint32 loadFailures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        UniquePtr<ObjectRegistry> registry = MakeLoaderRegistry(env, attempt, &failing);
        REQUIRE(registry.Get() != nullptr);
        LoaderRig rig(env);
        rig.scene.Add(1);
        rig.scene.Add(2, 2048);
        rig.scene.Add(3);
        rig.scene.Add(4);
        rig.scene.Hard(1, 2);
        rig.scene.Hard(1, 3);
        rig.scene.Hard(3, 4);
        rig.scene.Hard(4, 3);
        // 독립 요청 50 — 실패 하나가 요청 1 계열과 50을 함께 실패시키지 않는다(그 요청만 실패).
        rig.scene.Add(50);
        constexpr uint64 ChainBase = 100;
        constexpr uint32 ChainLength = 16;
        rig.scene.Hard(1, ChainBase);
        for (uint32 index = 0; index < ChainLength; ++index) {
            rig.scene.Add(ChainBase + index);
            if (index + 1 < ChainLength) {
                rig.scene.Hard(ChainBase + index, ChainBase + index + 1);
            }
        }

        rig.scene.root = "cooked/a-long-cooked-root-directory-name-that-does-not-fit-the-inline-"
                         "path-builder-capacity-of-the-loader-and-not-even-the-inline-capacity-of-"
                         "the-loader-root-builder-either-so-both-grow-from-the-allocator";
        REQUIRE(rig.Cook());
        // 비ASCII 경로(NFC quick check MAYBE)는 경로 색인 키 만들기에서도 할당한다.
        CookedManifestEntry& unicode = rig.scene.EntryOf(4);
        unicode.pathHint.Clear();
        REQUIRE(unicode.pathHint.TryAppend("/Game/P\xE1\xBA\xA1\xCC\x81"));
        rig.config.allocator = &failing;
        rig.config.registry = registry.Get();
        bool done = false;
        {
            AssetLoader loader(rig.config);
            failing.FailOnAttempt(attempt);
            AssetDiagnostic failure;
            // 진단 out 인자가 없어도 같은 오류다(짝수 시도).
            auto initialized = loader.Initialize(attempt % 2 == 0 ? nullptr : &failure);
            AssetLoadHandle handle{};
            bool requested = false;
            AssetLoadHandle fifty{};
            bool fiftyRequested = false;
            bool fiftyFailed = false;
            bool oneFailed = false;
            if (initialized) {
                AssetLoadRequest request;
                request.id = FixedId(1);
                auto made = loader.Request(request, &failure);
                if (made) {
                    handle = made.Value();
                    requested = true;
                    // 합류(같은 에셋·진행 중 순환의 구성 에셋)와 상태 조회도 실패 지점을 지난다.
                    AssetLoadRequest member;
                    member.id = FixedId(4);
                    const bool joined =
                        loader.Request(request).IsOk() && loader.Request(member).IsOk();
                    auto independent = loader.Request(LoaderRequestOf(50));
                    if (independent) {
                        fifty = independent.Value();
                        fiftyRequested = true;
                    } else {
                        fiftyFailed = true;
                    }
                    REQUIRE(loader.Pump());
                    AssetLoaderSnapshot snapshot(failing);
                    const bool described = loader.Describe(snapshot).IsOk();
                    (void)joined;
                    (void)described;
                    for (uint32 round = 0; round < 100; ++round) {
                        REQUIRE(loader.Pump());
                    }
                } else {
                    CHECK(made.Error() == ErrorCode::OutOfMemory &&
                          failure.code == AssetDiagnosticCode::None);
                    ++requestFailures;
                    oneFailed = true;
                }
            } else {
                CHECK(initialized.Error() == ErrorCode::OutOfMemory &&
                      failure.code == AssetDiagnosticCode::None);
            }

            const bool exhausted = failing.Attempts() < attempt;
            failing.DisableFailure();
            if (requested) {
                auto status = loader.Status(handle);
                REQUIRE(status);
                if (exhausted) {
                    CHECK(status.Value().state == AssetLoadState::Succeeded);
                    done = true;
                } else if (status.Value().state != AssetLoadState::Succeeded) {
                    oneFailed = true;
                    CHECK(status.Value().state == AssetLoadState::Failed);
                    CHECK(status.Value().error == ErrorCode::OutOfMemory &&
                          status.Value().diagnostic.code == AssetDiagnosticCode::None);
                    ++loadFailures;
                }
            }

            if (fiftyRequested) {
                auto status = loader.Status(fifty);
                REQUIRE(status);
                fiftyFailed = fiftyFailed || status.Value().state == AssetLoadState::Failed;
            }

            CHECK(!(oneFailed && fiftyFailed));
            if (initialized) {
                // 실패 뒤에도 같은 로더로 끝까지 로드한다.
                AssetLoadRequest request;
                request.id = FixedId(1);
                auto retry = loader.Request(request);
                REQUIRE(retry);
                for (uint32 round = 0; round < 100; ++round) {
                    REQUIRE(loader.Pump());
                }

                auto status = loader.Status(retry.Value());
                REQUIRE(status);
                CHECK(status.Value().state == AssetLoadState::Succeeded);
            }

            if (!exhausted) {
                ++failures;
            }

            CHECK(loader.Shutdown());
        }

        registry.Reset();
        if (done) {
            break;
        }
    }

    CHECK(failures > 10 && requestFailures > 0 && loadFailures > 0);
}

namespace {

struct ShutdownHelper {
    TestPackageIo* io = nullptr;
    platform::ManualResetEvent waiting;
};

// 완료 대기가 조건 변수에서 기다리기 직전에 불린다(11.6절 17).
void SignalWaiting(void* context) noexcept {
    static_cast<ShutdownHelper*>(context)->waiting.Signal();
}

} // namespace

// 종료(11.6절 14): Pending 요청은 ShuttingDown으로 끝나고(Status는 계속 읽힌다), 진행 중 IO가 끝날
// 때까지 기다린 뒤(다른 스레드가 Game 스레드의 대기 진입(11.6절 17 hook) 뒤에 완료를 내보낸다)
// 공개한 패키지를
// 놓는다(FindPackage NotFound). 그 뒤 Request·Pump·WaitFor는 ShuttingDown, 두 번째 Shutdown은
// 성공이다.
TEST(AssetLoader_ShutdownDrainsAndReleases) {
    AssetEnv env;
    REQUIRE(env.Ready());
    LoaderRig rig(env);
    for (uint64 id = 1; id <= 8; ++id) {
        rig.scene.Add(id);
    }

    rig.scene.Hard(3, 4);
    rig.scene.Hard(5, 6);
    rig.scene.Hard(6, 5);
    REQUIRE(rig.Cook());
    // 6의 Begin이 실패하면 순환 5·6 단위가 실패하고 5의 IO는 진행 중으로 남는다.
    rig.io.FindFile(rig.scene.PathOf(6))->beginError = ErrorCode::Busy;
    rig.config.limits.maxPackagesInFlight = 4;
    ShutdownHelper helper;
    helper.io = &rig.io;
    rig.config.waitEntered = SignalWaiting;
    rig.config.waitEnteredContext = &helper;
    REQUIRE(rig.Start());
    const AssetLoadHandle loaded = rig.Request(1);
    REQUIRE(rig.PumpUntilSettled());
    REQUIRE(rig.IsLoaded(1));
    rig.io.hold = true;
    const AssetLoadHandle pending = rig.Request(2);
    const AssetLoadHandle dropped = rig.Request(7);
    const AssetLoadHandle cycle = rig.Request(5);
    REQUIRE(rig.loader->Pump());
    // 2·7·5가 IO 중이고(5·6 단위는 실패해 5를 기다린다), 7을 놓아 그 단위는 버린 채 IO를 기다린다.
    REQUIRE(rig.io.held.Size() == 3);
    REQUIRE(rig.loader->Release(dropped));
    // 상한 4가 찼으므로 3 → 4 사슬은 허가 대기다.
    const AssetLoadHandle chain = rig.Request(3);
    CHECK(rig.StatusOf(cycle).state == AssetLoadState::Failed);
    // 완료 스레드는 Game 스레드가 실제로 기다리기 시작한 뒤(대기 진입 hook의 event)에 완료를
    // 내보낸다.
    platform::Thread thread;
    REQUIRE(thread.Start(
                [](void* context) noexcept {
                    auto& state = *static_cast<ShutdownHelper*>(context);
                    (void)state.waiting.Wait();
                    state.io->Complete();
                },
                &helper) == platform::ThreadStatus::Success);
    CHECK(rig.loader->Shutdown());
    REQUIRE(thread.Join() == platform::ThreadStatus::Success);
    const AssetLoadStatus status = rig.StatusOf(pending);
    CHECK(status.state == AssetLoadState::Failed && status.error == ErrorCode::ShuttingDown);
    CHECK(rig.StatusOf(chain).error == ErrorCode::ShuttingDown);
    CHECK(rig.StateOf(loaded) == AssetLoadState::Succeeded);
    CHECK(!rig.IsLoaded(1) && rig.io.active == 0);
    CHECK(rig.loader->Request(LoaderRequestOf(1)).Error() == ErrorCode::ShuttingDown);
    CHECK(rig.loader->Pump().Error() == ErrorCode::ShuttingDown);
    CHECK(rig.loader->WaitFor(loaded, "test").Error() == ErrorCode::ShuttingDown);
    CHECK(rig.loader->Shutdown());
    CHECK(rig.loader->Release(pending));
}

} // namespace loader_failure_tests_file
