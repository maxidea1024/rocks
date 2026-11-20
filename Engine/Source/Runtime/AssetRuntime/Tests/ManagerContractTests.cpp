#include "ManagerTestSupport.h"

#include <AssetTestFixtures/TestAssets.generated.h>
#include <Platform/Thread.h>

// AssetManager API 계약 시험이다(Docs/Design/AssetSystem.ko.md 11.7절 1·5·6·7·12): 초기화 전
// InvalidArgument·잘못된 설정, 소유 스레드 밖 WrongThread, 무효 handle StaleHandle, callback 안
// WrongContext, 종료 뒤 ShuttingDown, 닫힌·무효 scope, handle 이동 대입, 로더 PostLoad 안의 동기
// 대기, ingest 전에 쌓인 간선의 순환 병합, 간선 없는 멤버를 가진 단위의 병합.
using namespace rocks;
namespace manager_contract_tests_file {

using namespace rocks::asset_test;

namespace {

constexpr usize CorruptOffset = 70;

// body를 다른 스레드에서 한 번 부르고 끝날 때까지 기다린다.
template <class Body>
bool RunOnOtherThread(Body& body) noexcept {
    platform::Thread thread;
    if (thread.Start([](void* context) noexcept { (*static_cast<Body*>(context))(); }, &body) !=
        platform::ThreadStatus::Success) {
        return false;
    }

    return thread.Join() == platform::ThreadStatus::Success;
}

DeferredRun NoRun(Allocator& allocator) noexcept {
    auto made = DeferredRun::TryCreate(allocator, [](const AssetLease&) noexcept {});
    return made ? Move(made.Value()) : DeferredRun();
}

DeferredFail NoFail(Allocator& allocator) noexcept {
    auto made =
        DeferredFail::TryCreate(allocator, [](ErrorCode, const AssetDiagnostic&) noexcept {});
    return made ? Move(made.Value()) : DeferredFail();
}

PrimaryReadyCallback NoPrimary(Allocator& allocator) noexcept {
    auto made = PrimaryReadyCallback::TryCreate(
        allocator, [](Result<PrimaryAssetLease>, const AssetDiagnostic&) noexcept {});
    return made ? Move(made.Value()) : PrimaryReadyCallback();
}

ErrorCode ErrorOf(const Result<void>& result) noexcept {
    return result ? ErrorCode::None : result.Error();
}

template <class T>
ErrorCode ErrorOf(const Result<T>& result) noexcept {
    return result ? ErrorCode::None : result.Error();
}

// 1(PrimaryAsset, bundle Visual{2})은 2를 Soft로 가리킨다.
bool CookPrimary(ManagerRig& rig) noexcept {
    rig.scene.Add(1).soft = 2;
    rig.scene.Add(2);
    if (!rig.Cook()) {
        return false;
    }

    CookedManifestEntry& entry = rig.scene.EntryOf(1);
    entry.flags = static_cast<uint32>(CookedManifestFlags::PrimaryAsset);
    const uint64 visual[] = {2};
    return AddBundle(entry, "Visual", visual);
}

// 로더 PostLoad 안에서 LoadBlocking·Pump를 부른다(로더 callback 안 — 로더가 WrongContext로
// 거절한다).
class BlockingPostLoad final : public AssetPostLoadHandler {
public:
    void PostLoad(ObjectHandle, AssetId asset) noexcept override {
        if (manager == nullptr || !(asset == FixedId(1))) {
            return;
        }

        AssetDiagnostic diagnostic;
        auto lease =
            manager->LoadBlocking(RequestOf(2), "ManagerContractTests.PostLoad", &diagnostic);
        error = lease ? ErrorCode::None : lease.Error();
        auto pumped = manager->Pump();
        pumpError = pumped ? ErrorCode::None : pumped.Error();
        ++calls;
    }

    AssetManager* manager = nullptr;
    ErrorCode error = ErrorCode::None;
    ErrorCode pumpError = ErrorCode::None;
    uint32 calls = 0;
};

} // namespace

// 초기화 전·잘못된 설정(11.7절 1): Initialize 전의 모든 API는 InvalidArgument(Shutdown은 무동작
// 성공)이고, 필수 설정이 빠지거나 Registry를 넘기거나 상한이 0이면 Initialize가 InvalidArgument와
// 진단을 준다. 두 번째 Initialize도 InvalidArgument다.
TEST(AssetManager_UninitializedAndBadConfig) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ManagerRig rig(env);
    REQUIRE(CookPrimary(rig));
    auto made = UniquePtr<AssetManager>::TryMake(env.allocator, rig.config);
    REQUIRE(made);
    AssetManager& idle = *made.Value();
    LoadTicket ticket;
    DeferredCommand command;
    PrimaryAssetTicket primary;
    AssetLease lease;
    AssetManagerSnapshot snapshot(env.allocator);
    CHECK(ErrorOf(idle.CreateScope()) == ErrorCode::InvalidArgument);
    CHECK(ErrorOf(idle.RequestAsset(RequestOf(1))) == ErrorCode::InvalidArgument);
    CHECK(ErrorOf(idle.OnReady(ticket, TaskLane::Game, AssetReadyCallback())) ==
          ErrorCode::InvalidArgument);
    CHECK(ErrorOf(idle.LoadBlocking(RequestOf(1), "Idle")) == ErrorCode::InvalidArgument);
    CHECK(ErrorOf(idle.RunWhenReady(FixedId(1), NotReadyPolicy::Defer, nullptr,
                                    NoRun(env.allocator), NoFail(env.allocator))) ==
          ErrorCode::InvalidArgument);
    CHECK(ErrorOf(idle.Cancel(command)) == ErrorCode::InvalidArgument);
    CHECK(ErrorOf(idle.With(lease, [](ObjectHandle) noexcept {})) == ErrorCode::InvalidArgument);
    CHECK(ErrorOf(idle.Pump()) == ErrorCode::InvalidArgument);
    CHECK(ErrorOf(idle.LoadPrimaryAsset(FixedId(1), {})) == ErrorCode::InvalidArgument);
    CHECK(ErrorOf(idle.OnReady(primary, TaskLane::Game, PrimaryReadyCallback())) ==
          ErrorCode::InvalidArgument);
    CHECK(ErrorOf(idle.EstimatePrimaryAsset(FixedId(1), {})) == ErrorCode::InvalidArgument);
    CHECK(ErrorOf(idle.Describe(snapshot)) == ErrorCode::InvalidArgument);
    CHECK(idle.Shutdown());

    // 잘못된 설정은 진단에도 남는다.
    constexpr uint32 BadConfigs = 6;
    for (uint32 bad = 0; bad < BadConfigs; ++bad) {
        AssetManagerConfig config = rig.config;
        switch (bad) {
        case 0:
            config.loader.allocator = nullptr;
            break;
        case 1:
            config.loader.types = nullptr;
            break;
        case 2:
            config.loader.manifest = nullptr;
            break;
        case 3:
            config.loader.registry = env.target.Get();
            break;
        case 4:
            config.observerMaxKeys = 0;
            break;
        default:
            config.maxDeferredCommands = 0;
            break;
        }

        auto broken = UniquePtr<AssetManager>::TryMake(env.allocator, config);
        REQUIRE(broken);
        AssetDiagnostic diagnostic;
        CHECK(ErrorOf(broken.Value()->Initialize(&diagnostic)) == ErrorCode::InvalidArgument);
        CHECK(diagnostic.error == ErrorCode::InvalidArgument);
        CHECK(ErrorOf(broken.Value()->Initialize()) == ErrorCode::InvalidArgument);
    }

    REQUIRE(idle.Initialize());
    CHECK(ErrorOf(idle.Initialize()) == ErrorCode::InvalidArgument);
    CHECK(idle.Shutdown());
}

// 소유 스레드 밖(11.7절 1·6): 모든 API가 WrongThread이고 상태는 바뀌지 않는다(scope Revoke 포함).
// 기본(무효) scope의 Revoke는 InvalidArgument다.
TEST(AssetManager_OffThreadCallsAreRejected) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ManagerRig rig(env);
    REQUIRE(CookPrimary(rig));
    REQUIRE(rig.Start());
    LoadTicket ticket = rig.Request(1);
    REQUIRE(rig.Settle());
    auto lease = rig.manager->LoadBlocking(RequestOf(1), "ManagerContractTests.Thread");
    REQUIRE(lease);
    auto command = rig.manager->RunWhenReady(FixedId(2), NotReadyPolicy::Fail, nullptr,
                                             NoRun(env.allocator), NoFail(env.allocator));
    CHECK(command.Error() == ErrorCode::NotReady);
    auto deferred = rig.manager->RunWhenReady(FixedId(2), NotReadyPolicy::Defer, nullptr,
                                              NoRun(env.allocator), NoFail(env.allocator));
    REQUIRE(deferred);
    auto primary = rig.manager->LoadPrimaryAsset(FixedId(1), {});
    REQUIRE(primary);
    auto scope = rig.manager->CreateScope();
    REQUIRE(scope);
    AssetScope empty;
    CHECK(ErrorOf(empty.Revoke()) == ErrorCode::InvalidArgument);

    constexpr usize Calls = 15;
    ErrorCode errors[Calls] = {};
    bool shutdownFailed = false;
    auto body = [&]() noexcept {
        AssetManager& manager = *rig.manager;
        AssetManagerSnapshot snapshot(env.allocator);
        usize at = 0;
        errors[at++] = ErrorOf(manager.CreateScope());
        errors[at++] = ErrorOf(manager.RequestAsset(RequestOf(1)));
        errors[at++] = ErrorOf(manager.OnReady(ticket, TaskLane::Game, AssetReadyCallback()));
        errors[at++] = ErrorOf(manager.LoadBlocking(RequestOf(1), "OffThread"));
        errors[at++] = ErrorOf(manager.RunWhenReady(FixedId(1), NotReadyPolicy::Defer, nullptr,
                                                    NoRun(env.allocator), NoFail(env.allocator)));
        errors[at++] = ErrorOf(manager.Cancel(deferred.Value()));
        errors[at++] = ErrorOf(manager.With(lease.Value(), [](ObjectHandle) noexcept {}));
        errors[at++] = ErrorOf(manager.Pump());
        errors[at++] = ErrorOf(manager.LoadPrimaryAsset(FixedId(1), {}));
        errors[at++] =
            ErrorOf(manager.OnReady(primary.Value(), TaskLane::Game, PrimaryReadyCallback()));
        errors[at++] = ErrorOf(manager.EstimatePrimaryAsset(FixedId(1), {}));
        errors[at++] = ErrorOf(manager.Describe(snapshot));
        errors[at++] = ErrorOf(scope.Value().Revoke());
        errors[at++] = ErrorOf(manager.Loader().PublishedHolds(FixedId(1)));
        errors[at++] = ErrorOf(manager.Loader().RetirePublished({}));
        shutdownFailed = manager.Shutdown().Error() == ErrorCode::WrongThread;
    };
    REQUIRE(RunOnOtherThread(body));
    for (usize index = 0; index < Calls; ++index) {
        CHECK(errors[index] == ErrorCode::WrongThread);
    }

    CHECK(shutdownFailed);
    CHECK(lease.Value().IsValid());
    CHECK(rig.Pump());
    CHECK(rig.UnitOf(1).info.tickets >= 1);
}

// 무효 handle·인자(11.7절 5): 기본 handle은 StaleHandle, 빈 묶음 callback은 InvalidArgument, 없는
// id는 NotFound(티켓 칸을 남기지 않는다), 준비 수준 0·무효 scope는 InvalidArgument, 잘못된 UTF-8
// bundle 이름은 NotFound다.
TEST(AssetManager_InvalidHandlesAndArguments) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ManagerRig rig(env);
    REQUIRE(CookPrimary(rig));
    REQUIRE(rig.Start());
    LoadTicket ticket;
    DeferredCommand command;
    PrimaryAssetTicket primary;
    AssetLease lease;
    CHECK(ErrorOf(rig.manager->OnReady(ticket, TaskLane::Game, AssetReadyCallback())) ==
          ErrorCode::StaleHandle);
    CHECK(ErrorOf(rig.manager->Cancel(command)) == ErrorCode::StaleHandle);
    CHECK(ErrorOf(rig.manager->OnReady(primary, TaskLane::Game, NoPrimary(env.allocator))) ==
          ErrorCode::StaleHandle);
    CHECK(ErrorOf(rig.manager->With(lease, [](ObjectHandle) noexcept {})) ==
          ErrorCode::StaleHandle);
    auto live = rig.manager->LoadPrimaryAsset(FixedId(1), {});
    REQUIRE(live);
    CHECK(ErrorOf(rig.manager->OnReady(live.Value(), TaskLane::Game, PrimaryReadyCallback())) ==
          ErrorCode::InvalidArgument);
    live.Value() = PrimaryAssetTicket();

    AssetDiagnostic diagnostic;
    CHECK(ErrorOf(rig.manager->RequestAsset(RequestOf(99), &diagnostic)) == ErrorCode::NotFound);
    AssetRequest none = RequestOf(1);
    none.required = 0;
    CHECK(ErrorOf(rig.manager->LoadBlocking(none, "None")) == ErrorCode::InvalidArgument);
    AssetScope invalid;
    AssetRequest scoped = RequestOf(1);
    scoped.scope = &invalid;
    CHECK(ErrorOf(rig.manager->RequestAsset(scoped)) == ErrorCode::InvalidArgument);
    CHECK(ErrorOf(rig.manager->LoadPrimaryAsset(FixedId(1), {}, LoadPriority::Normal, &invalid)) ==
          ErrorCode::InvalidArgument);
    CHECK(ErrorOf(rig.manager->RunWhenReady(FixedId(1), NotReadyPolicy::Defer, &invalid,
                                            NoRun(env.allocator), NoFail(env.allocator))) ==
          ErrorCode::InvalidArgument);
    // 잘못된 UTF-8 이름은 키를 만들 수 없어 없는 bundle과 같다.
    const char invalidUtf8[] = {static_cast<char>(0xFF), 0};
    const StringView malformed[] = {StringView(invalidUtf8, 1)};
    CHECK(ErrorOf(rig.manager->LoadPrimaryAsset(FixedId(1), malformed)) == ErrorCode::NotFound);
    CHECK(ErrorOf(rig.manager->EstimatePrimaryAsset(FixedId(1), malformed)) == ErrorCode::NotFound);

    // 닫힌 scope의 동기 로드는 ScopeClosed다.
    auto scope = rig.manager->CreateScope();
    REQUIRE(scope);
    REQUIRE(scope.Value().Revoke());
    AssetRequest closed = RequestOf(1);
    closed.scope = &scope.Value();
    CHECK(ErrorOf(rig.manager->LoadBlocking(closed, "Closed")) == ErrorCode::ScopeClosed);

    AssetManagerSnapshot snapshot(env.allocator);
    REQUIRE(rig.manager->Describe(snapshot));
    CHECK(snapshot.liveTickets == 0 && snapshot.liveLeases == 0);
}

// 동기 로드 실패 진단(11.7절 5): 손상 패키지의 LoadBlocking은 그 오류와 진단을 failure에 준다.
// 로더 PostLoad callback 안의 LoadBlocking·Pump는 로더가 WrongContext로 거절해 그 오류를 돌려준다.
TEST(AssetManager_LoadBlockingFailures) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ManagerRig rig(env);
    rig.scene.Add(1);
    rig.scene.Add(2);
    rig.scene.Add(3);
    REQUIRE(rig.Cook());
    rig.scene.FileOf(3)[CorruptOffset] ^= 0x01;
    BlockingPostLoad postLoad;
    rig.config.loader.postLoad = &postLoad;
    REQUIRE(rig.Start());
    postLoad.manager = rig.manager.Get();

    AssetDiagnostic diagnostic;
    auto broken = rig.manager->LoadBlocking(RequestOf(3), "Corrupt", &diagnostic);
    CHECK(broken.Error() == ErrorCode::InvalidArgument);
    CHECK(diagnostic.code == AssetDiagnosticCode::CorruptPackage && diagnostic.asset == FixedId(3));

    LoadTicket ticket = rig.Request(1);
    REQUIRE(rig.Settle());
    CHECK(postLoad.calls == 1 && postLoad.error == ErrorCode::WrongContext);
    CHECK(postLoad.pumpError == ErrorCode::WrongContext);
    CHECK(rig.IsLoaded(1));
    AssetManagerSnapshot snapshot(env.allocator);
    REQUIRE(rig.manager->Describe(snapshot));
    CHECK(snapshot.liveTickets == 1 && snapshot.liveLeases == 0);
}

// callback 안(11.7절 4·12): 준비 callback 안의 Pump·Shutdown은 WrongContext다.
TEST(AssetManager_CallbackContextRejectsPumpAndShutdown) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ManagerRig rig(env);
    rig.scene.Add(1);
    REQUIRE(rig.Cook());
    REQUIRE(rig.Start());
    ErrorCode pumped = ErrorCode::None;
    ErrorCode shutdown = ErrorCode::None;
    LoadTicket ticket = rig.Request(1);
    auto callback = AssetReadyCallback::TryCreate(
        env.allocator, [&](Result<AssetLease>, const AssetDiagnostic&) noexcept {
            pumped = ErrorOf(rig.manager->Pump());
            shutdown = ErrorOf(rig.manager->Shutdown());
        });
    REQUIRE(callback);
    REQUIRE(rig.manager->OnReady(ticket, TaskLane::Game, Move(callback.Value())));
    REQUIRE(rig.Settle());
    CHECK(pumped == ErrorCode::WrongContext && shutdown == ErrorCode::WrongContext);
    CHECK(rig.Pump());
}

// 종료 뒤(11.7절 12): 새 scope·요청·보류 명령·묶음 로드·Pump는 ShuttingDown이고 Shutdown은 다시
// 불러도 성공이다. scope Revoke로 회수된 티켓은 Shutdown이 다시 건드리지 않는다.
TEST(AssetManager_AfterShutdown) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ManagerRig rig(env);
    REQUIRE(CookPrimary(rig));
    REQUIRE(rig.Start());
    auto scope = rig.manager->CreateScope();
    REQUIRE(scope);
    ReadyRecord ready(env.allocator);
    LoadTicket ticket = rig.Request(1, &scope.Value());
    REQUIRE(rig.manager->OnReady(ticket, TaskLane::Game, RecordReady(env.allocator, ready)));
    REQUIRE(rig.Settle());
    REQUIRE(ready.calls == 1);
    REQUIRE(scope.Value().Revoke());
    CHECK(ErrorOf(scope.Value().Revoke()) == ErrorCode::None);
    CHECK(!ready.leases[0].IsValid());

    REQUIRE(rig.manager->Shutdown());
    CHECK(rig.manager->Shutdown());
    CHECK(ErrorOf(rig.manager->CreateScope()) == ErrorCode::ShuttingDown);
    CHECK(ErrorOf(rig.manager->RequestAsset(RequestOf(1))) == ErrorCode::ShuttingDown);
    CHECK(ErrorOf(rig.manager->LoadBlocking(RequestOf(1), "Down")) == ErrorCode::ShuttingDown);
    CHECK(ErrorOf(rig.manager->RunWhenReady(FixedId(1), NotReadyPolicy::Defer, nullptr,
                                            NoRun(env.allocator), NoFail(env.allocator))) ==
          ErrorCode::ShuttingDown);
    CHECK(ErrorOf(rig.manager->LoadPrimaryAsset(FixedId(1), {})) == ErrorCode::ShuttingDown);
    CHECK(ErrorOf(rig.manager->Pump()) == ErrorCode::ShuttingDown);
    CHECK(ErrorOf(scope.Value().Revoke()) == ErrorCode::None);
}

// handle 이동 대입(11.7절 6): 쥔 handle 위에 대입하면 옛 handle을 놓고(티켓·보류 명령·묶음 티켓은
// 해제 큐, scope는 Revoke) 원본은 무효가 된다. 자기 대입은 무동작이다. 보류 명령 handle을 먼저
// 놓아도 명령은 실행되고 그 뒤 칸이 회수된다.
TEST(AssetManager_HandleMoveAssignment) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ManagerRig rig(env);
    REQUIRE(CookPrimary(rig));
    REQUIRE(rig.Start());
    uint32 runs = 0;
    auto counting = [&]() noexcept {
        auto made =
            DeferredRun::TryCreate(env.allocator, [&runs](const AssetLease&) noexcept { ++runs; });
        return made ? Move(made.Value()) : DeferredRun();
    };
    auto first = rig.manager->RunWhenReady(FixedId(1), NotReadyPolicy::Defer, nullptr, counting(),
                                           NoFail(env.allocator));
    auto second = rig.manager->RunWhenReady(FixedId(2), NotReadyPolicy::Defer, nullptr, counting(),
                                            NoFail(env.allocator));
    REQUIRE(first && second);
    DeferredCommand held = Move(first.Value());
    CHECK(held.IsValid() && !first.Value().IsValid());
    held = Move(second.Value());
    CHECK(held.IsValid() && !second.Value().IsValid());
    DeferredCommand& alias = held;
    held = Move(alias);
    CHECK(held.IsValid());
    held = DeferredCommand();
    CHECK(!held.IsValid());

    auto scopeA = rig.manager->CreateScope();
    auto scopeB = rig.manager->CreateScope();
    REQUIRE(scopeA && scopeB);
    LoadTicket scoped = rig.Request(1, &scopeA.Value());
    AssetScope owner = Move(scopeA.Value());
    CHECK(owner.IsValid() && !scopeA.Value().IsValid());
    owner = Move(scopeB.Value());
    CHECK(owner.IsValid());
    AssetScope& self = owner;
    owner = Move(self);
    CHECK(owner.IsValid());

    auto primaryA = rig.manager->LoadPrimaryAsset(FixedId(1), {});
    auto primaryB = rig.manager->LoadPrimaryAsset(FixedId(1), {});
    REQUIRE(primaryA && primaryB);
    PrimaryAssetTicket bundle = Move(primaryA.Value());
    bundle = Move(primaryB.Value());
    CHECK(bundle.IsValid() && !primaryB.Value().IsValid());
    PrimaryAssetTicket& same = bundle;
    bundle = Move(same);
    CHECK(bundle.IsValid());

    REQUIRE(rig.Settle());
    CHECK(runs == 2);
    AssetManagerSnapshot snapshot(env.allocator);
    REQUIRE(rig.manager->Describe(snapshot));
    CHECK(snapshot.deferredCommands == 0);
    bundle = PrimaryAssetTicket();
    scoped = LoadTicket();
    owner = AssetScope();
    REQUIRE(rig.Pump());
    REQUIRE(rig.Pump());
    CHECK(rig.UnitCount() == 0);
}

// ingest 전에 쌓인 간선(11.7절 3·4): PostLoad 예산 1이면 순환 단위 {1, 2, 3}은 한 Pump에서
// Commit(간선 기록)되고 PostLoad를 Pump마다 하나씩 해 세 번째 Pump에서 넘어온다(PostLoad handler가
// 있어야 단계가 나뉜다). 그 사이 상주한 5에서 1로 간선이 생기면(1 → 5는 쿠킹 간선) 넘겨받을 때
// 들어오는 간선을 보유로 세고 5와의 순환을 찾아 한 단위로 합친다.
TEST(AssetManager_EdgesBeforeIngestMergeCycles) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ManagerRig rig(env);
    rig.scene.Add(1);
    rig.scene.Add(2);
    rig.scene.Add(3);
    rig.scene.Add(5);
    rig.scene.Hard(1, 2);
    rig.scene.Hard(2, 1);
    rig.scene.Hard(2, 3);
    rig.scene.Hard(3, 2);
    rig.scene.Hard(1, 5);
    REQUIRE(rig.Cook());
    RecordingPostLoad postLoad(env.allocator);
    rig.config.loader.postLoad = &postLoad;
    REQUIRE(rig.Start());
    LoadTicket five = rig.Request(5);
    REQUIRE(rig.Settle());
    AssetManagerPumpBudget slow;
    slow.loader.maxPostLoads = 1;
    LoadTicket one = rig.Request(1);
    bool committed = false;
    constexpr uint32 PumpLimit = 64;
    for (uint32 round = 0; round < PumpLimit && !committed; ++round) {
        rig.io.Complete();
        rig.worker.RunAll();
        REQUIRE(rig.Pump(slow));
        committed = rig.IsLoaded(1) && !rig.UnitOf(1).found;
    }

    REQUIRE(committed);
    // 두 번째 PostLoad의 Pump가 5 → 1 간선을 표에 넣는다(아직 넘겨받지 않았다 — 순환을 보지
    // 않는다).
    REQUIRE(rig.SetHard(5, 1));
    REQUIRE(rig.Pump(slow));
    CHECK(!rig.UnitOf(1).found && rig.UnitCount() == 1);
    // 아직 넘어오지 않은 1의 Commit 간선 1 → 5는 5의 보유다.
    CHECK(rig.UnitOf(5).info.inboundEdges == 1);
    // 세 번째 PostLoad 뒤 넘겨받을 때 쌓인 간선(들어오는 5 → 1, 나가는 1 → 5)으로 합친다.
    REQUIRE(rig.Pump(slow));
    REQUIRE(rig.UnitOf(1).found);
    const auto merged = rig.UnitOf(5);
    CHECK(merged.assets == 4 && merged.info.inboundEdges == 0 && merged.info.tickets == 2);
    CHECK(rig.UnitCount() == 1);
    five = LoadTicket();
    one = LoadTicket();
    REQUIRE(rig.Pump());
    CHECK(!rig.IsLoaded(1) && !rig.IsLoaded(2) && !rig.IsLoaded(3) && !rig.IsLoaded(5));
    CHECK(rig.UnitCount() == 0);
}

// 간선 없는 멤버(11.7절 3 — 병합은 나누지 않는다): 순환 단위 {1, 2}의 쿠킹 간선을 모두 지우면 두
// 패키지는 간선 칸이 없는 채 한 단위로 남는다. 그 뒤 1 ↔ 3 간선이 생기면 순환 탐색은 간선 칸 없는
// 2를 건너뛰며 셋을 합치고, 지웠던 간선 칸은 다시 쓴다.
TEST(AssetManager_MergeKeepsEdgelessMembers) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ManagerRig rig(env);
    rig.scene.Add(1);
    rig.scene.Add(2);
    rig.scene.Add(3);
    rig.scene.Hard(1, 2);
    rig.scene.Hard(2, 1);
    REQUIRE(rig.Cook());
    REQUIRE(rig.Start());
    {
        LoadTicket pair = rig.Request(1);
        LoadTicket third = rig.Request(3);
        REQUIRE(rig.Settle());
        REQUIRE(rig.UnitCount() == 2);
        REQUIRE(rig.ClearHardList(1));
        REQUIRE(rig.ClearHardList(2));
        REQUIRE(rig.Pump());
        CHECK(rig.UnitCount() == 2 && rig.UnitOf(2).assets == 2);
        REQUIRE(rig.SetHard(1, 3));
        REQUIRE(rig.Pump());
        CHECK(rig.UnitCount() == 2 && rig.UnitOf(3).info.inboundEdges == 1);
        REQUIRE(rig.SetHard(3, 1));
        REQUIRE(rig.Pump());
        CHECK(rig.UnitCount() == 1);
        const auto merged = rig.UnitOf(2);
        CHECK(merged.assets == 3 && merged.info.inboundEdges == 0 && merged.info.tickets == 2);
    }

    REQUIRE(rig.Pump());
    CHECK(rig.UnitCount() == 0);
    CHECK(!rig.IsLoaded(1) && !rig.IsLoaded(2) && !rig.IsLoaded(3));
}

// handle 경계(11.7절 5·6·7·9): 닫은 scope 칸은 다시 쓴다. 열린 scope의 동기 lease는 그 scope가
// 회수되면 With가 StaleHandle이다. 기본 lease는 무효(빈 id·handle)이고 자기 이동 대입은 무동작이다.
// 보류 명령은 run·fail이 비었거나 정책 값이 범위 밖이면 InvalidArgument, 끝난 명령 칸은 다시 쓰고,
// 두 번 Cancel·끝난 뒤 Cancel은 무동작 성공이다(fail은 한 번). callback 안이나 프레임 안의
// LoadBlocking은 WrongContext다.
TEST(AssetManager_HandleEdgeCases) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ManagerRig rig(env);
    rig.scene.Add(1);
    rig.scene.Add(2);
    REQUIRE(rig.Cook());
    REQUIRE(rig.Start());
    {
        auto closed = rig.manager->CreateScope();
        REQUIRE(closed);
        closed.Value() = AssetScope();
        auto reused = rig.manager->CreateScope();
        CHECK(reused && reused.Value().IsValid());
    }

    auto scope = rig.manager->CreateScope();
    REQUIRE(scope);
    AssetRequest scoped = RequestOf(1);
    scoped.scope = &scope.Value();
    auto lease = rig.manager->LoadBlocking(scoped, "ManagerContractTests.Scoped");
    REQUIRE(lease);
    CHECK(rig.manager->With(lease.Value(), [](ObjectHandle) noexcept {}));
    REQUIRE(scope.Value().Revoke());
    CHECK(ErrorOf(rig.manager->With(lease.Value(), [](ObjectHandle) noexcept {})) ==
          ErrorCode::StaleHandle);
    CHECK(!lease.Value().IsValid() && lease.Value().Asset() == AssetId{});

    AssetLease empty;
    CHECK(!empty.IsValid() && empty.Asset() == AssetId{});
    CHECK(empty.Object().IsNull() && empty.Package().IsNull());
    LoadTicket ticket = rig.Request(1);
    LoadTicket& ticketAlias = ticket;
    ticket = Move(ticketAlias);
    CHECK(ticket.IsValid());
    auto held = rig.manager->LoadBlocking(RequestOf(1), "ManagerContractTests.Self");
    REQUIRE(held);
    AssetLease& leaseAlias = held.Value();
    held.Value() = Move(leaseAlias);
    CHECK(held.Value().IsValid());

    CHECK(ErrorOf(rig.manager->RunWhenReady(FixedId(1), NotReadyPolicy::Defer, nullptr,
                                            DeferredRun(), NoFail(env.allocator))) ==
          ErrorCode::InvalidArgument);
    CHECK(ErrorOf(rig.manager->RunWhenReady(FixedId(1), NotReadyPolicy::Defer, nullptr,
                                            NoRun(env.allocator), DeferredFail())) ==
          ErrorCode::InvalidArgument);
    constexpr auto OutOfRangePolicy = static_cast<NotReadyPolicy>(3);
    CHECK(ErrorOf(rig.manager->RunWhenReady(FixedId(1), OutOfRangePolicy, nullptr,
                                            NoRun(env.allocator), NoFail(env.allocator))) ==
          ErrorCode::InvalidArgument);

    // 끝난 명령 칸은 다시 쓴다.
    {
        auto first = rig.manager->RunWhenReady(FixedId(1), NotReadyPolicy::Defer, nullptr,
                                               NoRun(env.allocator), NoFail(env.allocator));
        REQUIRE(first);
        REQUIRE(rig.Pump());
    }

    REQUIRE(rig.Pump());
    uint32 fails = 0;
    ErrorCode failed = ErrorCode::None;
    auto counting = DeferredFail::TryCreate(env.allocator,
                                            [&](ErrorCode error, const AssetDiagnostic&) noexcept {
                                                ++fails;
                                                failed = error;
                                            });
    REQUIRE(counting);
    auto command = rig.manager->RunWhenReady(FixedId(2), NotReadyPolicy::Defer, nullptr,
                                             NoRun(env.allocator), Move(counting.Value()));
    REQUIRE(command);
    REQUIRE(rig.manager->Cancel(command.Value()));
    REQUIRE(rig.manager->Cancel(command.Value()));
    REQUIRE(rig.Pump());
    CHECK(fails == 1 && failed == ErrorCode::Cancelled);
    REQUIRE(rig.manager->Cancel(command.Value()));
    REQUIRE(rig.Pump());
    CHECK(fails == 1);

    // callback 안의 LoadBlocking.
    ErrorCode inside = ErrorCode::None;
    LoadTicket other = rig.Request(1);
    auto callback = AssetReadyCallback::TryCreate(
        env.allocator, [&](Result<AssetLease>, const AssetDiagnostic&) noexcept {
            inside = ErrorOf(rig.manager->LoadBlocking(RequestOf(1), "ManagerContractTests.Cb"));
        });
    REQUIRE(callback);
    REQUIRE(rig.manager->OnReady(other, TaskLane::Game, Move(callback.Value())));
    REQUIRE(rig.Pump());
    CHECK(inside == ErrorCode::WrongContext);
}

// 프레임 안(11.5절 3·11.7절 5): inFrame이 참이면 LoadBlocking은 WrongContext다.
TEST(AssetManager_LoadBlockingInFrame) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ManagerRig rig(env);
    rig.scene.Add(1);
    REQUIRE(rig.Cook());
    rig.config.loader.inFrame = [](void*) noexcept { return true; };
    REQUIRE(rig.Start());
    CHECK(ErrorOf(rig.manager->LoadBlocking(RequestOf(1), "Frame")) == ErrorCode::WrongContext);
}

// 종료 뒤 scope(11.7절 7·12): 회수하지 않은 scope는 Shutdown 뒤 Revoke·소멸이 다시 회수하지
// 않는다(모두 Shutdown이 회수했다).
TEST(AssetManager_ScopeOutlivesShutdown) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ManagerRig rig(env);
    rig.scene.Add(1);
    REQUIRE(rig.Cook());
    REQUIRE(rig.Start());
    auto scope = rig.manager->CreateScope();
    REQUIRE(scope);
    AssetRequest scoped = RequestOf(1);
    scoped.scope = &scope.Value();
    auto lease = rig.manager->LoadBlocking(scoped, "ManagerContractTests.Outlive");
    REQUIRE(lease);
    REQUIRE(rig.manager->Shutdown());
    CHECK(!lease.Value().IsValid());
    CHECK(scope.Value().Revoke());
    scope.Value() = AssetScope();
    CHECK(!scope.Value().IsValid());
}

// 묶음 티켓을 놓는 순서(11.7절 7·10): (가) scope 회수로 끝난 묶음은 callback이 남아 있으면 티켓을
// 놓아도 다음 Pump에 ScopeClosed를 한 번 받는다. (나) 이미 끝났지만 callback을 아직 받지 않은
// 묶음의 티켓을 놓으면 Cancelled를 받고, 그 Pump의 티켓 callback 안에서 scope를 회수해도(놓은
// 묶음은 칸이 없다) 다시 끝나지 않는다.
TEST(AssetManager_PrimaryReleaseOrderings) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ManagerRig rig(env);
    REQUIRE(CookPrimary(rig));
    REQUIRE(rig.Start());
    ErrorCode revokedError = ErrorCode::None;
    uint32 revokedCalls = 0;
    {
        auto scope = rig.manager->CreateScope();
        REQUIRE(scope);
        auto primary =
            rig.manager->LoadPrimaryAsset(FixedId(1), {}, LoadPriority::Normal, &scope.Value());
        REQUIRE(primary);
        auto callback = PrimaryReadyCallback::TryCreate(
            env.allocator, [&](Result<PrimaryAssetLease> lease, const AssetDiagnostic&) noexcept {
                ++revokedCalls;
                revokedError = lease ? ErrorCode::None : lease.Error();
            });
        REQUIRE(callback);
        REQUIRE(rig.manager->OnReady(primary.Value(), TaskLane::Game, Move(callback.Value())));
        REQUIRE(scope.Value().Revoke());
        primary.Value() = PrimaryAssetTicket();
        REQUIRE(rig.Settle());
    }

    CHECK(revokedCalls == 1 && revokedError == ErrorCode::ScopeClosed);

    LoadTicket resident = rig.Request(1);
    REQUIRE(rig.Settle());
    auto scope = rig.manager->CreateScope();
    REQUIRE(scope);
    auto primary =
        rig.manager->LoadPrimaryAsset(FixedId(1), {}, LoadPriority::Normal, &scope.Value());
    REQUIRE(primary);
    ErrorCode droppedError = ErrorCode::None;
    uint32 droppedCalls = 0;
    auto primaryCallback = PrimaryReadyCallback::TryCreate(
        env.allocator, [&](Result<PrimaryAssetLease> lease, const AssetDiagnostic&) noexcept {
            ++droppedCalls;
            droppedError = lease ? ErrorCode::None : lease.Error();
        });
    REQUIRE(primaryCallback);
    REQUIRE(rig.manager->OnReady(primary.Value(), TaskLane::Game, Move(primaryCallback.Value())));
    LoadTicket trigger = rig.Request(1);
    auto revoke = AssetReadyCallback::TryCreate(
        env.allocator, [&scope](Result<AssetLease>, const AssetDiagnostic&) noexcept {
            (void)scope.Value().Revoke();
        });
    REQUIRE(revoke);
    REQUIRE(rig.manager->OnReady(trigger, TaskLane::Game, Move(revoke.Value())));
    primary.Value() = PrimaryAssetTicket();
    REQUIRE(rig.Pump());
    CHECK(droppedCalls == 1 && droppedError == ErrorCode::Cancelled);
    REQUIRE(rig.Pump());
    CHECK(droppedCalls == 1);
}

// 놓인 lease와 scope 회수(11.7절 6·7): lease 객체를 소멸해 해제 큐에 넣은 뒤 Pump 전에 scope를
// 회수하면 회수가 보유를 놓고, 다음 Pump의 해제 큐 처리는 회수된 lease의 보유를 다시 빼지 않는다.
TEST(AssetManager_RevokeSkipsReleasedLease) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ManagerRig rig(env);
    rig.scene.Add(1);
    REQUIRE(rig.Cook());
    REQUIRE(rig.Start());
    LoadTicket keep = rig.Request(1);
    REQUIRE(rig.Settle());
    auto scope = rig.manager->CreateScope();
    REQUIRE(scope);
    AssetRequest scoped = RequestOf(1);
    scoped.scope = &scope.Value();
    {
        auto lease = rig.manager->LoadBlocking(scoped, "ManagerContractTests.Released");
        REQUIRE(lease);
        REQUIRE(rig.Pump());
        CHECK(rig.UnitOf(1).info.leases == 1);
    }

    REQUIRE(scope.Value().Revoke());
    REQUIRE(rig.Pump());
    const auto unit = rig.UnitOf(1);
    REQUIRE(unit.found);
    CHECK(unit.info.leases == 0 && unit.info.tickets == 1);
    keep = LoadTicket();
    REQUIRE(rig.Pump());
    CHECK(rig.UnitCount() == 0);
}

} // namespace manager_contract_tests_file
