#include "ManagerTestSupport.h"

#include <AssetTestFixtures/TestAssets.generated.h>
#include <Platform/Thread.h>

// AssetManager handle 계약 시험이다(Docs/Design/AssetSystem 11.1·11.7절 5–9·12): 준비
// callback (정확히 한 번·AlreadyRegistered·lane·늦은 등록·먼저 놓은 티켓의 Cancelled·실패 진단),
// scope Revoke, LoadBlocking과 프레임 판정, 기대 타입·준비 수준, 미준비 대상 정책, 다른 스레드의
// lease 해제, 종료.
using namespace rocks;
namespace manager_handle_tests_file {

using namespace rocks::asset_test;

namespace {

// 손상 위치(패키지 헤더 뒤 — SHA-256이 달라진다)다.
constexpr usize CorruptOffset = 70;

struct DeferredRecord {
    uint32 runs = 0;
    uint32 fails = 0;
    ErrorCode error = ErrorCode::None;
    AssetDiagnostic diagnostic;
    AssetId ranAsset{};
};

DeferredRun RecordRun(Allocator& allocator, DeferredRecord& record) noexcept {
    auto made = DeferredRun::TryCreate(allocator, [&record](const AssetLease& lease) noexcept {
        ++record.runs;
        record.ranAsset = lease.Asset();
    });
    return made ? Move(made.Value()) : DeferredRun();
}

DeferredFail RecordFail(Allocator& allocator, DeferredRecord& record) noexcept {
    auto made = DeferredFail::TryCreate(
        allocator, [&record](ErrorCode error, const AssetDiagnostic& diagnostic) noexcept {
            ++record.fails;
            record.error = error;
            record.diagnostic = diagnostic;
        });
    return made ? Move(made.Value()) : DeferredFail();
}

bool FrameFlag(void* context) noexcept {
    return *static_cast<bool*>(context);
}

} // namespace

// callback 계약(11.7절 5): 끝난 뒤 Pump에서 정확히 한 번, 두 번째 등록은
// AlreadyRegistered(callback은 caller에 남는다), Game이 아닌 lane은 Unsupported, 빈 callback은
// InvalidArgument, 끝난 티켓에 늦게 등록하면 등록 함수 안이 아니라 다음 Pump에서 부른다.
TEST(AssetManager_ReadyCallbackContract) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ManagerRig rig(env);
    rig.scene.Add(1);
    rig.scene.Add(2);
    REQUIRE(rig.Cook());
    REQUIRE(rig.Start());
    ReadyRecord first(env.allocator);
    ReadyRecord late(env.allocator);
    ReadyRecord spare(env.allocator);
    LoadTicket ticket = rig.Request(1);
    REQUIRE(rig.manager->OnReady(ticket, TaskLane::Game, RecordReady(env.allocator, first)));
    AssetReadyCallback second = RecordReady(env.allocator, spare);
    CHECK(rig.manager->OnReady(ticket, TaskLane::Game, Move(second)).Error() ==
          ErrorCode::AlreadyRegistered);
    CHECK(static_cast<bool>(second));
    LoadTicket other = rig.Request(2);
    AssetReadyCallback worker = RecordReady(env.allocator, spare);
    CHECK(rig.manager->OnReady(other, TaskLane::Worker, Move(worker)).Error() ==
          ErrorCode::Unsupported);
    CHECK(
        rig.manager->OnReady(other, TaskLane::Render, RecordReady(env.allocator, spare)).Error() ==
        ErrorCode::Unsupported);
    CHECK(rig.manager->OnReady(other, TaskLane::Game, AssetReadyCallback()).Error() ==
          ErrorCode::InvalidArgument);
    REQUIRE(rig.Settle());
    CHECK(first.calls == 1 && first.error == ErrorCode::None && first.leases.Size() == 1);

    // 늦은 등록.
    REQUIRE(rig.manager->OnReady(other, TaskLane::Game, RecordReady(env.allocator, late)));
    CHECK(late.calls == 0);
    REQUIRE(rig.Pump());
    CHECK(late.calls == 1 && late.leases.Size() == 1);
    REQUIRE(rig.Pump());
    CHECK(first.calls == 1 && late.calls == 1 && spare.calls == 0);
    CHECK(rig.manager->OnReady(other, TaskLane::Game, RecordReady(env.allocator, spare)).Error() ==
          ErrorCode::AlreadyRegistered);
}

// 먼저 놓은 티켓(11.7절 5): 끝나기 전에 티켓을 놓으면 로더 요청이 취소되고 등록된 callback은
// Cancelled를 한 번 받는다. 로드 실패는 오류와 로더 진단을 callback에 준다.
TEST(AssetManager_DroppedTicketCancelsAndFailureCarriesDiagnostic) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ManagerRig rig(env);
    rig.scene.Add(1);
    rig.scene.Add(2);
    REQUIRE(rig.Cook());
    rig.scene.FileOf(2)[CorruptOffset] ^= 0x01;
    REQUIRE(rig.Start());
    ReadyRecord cancelled(env.allocator);
    ReadyRecord failed(env.allocator);
    {
        LoadTicket ticket = rig.Request(1);
        REQUIRE(
            rig.manager->OnReady(ticket, TaskLane::Game, RecordReady(env.allocator, cancelled)));
        REQUIRE(rig.Pump());
    }

    REQUIRE(rig.Pump());
    CHECK(cancelled.calls == 1 && cancelled.error == ErrorCode::Cancelled);
    LoadTicket corrupt = rig.Request(2);
    REQUIRE(rig.manager->OnReady(corrupt, TaskLane::Game, RecordReady(env.allocator, failed)));
    REQUIRE(rig.Settle());
    CHECK(!rig.IsLoaded(1));
    CHECK(failed.calls == 1 && failed.error == ErrorCode::CorruptData);
    CHECK(failed.diagnostic.code == AssetDiagnosticCode::CorruptPackage &&
          failed.diagnostic.asset == FixedId(2));
    CHECK(cancelled.calls == 1);
}

// scope Revoke(11.7절 7): 그 scope의 진행 중 callback은 ScopeClosed를 한 번 받고, lease는 회수되며
// (IsValid false — 소멸은 무동작), 새 요청은 ScopeClosed다. 다른 scope의 같은 에셋 lease는
// 그대로다. scope의 소멸도 Revoke다.
TEST(AssetManager_ScopeRevokeReclaims) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ManagerRig rig(env);
    rig.scene.Add(1);
    rig.scene.Add(2);
    REQUIRE(rig.Cook());
    REQUIRE(rig.Start());
    auto revoked = rig.manager->CreateScope();
    auto kept = rig.manager->CreateScope();
    REQUIRE(revoked && kept);
    ReadyRecord inScope(env.allocator);
    ReadyRecord outScope(env.allocator);
    ReadyRecord pending(env.allocator);
    LoadTicket mine = rig.Request(1, &revoked.Value());
    LoadTicket theirs = rig.Request(1, &kept.Value());
    REQUIRE(rig.manager->OnReady(mine, TaskLane::Game, RecordReady(env.allocator, inScope)));
    REQUIRE(rig.manager->OnReady(theirs, TaskLane::Game, RecordReady(env.allocator, outScope)));
    REQUIRE(rig.Settle());
    REQUIRE(inScope.leases.Size() == 1 && outScope.leases.Size() == 1);
    LoadTicket later = rig.Request(2, &revoked.Value());
    REQUIRE(rig.manager->OnReady(later, TaskLane::Game, RecordReady(env.allocator, pending)));
    CHECK(rig.UnitOf(1).info.leases == 2 && rig.UnitOf(1).info.tickets == 2);

    REQUIRE(revoked.Value().Revoke());
    CHECK(!inScope.leases[0].IsValid());
    CHECK(outScope.leases[0].IsValid());
    CHECK(rig.UnitOf(1).info.leases == 1 && rig.UnitOf(1).info.tickets == 1);
    CHECK(rig.Request(1, &revoked.Value()).IsValid() == false);
    AssetRequest request;
    request.id = FixedId(1);
    request.scope = &revoked.Value();
    CHECK(rig.manager->RequestAsset(request).Error() == ErrorCode::ScopeClosed);
    REQUIRE(rig.Pump());
    CHECK(pending.calls == 1 && pending.error == ErrorCode::ScopeClosed);
    // 회수된 lease의 소멸은 보유를 다시 줄이지 않는다.
    inScope.leases.Clear();
    REQUIRE(rig.Pump());
    CHECK(rig.UnitOf(1).info.leases == 1 && rig.IsLoaded(1));

    // scope 소멸은 Revoke다.
    kept = Result<AssetScope>::Failure(ErrorCode::None);
    CHECK(!outScope.leases[0].IsValid());
    outScope.leases.Clear();
    mine = LoadTicket();
    theirs = LoadTicket();
    later = LoadTicket();
    REQUIRE(rig.Pump());
    CHECK(!rig.IsLoaded(1));
}

// LoadBlocking(11.7절 8): 결과까지 기다려 lease를 준다. 프레임 실행 중이면 WrongContext,
// 매니페스트에 없는 id는 NotFound(진단 포함)다.
TEST(AssetManager_LoadBlockingWaitsAndRejectsFrames) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ManagerRig rig(env);
    rig.scene.Add(1);
    rig.scene.Add(2);
    rig.scene.Hard(1, 2);
    REQUIRE(rig.Cook());
    bool inFrame = false;
    rig.config.loader.inFrame = FrameFlag;
    rig.config.loader.inFrameContext = &inFrame;
    REQUIRE(rig.Start());
    AssetRequest request;
    request.id = FixedId(1);
    inFrame = true;
    CHECK(rig.manager->LoadBlocking(request, "ManagerHandleTests.Frame").Error() ==
          ErrorCode::WrongContext);
    inFrame = false;
    {
        auto lease = rig.manager->LoadBlocking(request, "ManagerHandleTests.Load");
        REQUIRE(lease);
        CHECK(lease.Value().IsValid() && lease.Value().Asset() == FixedId(1));
        CHECK(rig.IsLoaded(1) && rig.IsLoaded(2));
        CHECK(rig.UnitOf(1).info.leases == 1 && rig.UnitOf(1).info.tickets == 0);
        bool visited = false;
        REQUIRE(rig.manager->With(lease.Value(), [&](ObjectHandle asset) noexcept {
            visited = asset == rig.AssetObject(1);
        }));
        CHECK(visited);
    }

    AssetDiagnostic failure;
    AssetRequest missing;
    missing.id = FixedId(99);
    CHECK(rig.manager->LoadBlocking(missing, "ManagerHandleTests.Missing", &failure).Error() ==
          ErrorCode::NotFound);
    CHECK(failure.code == AssetDiagnosticCode::MissingDependency && failure.asset == FixedId(99));
    REQUIRE(rig.Pump());
    REQUIRE(rig.Pump());
    CHECK(!rig.IsLoaded(1) && !rig.IsLoaded(2));
}

// 기대 타입과 준비 수준(11.1절): 대표 에셋이 기대 타입이 아니면 TypeMismatch(진단 asset), Render
// 준비는 UnsupportedCapability, 알 수 없는 비트·0은 InvalidArgument다.
TEST(AssetManager_ExpectedTypeAndReadyMask) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ManagerRig rig(env);
    rig.scene.Add(1);
    REQUIRE(rig.Cook());
    REQUIRE(rig.Start());
    ReadyRecord matching(env.allocator);
    ReadyRecord mismatched(env.allocator);
    AssetRequest request;
    request.id = FixedId(1);
    request.expectedType = TestAssetType();
    auto good = rig.manager->RequestAsset(request);
    request.expectedType = TestPartType();
    auto bad = rig.manager->RequestAsset(request);
    REQUIRE(good && bad);
    REQUIRE(
        rig.manager->OnReady(good.Value(), TaskLane::Game, RecordReady(env.allocator, matching)));
    REQUIRE(
        rig.manager->OnReady(bad.Value(), TaskLane::Game, RecordReady(env.allocator, mismatched)));
    REQUIRE(rig.Settle());
    CHECK(matching.calls == 1 && matching.error == ErrorCode::None);
    CHECK(mismatched.calls == 1 && mismatched.error == ErrorCode::TypeMismatch);
    CHECK(mismatched.diagnostic.code == AssetDiagnosticCode::TypeMismatch &&
          mismatched.diagnostic.asset == FixedId(1));
    // 타입이 맞지 않은 티켓은 보유를 걸지 않는다.
    CHECK(rig.UnitOf(1).info.tickets == 1);

    AssetRequest render;
    render.id = FixedId(1);
    render.required = static_cast<uint8>(ReadyMask::Cpu) | static_cast<uint8>(ReadyMask::Render);
    CHECK(rig.manager->RequestAsset(render).Error() == ErrorCode::UnsupportedCapability);
    render.required = 0;
    CHECK(rig.manager->RequestAsset(render).Error() == ErrorCode::InvalidArgument);
    render.required = 0x80;
    CHECK(rig.manager->RequestAsset(render).Error() == ErrorCode::InvalidArgument);
}

// 미준비 대상 정책(11.7절 9): Fail은 상주가 아니면 NotReady(run·fail 모두 부르지 않음), 상주면 다음
// Pump에서 run. Defer·PromoteAndDefer는 요청을 내고 준비되면 run. 실패는 fail(오류·진단), Cancel·
// scope Revoke는 fail(Cancelled·ScopeClosed) 한 번이고, 큐 상한은 CapacityExceeded다.
TEST(AssetManager_RunWhenReadyPolicies) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ManagerRig rig(env);
    rig.scene.Add(1);
    rig.scene.Add(2);
    rig.scene.Add(3);
    REQUIRE(rig.Cook());
    rig.scene.FileOf(3)[CorruptOffset] ^= 0x01;
    rig.config.maxDeferredCommands = 3;
    REQUIRE(rig.Start());
    DeferredRecord notReady;
    auto refused = rig.manager->RunWhenReady(FixedId(1), NotReadyPolicy::Fail, nullptr,
                                             RecordRun(env.allocator, notReady),
                                             RecordFail(env.allocator, notReady));
    CHECK(refused.Error() == ErrorCode::NotReady);

    DeferredRecord deferred;
    DeferredRecord promoted;
    DeferredRecord broken;
    auto first = rig.manager->RunWhenReady(FixedId(1), NotReadyPolicy::Defer, nullptr,
                                           RecordRun(env.allocator, deferred),
                                           RecordFail(env.allocator, deferred));
    auto second = rig.manager->RunWhenReady(FixedId(2), NotReadyPolicy::PromoteAndDefer, nullptr,
                                            RecordRun(env.allocator, promoted),
                                            RecordFail(env.allocator, promoted));
    auto third = rig.manager->RunWhenReady(FixedId(3), NotReadyPolicy::Defer, nullptr,
                                           RecordRun(env.allocator, broken),
                                           RecordFail(env.allocator, broken));
    REQUIRE(first && second && third);
    DeferredRecord overflow;
    CHECK(rig.manager
              ->RunWhenReady(FixedId(2), NotReadyPolicy::Defer, nullptr,
                             RecordRun(env.allocator, overflow),
                             RecordFail(env.allocator, overflow))
              .Error() == ErrorCode::CapacityExceeded);
    CHECK(deferred.runs == 0);
    REQUIRE(rig.Settle());
    CHECK(deferred.runs == 1 && deferred.fails == 0 && deferred.ranAsset == FixedId(1));
    CHECK(promoted.runs == 1 && promoted.fails == 0);
    CHECK(broken.runs == 0 && broken.fails == 1 && broken.error == ErrorCode::CorruptData &&
          broken.diagnostic.code == AssetDiagnosticCode::CorruptPackage);
    CHECK(overflow.runs == 0 && overflow.fails == 0);

    // 상주(lease가 붙잡음)면 Fail 정책도 받아 다음 Pump에서 실행한다.
    auto lease = rig.manager->LoadBlocking(RequestOf(1), "ManagerHandleTests.Lease");
    REQUIRE(lease);
    DeferredRecord resident;
    auto now = rig.manager->RunWhenReady(FixedId(1), NotReadyPolicy::Fail, nullptr,
                                         RecordRun(env.allocator, resident),
                                         RecordFail(env.allocator, resident));
    REQUIRE(now);
    CHECK(resident.runs == 0);
    REQUIRE(rig.Pump());
    CHECK(resident.runs == 1 && resident.fails == 0);

    // Cancel과 scope Revoke.
    auto scope = rig.manager->CreateScope();
    REQUIRE(scope);
    DeferredRecord cancelled;
    DeferredRecord closed;
    auto cancel = rig.manager->RunWhenReady(FixedId(2), NotReadyPolicy::Defer, nullptr,
                                            RecordRun(env.allocator, cancelled),
                                            RecordFail(env.allocator, cancelled));
    auto scoped = rig.manager->RunWhenReady(FixedId(2), NotReadyPolicy::Defer, &scope.Value(),
                                            RecordRun(env.allocator, closed),
                                            RecordFail(env.allocator, closed));
    REQUIRE(cancel && scoped);
    REQUIRE(rig.manager->Cancel(cancel.Value()));
    REQUIRE(scope.Value().Revoke());
    REQUIRE(rig.Pump());
    CHECK(cancelled.runs == 0 && cancelled.fails == 1 && cancelled.error == ErrorCode::Cancelled);
    CHECK(closed.runs == 0 && closed.fails == 1 && closed.error == ErrorCode::ScopeClosed);
    REQUIRE(rig.manager->Cancel(cancel.Value()));
    REQUIRE(rig.Pump());
    CHECK(cancelled.fails == 1);
    DeferredRecord refusedScope;
    CHECK(rig.manager
              ->RunWhenReady(FixedId(2), NotReadyPolicy::Defer, &scope.Value(),
                             RecordRun(env.allocator, refusedScope),
                             RecordFail(env.allocator, refusedScope))
              .Error() == ErrorCode::ScopeClosed);
    CHECK(rig.manager
              ->RunWhenReady(FixedId(2), NotReadyPolicy::Defer, nullptr, DeferredRun(),
                             RecordFail(env.allocator, refusedScope))
              .Error() == ErrorCode::InvalidArgument);
    CHECK(rig.manager
              ->RunWhenReady(FixedId(99), NotReadyPolicy::Defer, nullptr,
                             RecordRun(env.allocator, refusedScope),
                             RecordFail(env.allocator, refusedScope))
              .Error() == ErrorCode::NotFound);
}

namespace {

struct ThreadRelease {
    AssetLease lease;
    LoadTicket ticket;
};

void ReleaseOnOtherThread(void* context) noexcept {
    auto* release = static_cast<ThreadRelease*>(context);
    release->lease = AssetLease();
    release->ticket = LoadTicket();
}

} // namespace

// 다른 스레드의 해제(11.7절 6): lease·티켓을 다른 스레드에서 소멸하면 해제 큐에 들어가고(Join 뒤)
// 다음 Pump가 반영해 상주를 놓는다.
TEST(AssetManager_ReleaseFromOtherThread) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ManagerRig rig(env);
    rig.scene.Add(1);
    REQUIRE(rig.Cook());
    REQUIRE(rig.Start());
    ThreadRelease release;
    release.ticket = rig.Request(1);
    auto lease = rig.manager->LoadBlocking(RequestOf(1), "ManagerHandleTests.Thread");
    REQUIRE(lease);
    release.lease = Move(lease.Value());
    REQUIRE(rig.Pump());
    CHECK(rig.UnitOf(1).info.leases == 1 && rig.UnitOf(1).info.tickets == 1);
    platform::Thread thread;
    REQUIRE(thread.Start(ReleaseOnOtherThread, &release) == platform::ThreadStatus::Success);
    REQUIRE(thread.Join() == platform::ThreadStatus::Success);
    CHECK(rig.IsLoaded(1));
    REQUIRE(rig.Pump());
    CHECK(!rig.IsLoaded(1));
}

// 종료(11.7절 12): 아직 부르지 않은 callback·보류 명령 fail은 ShuttingDown을 한 번 받고, lease는
// 회수되며(IsValid false), 그 뒤 요청·Pump는 ShuttingDown이다.
TEST(AssetManager_ShutdownDeliversShuttingDown) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ManagerRig rig(env);
    rig.scene.Add(1);
    rig.scene.Add(2);
    REQUIRE(rig.Cook());
    REQUIRE(rig.Start());
    ReadyRecord pending(env.allocator);
    DeferredRecord command;
    auto lease = rig.manager->LoadBlocking(RequestOf(1), "ManagerHandleTests.Shut");
    REQUIRE(lease);
    LoadTicket ticket = rig.Request(2);
    REQUIRE(rig.manager->OnReady(ticket, TaskLane::Game, RecordReady(env.allocator, pending)));
    auto deferred = rig.manager->RunWhenReady(FixedId(2), NotReadyPolicy::Defer, nullptr,
                                              RecordRun(env.allocator, command),
                                              RecordFail(env.allocator, command));
    REQUIRE(deferred);
    REQUIRE(rig.manager->Shutdown());
    CHECK(pending.calls == 1 && pending.error == ErrorCode::ShuttingDown);
    CHECK(command.fails == 1 && command.error == ErrorCode::ShuttingDown && command.runs == 0);
    CHECK(!lease.Value().IsValid());
    CHECK(rig.manager->RequestAsset(RequestOf(1)).Error() == ErrorCode::ShuttingDown);
    CHECK(rig.manager->Pump().Error() == ErrorCode::ShuttingDown);
    REQUIRE(rig.manager->Shutdown());
    CHECK(pending.calls == 1 && command.fails == 1);
}

} // namespace manager_handle_tests_file
