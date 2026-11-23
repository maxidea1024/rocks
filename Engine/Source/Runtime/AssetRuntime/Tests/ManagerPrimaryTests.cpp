#include "ManagerTestSupport.h"

#include <AssetTestFixtures/TestAssets.generated.h>

// AssetManager PrimaryAsset·bundle 시험이다(Docs/Design/AssetSystem 10.1절 5·11.7절 10):
// bundle 구성원 함께 로드·caseless 이름·bundle 사이 중복 제거·lease 묶음, 오류(PrimaryAsset
// 아님·없는 bundle· 매니페스트에 없음·AlreadyRegistered·lane), 처음 실패, 먼저 놓은 묶음 티켓·scope
// Revoke, 용량 추정.
using namespace rocks;
namespace manager_primary_tests_file {

using namespace rocks::asset_test;

namespace {

constexpr usize CorruptOffset = 70;

// 1(PrimaryAsset)은 2를 Soft로, 3을 Hard로 가리킨다. bundle: Empty{}, Gameplay{2, 3}, Visual{2}.
bool CookPrimaryScene(ManagerRig& rig) noexcept {
    rig.scene.Add(1).soft = 2;
    rig.scene.Add(2);
    rig.scene.Add(3);
    rig.scene.Hard(1, 3);
    if (!rig.Cook()) {
        return false;
    }

    CookedManifestEntry& entry = rig.scene.EntryOf(1);
    entry.flags = static_cast<uint32>(CookedManifestFlags::PrimaryAsset);
    const uint64 gameplay[] = {2, 3};
    const uint64 visual[] = {2};
    return AddBundle(entry, "Empty") && AddBundle(entry, "Gameplay", gameplay) &&
           AddBundle(entry, "Visual", visual);
}

struct PrimaryRecord {
    explicit PrimaryRecord(Allocator& allocator) noexcept : assets(allocator), lease(allocator) {
    }

    uint32 calls = 0;
    ErrorCode error = ErrorCode::None;
    AssetDiagnostic diagnostic;
    Array<AssetId> assets;
    PrimaryAssetLease lease;
};

PrimaryReadyCallback RecordPrimary(Allocator& allocator, PrimaryRecord& record) noexcept {
    auto made = PrimaryReadyCallback::TryCreate(
        allocator,
        [&record](Result<PrimaryAssetLease> lease, const AssetDiagnostic& diagnostic) noexcept {
            ++record.calls;
            record.error = lease ? ErrorCode::None : lease.Error();
            record.diagnostic = diagnostic;
            if (lease) {
                for (const AssetLease& one : lease.Value().Leases()) {
                    (void)record.assets.TryPushBack(one.Asset());
                }

                record.lease = Move(lease.Value());
            }
        });
    return made ? Move(made.Value()) : PrimaryReadyCallback();
}

bool SameAssets(const Array<AssetId>& assets, Span<const uint64> expected) noexcept {
    if (assets.Size() != expected.Size()) {
        return false;
    }

    for (usize index = 0; index < assets.Size(); ++index) {
        if (!(assets[index] == FixedId(expected[index]))) {
            return false;
        }
    }

    return true;
}

} // namespace

// bundle 로드(11.7절 10): bundle 없이는 대표와 그 Hard 의존만, 지정 bundle(caseless 이름)이면
// 구성원도 로드하고 lease 묶음은 대표 → 구성원 AssetId 순이다. bundle 사이 중복은 한 번이다. 묶음
// lease와 티켓을 놓으면 모두 Discard된다.
TEST(AssetManager_LoadPrimaryAssetWithBundles) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ManagerRig rig(env);
    REQUIRE(CookPrimaryScene(rig));
    REQUIRE(rig.Start());
    PrimaryRecord bare(env.allocator);
    PrimaryRecord visual(env.allocator);
    PrimaryRecord both(env.allocator);
    {
        auto ticket = rig.manager->LoadPrimaryAsset(FixedId(1), {});
        REQUIRE(ticket);
        REQUIRE(rig.manager->OnReady(ticket.Value(), TaskLane::Game,
                                     RecordPrimary(env.allocator, bare)));
        REQUIRE(rig.Settle());
        const uint64 expected[] = {1};
        CHECK(bare.calls == 1 && bare.error == ErrorCode::None &&
              SameAssets(bare.assets, expected));
        CHECK(rig.IsLoaded(1) && rig.IsLoaded(3) && !rig.IsLoaded(2));
    }

    {
        const StringView names[] = {"visual"};
        auto ticket = rig.manager->LoadPrimaryAsset(FixedId(1), names);
        REQUIRE(ticket);
        REQUIRE(rig.manager->OnReady(ticket.Value(), TaskLane::Game,
                                     RecordPrimary(env.allocator, visual)));
        REQUIRE(rig.Settle());
        const uint64 expected[] = {1, 2};
        CHECK(visual.calls == 1 && SameAssets(visual.assets, expected));
        CHECK(rig.IsLoaded(2));
    }

    {
        const StringView names[] = {"Visual", "GAMEPLAY", "Empty"};
        auto ticket = rig.manager->LoadPrimaryAsset(FixedId(1), names, LoadPriority::High);
        REQUIRE(ticket);
        REQUIRE(rig.manager->OnReady(ticket.Value(), TaskLane::Game,
                                     RecordPrimary(env.allocator, both)));
        REQUIRE(rig.Settle());
        const uint64 expected[] = {1, 2, 3};
        CHECK(both.calls == 1 && SameAssets(both.assets, expected));
        CHECK(both.lease.Leases().Size() == 3 && both.lease.Leases()[2].IsValid());
    }

    bare.lease = PrimaryAssetLease(env.allocator);
    visual.lease = PrimaryAssetLease(env.allocator);
    both.lease = PrimaryAssetLease(env.allocator);
    REQUIRE(rig.Pump());
    REQUIRE(rig.Pump());
    CHECK(!rig.IsLoaded(1) && !rig.IsLoaded(2) && !rig.IsLoaded(3));
    CHECK(rig.UnitCount() == 0);
}

// 오류(11.7절 10): PrimaryAsset이 아닌 항목 InvalidArgument, 없는 bundle NotFound(진단 asset =
// 대표), 매니페스트에 없는 id NotFound, 두 번째 등록 AlreadyRegistered, Game이 아닌 lane
// Unsupported.
TEST(AssetManager_LoadPrimaryAssetErrors) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ManagerRig rig(env);
    REQUIRE(CookPrimaryScene(rig));
    REQUIRE(rig.Start());
    CHECK(rig.manager->LoadPrimaryAsset(FixedId(3), {}).Error() == ErrorCode::InvalidArgument);
    AssetDiagnostic failure;
    const StringView unknown[] = {"Audio"};
    CHECK(
        rig.manager->LoadPrimaryAsset(FixedId(1), unknown, LoadPriority::Normal, nullptr, &failure)
            .Error() == ErrorCode::NotFound);
    CHECK(failure.asset == FixedId(1));
    CHECK(rig.manager->LoadPrimaryAsset(FixedId(99), {}).Error() == ErrorCode::NotFound);
    PrimaryRecord record(env.allocator);
    PrimaryRecord spare(env.allocator);
    auto ticket = rig.manager->LoadPrimaryAsset(FixedId(1), {});
    REQUIRE(ticket);
    CHECK(
        rig.manager->OnReady(ticket.Value(), TaskLane::Worker, RecordPrimary(env.allocator, spare))
            .Error() == ErrorCode::Unsupported);
    REQUIRE(
        rig.manager->OnReady(ticket.Value(), TaskLane::Game, RecordPrimary(env.allocator, record)));
    CHECK(rig.manager->OnReady(ticket.Value(), TaskLane::Game, RecordPrimary(env.allocator, spare))
              .Error() == ErrorCode::AlreadyRegistered);
    REQUIRE(rig.Settle());
    CHECK(record.calls == 1 && spare.calls == 0);
}

// 처음 실패(11.7절 10 — 6.2절 Hard 규칙의 엄격함): 구성원 하나가 손상이면 callback은 그 진단으로 한
// 번 끝나고 lease를 주지 않는다(받은 구성원 관심은 티켓이 놓을 때까지만).
TEST(AssetManager_LoadPrimaryAssetFirstFailure) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ManagerRig rig(env);
    REQUIRE(CookPrimaryScene(rig));
    rig.scene.FileOf(2)[CorruptOffset] ^= 0x01;
    REQUIRE(rig.Start());
    PrimaryRecord record(env.allocator);
    {
        const StringView names[] = {"Visual"};
        auto ticket = rig.manager->LoadPrimaryAsset(FixedId(1), names);
        REQUIRE(ticket);
        REQUIRE(rig.manager->OnReady(ticket.Value(), TaskLane::Game,
                                     RecordPrimary(env.allocator, record)));
        REQUIRE(rig.Settle());
        CHECK(record.calls == 1 && record.error == ErrorCode::InvalidArgument);
        CHECK(record.diagnostic.code == AssetDiagnosticCode::CorruptPackage &&
              record.diagnostic.asset == FixedId(2));
        CHECK(record.lease.Leases().IsEmpty());
        CHECK(rig.IsLoaded(1));
    }

    REQUIRE(rig.Pump());
    REQUIRE(rig.Pump());
    CHECK(!rig.IsLoaded(1) && !rig.IsLoaded(3));
}

// 먼저 놓은 묶음 티켓과 scope Revoke(11.7절 7·10): 끝나기 전에 놓으면 Cancelled, scope Revoke면
// ScopeClosed를 한 번 받고 구성 요청은 모두 취소된다.
TEST(AssetManager_PrimaryTicketCancelAndRevoke) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ManagerRig rig(env);
    REQUIRE(CookPrimaryScene(rig));
    REQUIRE(rig.Start());
    auto scope = rig.manager->CreateScope();
    REQUIRE(scope);
    PrimaryRecord cancelled(env.allocator);
    PrimaryRecord closed(env.allocator);
    const StringView names[] = {"Gameplay"};
    {
        auto ticket = rig.manager->LoadPrimaryAsset(FixedId(1), names);
        REQUIRE(ticket);
        REQUIRE(rig.manager->OnReady(ticket.Value(), TaskLane::Game,
                                     RecordPrimary(env.allocator, cancelled)));
        REQUIRE(rig.Pump());
    }

    auto scoped =
        rig.manager->LoadPrimaryAsset(FixedId(1), names, LoadPriority::Normal, &scope.Value());
    REQUIRE(scoped);
    REQUIRE(
        rig.manager->OnReady(scoped.Value(), TaskLane::Game, RecordPrimary(env.allocator, closed)));
    REQUIRE(scope.Value().Revoke());
    REQUIRE(rig.Settle());
    CHECK(cancelled.calls == 1 && cancelled.error == ErrorCode::Cancelled);
    CHECK(closed.calls == 1 && closed.error == ErrorCode::ScopeClosed);
    CHECK(rig.manager->LoadPrimaryAsset(FixedId(1), names, LoadPriority::Normal, &scope.Value())
              .Error() == ErrorCode::ScopeClosed);
    CHECK(!rig.IsLoaded(1) && !rig.IsLoaded(2) && !rig.IsLoaded(3));
}

// 용량 추정(계획 §24.2 audit): 대표와 지정 bundle 구성원의 Hard 전이 폐포에 대한 매니페스트 byte
// 합이고 로드하지 않는다.
TEST(AssetManager_EstimatePrimaryAsset) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ManagerRig rig(env);
    REQUIRE(CookPrimaryScene(rig));
    REQUIRE(rig.Start());
    const CookedManifestEntry& one = rig.scene.EntryOf(1);
    const CookedManifestEntry& two = rig.scene.EntryOf(2);
    const CookedManifestEntry& three = rig.scene.EntryOf(3);
    auto bare = rig.manager->EstimatePrimaryAsset(FixedId(1), {});
    REQUIRE(bare);
    CHECK(bare.Value().packages == 2 &&
          bare.Value().packageBytes == one.packageBytes + three.packageBytes &&
          bare.Value().bulkRawBytes == one.bulkRawBytes + three.bulkRawBytes);
    const StringView names[] = {"Visual", "Gameplay"};
    auto all = rig.manager->EstimatePrimaryAsset(FixedId(1), names);
    REQUIRE(all);
    CHECK(all.Value().packages == 3 &&
          all.Value().packageBytes == one.packageBytes + two.packageBytes + three.packageBytes);
    CHECK(rig.manager->EstimatePrimaryAsset(FixedId(3), {}).Error() == ErrorCode::InvalidArgument);
    CHECK(!rig.IsLoaded(1));
}

} // namespace manager_primary_tests_file
