#include "ManagerTestSupport.h"

#include <AssetTestFixtures/TestAssets.generated.h>
#include <Object/ConstructionContext.h>
#include <Platform/Process.h>
#include <Platform/Thread.h>
#include <TestSupport/HostFailure.h>
#include <TestSupport/NFailAllocator.h>
#include <string.h>

// AssetManager 실패·경계 시험이다(Docs/Design/AssetSystem.ko.md 11.7절 3·6·13): Discard Busy(열린
// ConstructionContext)로 Closing에 머무름과 되살리기, 첫 Discard 뒤 Busy인 Discarding(퇴출된 id의
// 새 로드·Discarding 단위로 생긴 늦은 간선 진단), 모든 할당 지점의 실패 뒤 일관성, 수명 위반
// Fatal(살아 있는 lease·소유 스레드 밖 파괴·scope 소멸).
using namespace rocks;

namespace rocks::test {
bool RegisterProcessProbe(const char* mode,
                          int (*function)(int, const char* const*) noexcept) noexcept;
} // namespace rocks::test

namespace manager_failure_tests_file {

using namespace rocks::asset_test;

namespace {

// asset 아래 공개 전 객체를 만든 트랜잭션을 연다 — 그 패키지 부분 트리에 공개 전 객체가 있어
// Discard가 Busy다(11.7절 3, ObjectRegistry::Discard). 패키지 바로 아래는 대표 자식 규칙 때문에
// asset 객체 아래에 만든다.
bool HoldPending(ManagerRig& rig, UniquePtr<ConstructionContext>& transaction,
                 ObjectHandle asset) noexcept {
    ObjectRegistry& registry = rig.manager->Registry();
    auto made = UniquePtr<ConstructionContext>::TryMake(rig.env.allocator, registry,
                                                        registry.OwnerAccess());
    if (!made) {
        return false;
    }

    transaction = Move(made.Value());
    return transaction->Create<TestPart>(TestPartType(), asset, "Pending").IsOk();
}

// 에셋 객체 owner의 hard 필드를 target으로 바꾼다(퇴출된 옛 객체도 쓴다).
bool SetHardTo(ManagerRig& rig, ObjectHandle owner, ObjectHandle target) noexcept {
    ObjectRegistry& registry = rig.manager->Registry();
    const ObjectAccessContext access = registry.OwnerAccess();
    auto borrow = registry.ResolveForWrite<TestAsset>(access, owner);
    return borrow &&
           registry.SetReference(access, owner, borrow.Value().Get().Value()->hard, target).IsOk();
}

// 에셋 객체 owner의 hardList 끝에 target을 더한다.
bool AddListTo(ManagerRig& rig, ObjectHandle owner, ObjectHandle target) noexcept {
    ObjectRegistry& registry = rig.manager->Registry();
    const ObjectAccessContext access = registry.OwnerAccess();
    auto borrow = registry.ResolveForWrite<TestAsset>(access, owner);
    if (!borrow) {
        return false;
    }

    ObjectRefArray<Object>& list = borrow.Value().Get().Value()->hardList;
    return registry.InsertReferenceAt(access, owner, list, list.Size(), target).IsOk();
}

} // namespace

// Discard Busy(11.7절 3): 단위의 첫 패키지 Discard가 Busy면 아무것도 바뀌지 않아 Closing으로 남고
// (로더에서도 공개 상태), 그 사이 요청은 되살린다. 트랜잭션이 닫히면 다음 Pump가 Discard한다.
TEST(AssetManager_DiscardBusyKeepsClosing) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ManagerRig rig(env);
    rig.scene.Add(1);
    REQUIRE(rig.Cook());
    REQUIRE(rig.Start());
    UniquePtr<ConstructionContext> pending;
    {
        LoadTicket ticket = rig.Request(1);
        REQUIRE(rig.Settle());
        REQUIRE(HoldPending(rig, pending, rig.AssetObject(1)));
    }

    REQUIRE(rig.Pump());
    CHECK(rig.UnitOf(1).info.state == ResidentUnitState::Closing);
    CHECK(rig.IsLoaded(1));
    {
        LoadTicket revive = rig.Request(1);
        REQUIRE(rig.Pump());
        CHECK(rig.UnitOf(1).info.state == ResidentUnitState::Resident);
    }

    REQUIRE(rig.Pump());
    CHECK(rig.UnitOf(1).info.state == ResidentUnitState::Closing);
    pending.Reset();
    REQUIRE(rig.Pump());
    CHECK(!rig.IsLoaded(1) && rig.UnitCount() == 0);
}

// Discarding(11.7절 3): 순환 단위 1 ↔ 2에서 1의 Discard는 성공하고 2가 Busy면 단위는 Discarding이고
// 로더에서 퇴출된다 — 같은 id 요청은 새 로드로 새 패키지를 만들고(옛 것과 함께 있다), 아직
// Discard하지 않은 옛 2로 새 하드 간선이 생기면 늦은 간선 진단에 남는다(지우기는 세지 않는다). 옛
// 2에서 나가는 새 간선은 보유로만 세고, 상주 단위 사이 순환 탐색은 Discarding 단위를 지나지 않는다.
// 트랜잭션이 닫히면 옛 단위가 끝난다.
TEST(AssetManager_DiscardingRetiresAndReportsLateEdges) {
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
    LoadTicket owner = rig.Request(3);
    UniquePtr<ConstructionContext> pending;
    ObjectHandle oldTwo;
    {
        LoadTicket ticket = rig.Request(1);
        REQUIRE(rig.Settle());
        oldTwo = rig.AssetObject(2);
        REQUIRE(HoldPending(rig, pending, rig.AssetObject(2)));
    }

    const usize begun = rig.io.begun.Size();
    REQUIRE(rig.Pump());
    REQUIRE(rig.UnitOf(1).found);
    CHECK(rig.UnitOf(1).info.state == ResidentUnitState::Discarding);
    CHECK(!rig.IsLoaded(1) && !rig.IsLoaded(2));

    // 퇴출된 id의 새 로드(옛 단위와 함께 있다).
    LoadTicket again = rig.Request(1);
    REQUIRE(rig.Settle());
    CHECK(rig.IsLoaded(1) && rig.IsLoaded(2));
    CHECK(rig.io.begun.Size() == begun + 2);
    CHECK(rig.UnitCount() == 3);

    // 아직 Discard하지 않은 옛 2로 생긴 간선(둘째부터는 경고 없이 센다).
    const ObjectHandle three = rig.AssetObject(3);
    REQUIRE(SetHardTo(rig, three, oldTwo));
    REQUIRE(rig.Pump());
    auto late = [&]() noexcept {
        AssetManagerSnapshot snapshot(env.allocator);
        return rig.manager->Describe(snapshot) ? snapshot.lateEdgesIntoDiscarding : 0;
    };
    CHECK(late() == 1);
    REQUIRE(AddListTo(rig, three, oldTwo));
    REQUIRE(rig.Pump());
    CHECK(late() == 2);

    // Discarding 단위에서 나가는 새 간선은 대상의 보유이지만 순환 후보가 아니다.
    REQUIRE(SetHardTo(rig, oldTwo, three));
    REQUIRE(rig.Pump());
    CHECK(rig.UnitOf(3).info.inboundEdges == 1 && rig.UnitCount() == 3);
    // 늦은 간선을 지우는 것은 세지 않는다.
    REQUIRE(SetHardTo(rig, three, ObjectHandle{}));
    REQUIRE(rig.Pump());
    CHECK(late() == 2);

    // 새 1 ↔ 3 순환 탐색은 3에서 Discarding 단위로 가는 간선을 지나지 않는다.
    REQUIRE(rig.SetHard(1, 3));
    REQUIRE(rig.AddHardListEntry(3, 1));
    REQUIRE(rig.Pump());
    CHECK(rig.UnitCount() == 2 && rig.UnitOf(3).assets == 3);

    pending.Reset();
    REQUIRE(rig.Pump());
    CHECK(rig.UnitCount() == 1);
    // 옛 2의 Discard가 반납한 나가는 간선(옛 2 → 3)은 다음 Pump에 반영된다.
    CHECK(rig.UnitOf(3).info.inboundEdges == 1);
    REQUIRE(rig.Pump());
    CHECK(rig.UnitOf(3).info.inboundEdges == 0);
}

// 할당 실패(11.7절 — 모든 지점): Initialize·scope·요청·callback·보류 명령·묶음 로드(비ASCII bundle
// 이름 — 키 정규화가 할당한다)·용량 추정·넘겨받기·간선 표·병합·Discard·lease·동기 로드의 어느
// 할당이 한 번 실패해도 상태가 일관되고(그 요청만 실패하거나 다음 Pump에서 다시 한다), 실패를 끈 뒤
// 모든 handle을 놓으면 상주 단위·살아 있는 handle이 남지 않는다.
constexpr char NonAsciiBundleName[] = "Visual\xEC\x8B\x9C\xEA\xB0\x81";

bool CookSweepScene(ManagerRig& rig) noexcept {
    rig.scene.Add(1).soft = 4;
    rig.scene.Add(2);
    rig.scene.Add(3);
    rig.scene.Add(4);
    rig.scene.Add(5);
    rig.scene.Hard(1, 2);
    rig.scene.Hard(2, 1);
    rig.scene.Hard(1, 3);
    if (!rig.Cook()) {
        return false;
    }

    CookedManifestEntry& entry = rig.scene.EntryOf(1);
    entry.flags = static_cast<uint32>(CookedManifestFlags::PrimaryAsset);
    const uint64 members[] = {4};
    return AddBundle(entry, NonAsciiBundleName, members);
}

void RunSweepScenario(ManagerRig& rig, ReadyRecord& ready, PrimaryAssetLease& primaryLease,
                      uint32& runs) noexcept {
    Allocator& allocator = rig.env.allocator;
    // 묶음 로드를 먼저 한다(티켓 표보다 묶음 표가 먼저 자라 전달 목록 확장을 묶음 쪽에서 한다).
    auto bare = rig.manager->LoadPrimaryAsset(FixedId(1), {}, LoadPriority::High);
    auto scope = rig.manager->CreateScope();
    AssetScope* scoped = scope ? &scope.Value() : nullptr;
    LoadTicket ticket = rig.Request(1, scoped);
    if (ticket.IsValid()) {
        (void)rig.manager->OnReady(ticket, TaskLane::Game, RecordReady(allocator, ready));
    }

    auto run = DeferredRun::TryCreate(allocator, [&runs](const AssetLease&) noexcept { ++runs; });
    auto fail =
        DeferredFail::TryCreate(allocator, [](ErrorCode, const AssetDiagnostic&) noexcept {});
    Result<DeferredCommand> deferred = Result<DeferredCommand>::Failure(ErrorCode::OutOfMemory);
    if (run && fail) {
        deferred = rig.manager->RunWhenReady(FixedId(5), NotReadyPolicy::PromoteAndDefer, scoped,
                                             Move(run.Value()), Move(fail.Value()));
    }

    const StringView bundles[] = {NonAsciiBundleName};
    auto primary = rig.manager->LoadPrimaryAsset(FixedId(1), bundles, LoadPriority::Normal, scoped);
    if (primary) {
        auto callback = PrimaryReadyCallback::TryCreate(
            allocator,
            [&primaryLease](Result<PrimaryAssetLease> lease, const AssetDiagnostic&) noexcept {
                if (lease) {
                    primaryLease = Move(lease.Value());
                }
            });
        if (callback) {
            (void)rig.manager->OnReady(primary.Value(), TaskLane::Game, Move(callback.Value()));
        }
    }

    (void)rig.manager->EstimatePrimaryAsset(FixedId(1), bundles);
    (void)rig.manager->EstimatePrimaryAsset(FixedId(1), {});
    constexpr uint32 Rounds = 64;
    for (uint32 round = 0; round < Rounds; ++round) {
        rig.io.Complete();
        rig.worker.RunAll();
        (void)rig.manager->Pump();
    }

    auto blocking = rig.manager->LoadBlocking(RequestOf(5), "ManagerFailureTests.Sweep");
    if (rig.IsLoaded(3) && rig.IsLoaded(5)) {
        (void)rig.SetHard(5, 3);
        (void)rig.manager->Pump();
        (void)rig.SetHard(3, 5);
        (void)rig.manager->Pump();
    }

    (void)rig.manager->Pump();
}

TEST(AssetManager_AllocationFailures) {
    AssetEnv env;
    REQUIRE(env.Ready());
    test::NFailAllocator failing(env.allocator);
    uint32 failures = 0;
    uint32 completeRuns = 0;
    for (uint64 attempt = 1;; ++attempt) {
        ManagerRig rig(env);
        REQUIRE(CookSweepScene(rig));
        rig.config.loader.allocator = &failing;
        failing.FailOnAttempt(attempt);
        auto made = UniquePtr<AssetManager>::TryMake(env.allocator, rig.config);
        REQUIRE(made);
        rig.manager = Move(made.Value());
        bool failed = false;
        if (rig.manager->Initialize().IsOk()) {
            ReadyRecord ready(env.allocator);
            PrimaryAssetLease primaryLease(env.allocator);
            uint32 runs = 0;
            RunSweepScenario(rig, ready, primaryLease, runs);
            failed = failing.Attempts() >= attempt;
            failing.DisableFailure();
            completeRuns += failed ? 0 : 1;
            ready.leases.Clear();
            primaryLease = PrimaryAssetLease(env.allocator);
            constexpr uint32 CleanupRounds = 16;
            for (uint32 round = 0; round < CleanupRounds; ++round) {
                rig.io.Complete();
                rig.worker.RunAll();
                REQUIRE(rig.Pump());
            }

            AssetManagerSnapshot snapshot(env.allocator);
            REQUIRE(rig.manager->Describe(snapshot));
            CHECK(snapshot.units.IsEmpty() && snapshot.liveTickets == 0 &&
                  snapshot.liveLeases == 0 && snapshot.deferredCommands == 0);
            CHECK(snapshot.loader.units.IsEmpty());
            for (uint64 id = 1; id <= 5; ++id) {
                CHECK(!rig.IsLoaded(id));
            }
        } else {
            failed = failing.Attempts() >= attempt;
        }

        failing.DisableFailure();
        rig.manager.Reset();
        if (!failed) {
            break;
        }

        ++failures;
    }

    // 끝 실행은 실패 없이 전 과정을 돌았다(시나리오가 모든 할당 지점을 지났다).
    CHECK(completeRuns == 1);
    CHECK(failures > 0);
}

int AssetManagerFatalProbe(int argc, const char* const* argv) noexcept {
    rocks::test::PrepareFatalTestProcess();
    constexpr int ScenarioArgument = 3;
    if (argc <= ScenarioArgument) {
        return 5;
    }

    AssetEnv env;
    if (!env.Ready()) {
        return 2;
    }

    ManagerRig rig(env);
    rig.scene.Add(1);
    if (!rig.Cook() || !rig.Start()) {
        return 3;
    }

    const char* scenario = argv[ScenarioArgument];
    if (strcmp(scenario, "in-callback") == 0) {
        LoadTicket ticket = rig.Request(1);
        auto callback = AssetReadyCallback::TryCreate(
            env.allocator,
            [&rig](Result<AssetLease>, const AssetDiagnostic&) noexcept { rig.manager.Reset(); });
        if (!callback || !rig.manager->OnReady(ticket, TaskLane::Game, Move(callback.Value()))) {
            return 4;
        }

        (void)rig.Settle();
        return 0;
    }

    if (strcmp(scenario, "live-lease") == 0) {
        auto lease = rig.manager->LoadBlocking(RequestOf(1), "ManagerFailureTests.Probe");
        rig.manager.Reset();
        return 0;
    }

    if (strcmp(scenario, "scope-off-thread") == 0) {
        auto scope = rig.manager->CreateScope();
        if (!scope) {
            return 4;
        }

        platform::Thread thread;
        if (thread.Start(
                [](void* context) noexcept { static_cast<AssetScope*>(context)->~AssetScope(); },
                &scope.Value()) != platform::ThreadStatus::Success) {
            return 4;
        }

        (void)thread.Join();
        return 0;
    }

    platform::Thread thread;
    if (thread.Start(
            [](void* context) noexcept { static_cast<ManagerRig*>(context)->manager.Reset(); },
            &rig) != platform::ThreadStatus::Success) {
        return 4;
    }

    (void)thread.Join();
    return 0;
}

const bool AssetManagerProbeRegistered =
    rocks::test::RegisterProcessProbe("asset-manager", &AssetManagerFatalProbe);

bool AssetManagerProbeFails(const char* scenario, const char* message) noexcept {
    char executable[4096];
    usize executableBytes = 0;
    if (!platform::GetExecutablePath(executable, sizeof(executable), executableBytes)) {
        return false;
    }

    const platform::ProcessArgument arguments[] = {
        {"--process-probe", 15}, {"asset-manager", 13}, {scenario, strlen(scenario)}};
    platform::ProcessStartInfo info;
    info.executable = executable;
    info.executableBytes = executableBytes;
    info.arguments = arguments;
    info.argumentCount = 3;
    info.output = platform::ProcessStreamMode::Null;
    info.error = platform::ProcessStreamMode::Pipe;
    static char error[4096];
    platform::ProcessRunOptions options;
    options.error = error;
    options.errorCapacity = sizeof(error) - 1;
    const platform::ProcessRunResult run = platform::RunProcess(info, options);
    if (!run.status || run.timedOut) {
        return false;
    }

    error[run.errorBytes < sizeof(error) ? run.errorBytes : sizeof(error) - 1] = '\0';
    const bool abnormal = run.exit.kind != platform::ProcessExitKind::Exited || run.exit.code != 0;
    return abnormal && strstr(error, message) != nullptr;
}

// 수명 위반(11.7절 1·6): 살아 있는 lease를 남긴 채 AssetManager를 파괴하거나, 소유 스레드 밖에서
// AssetManager·AssetScope를 파괴하거나, 자기 callback 안에서 AssetManager를 파괴하면 자식
// 프로세스가 Fatal로 끝나고 그 문구를 남긴다.
TEST(AssetManager_LifetimeViolationsAreFatal) {
    REQUIRE(AssetManagerProbeRegistered);
    CHECK(AssetManagerProbeFails("live-lease", "AssetManager destroyed while leases"));
    CHECK(AssetManagerProbeFails("off-thread", "AssetManager destroyed outside its owner thread"));
    CHECK(AssetManagerProbeFails("in-callback", "AssetManager destroyed inside its own callback"));
    CHECK(AssetManagerProbeFails("scope-off-thread",
                                 "AssetScope destroyed outside the AssetManager owner thread"));
}

// 상태 기술(11.7절 11): 단위마다 보유 종류별 수·간선·로더 보유와 멤버 id, 살아 있는 티켓·lease·보류
// 명령 수를 한 번에 채운다. 다시 부르면 이전 내용을 비우고 채운다. Initialize 전은 InvalidArgument,
// 소유 스레드 밖은 WrongThread, out 할당 실패는 OutOfMemory로 전파한다.
struct DescribeOffThread {
    AssetManager* manager = nullptr;
    AssetManagerSnapshot* snapshot = nullptr;
    ErrorCode error = ErrorCode::None;
};

TEST(AssetManager_DescribeReportsState) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ManagerRig rig(env);
    rig.scene.Add(1);
    rig.scene.Add(2);
    rig.scene.Add(3);
    rig.scene.Add(4);
    rig.scene.Hard(1, 2);
    REQUIRE(rig.Cook());
    {
        auto made = UniquePtr<AssetManager>::TryMake(env.allocator, rig.config);
        REQUIRE(made);
        AssetManagerSnapshot early(env.allocator);
        CHECK(made.Value()->Describe(early).Error() == ErrorCode::InvalidArgument);
    }

    REQUIRE(rig.Start());
    LoadTicket ticket = rig.Request(1);
    REQUIRE(rig.Settle());
    auto lease = rig.manager->LoadBlocking(RequestOf(3), "ManagerFailureTests.Describe");
    REQUIRE(lease);
    auto run = DeferredRun::TryCreate(env.allocator, [](const AssetLease&) noexcept {});
    auto fail =
        DeferredFail::TryCreate(env.allocator, [](ErrorCode, const AssetDiagnostic&) noexcept {});
    REQUIRE(run && fail);
    auto deferred = rig.manager->RunWhenReady(FixedId(4), NotReadyPolicy::Defer, nullptr,
                                              Move(run.Value()), Move(fail.Value()));
    REQUIRE(deferred);
    REQUIRE(rig.Pump());

    AssetManagerSnapshot snapshot(env.allocator);
    for (uint32 round = 0; round < 2; ++round) {
        REQUIRE(rig.manager->Describe(snapshot));
        CHECK(snapshot.units.Size() == 3 && snapshot.unitAssets.Size() == 3);
        CHECK(snapshot.liveTickets == 1 && snapshot.liveLeases == 1);
        CHECK(snapshot.deferredCommands == 1 && snapshot.lateEdgesIntoDiscarding == 0);
        // Defer는 4를 Normal로 요청한다 — IO를 내보내지 않았으므로 로더 단위로 남는다.
        CHECK(snapshot.loader.units.Size() == 1);
    }

    const auto first = rig.UnitOf(1);
    const auto second = rig.UnitOf(2);
    const auto third = rig.UnitOf(3);
    REQUIRE(first.found && second.found && third.found);
    CHECK(first.info.tickets == 1 && first.info.leases == 0 && first.info.inboundEdges == 0);
    CHECK(second.info.tickets == 0 && second.info.inboundEdges == 1);
    CHECK(third.info.leases == 1 && third.info.tickets == 0);
    CHECK(first.info.state == ResidentUnitState::Resident && first.info.loaderHolds == 0);
    CHECK(!rig.UnitOf(4).found);

    DescribeOffThread off;
    off.manager = rig.manager.Get();
    off.snapshot = &snapshot;
    platform::Thread thread;
    REQUIRE(thread.Start(
                [](void* context) noexcept {
                    auto& state = *static_cast<DescribeOffThread*>(context);
                    auto described = state.manager->Describe(*state.snapshot);
                    state.error = described ? ErrorCode::None : described.Error();
                },
                &off) == platform::ThreadStatus::Success);
    REQUIRE(thread.Join() == platform::ThreadStatus::Success);
    CHECK(off.error == ErrorCode::WrongThread);

    // out 할당의 모든 지점(로더 기술·단위·멤버 id).
    test::NFailAllocator failing(env.allocator);
    uint32 failures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        AssetManagerSnapshot partial(failing);
        failing.FailOnAttempt(attempt);
        auto described = rig.manager->Describe(partial);
        const bool failed = failing.Attempts() >= attempt;
        failing.DisableFailure();
        if (!failed) {
            CHECK(described.IsOk() && partial.units.Size() == 3);
            break;
        }

        CHECK(described.Error() == ErrorCode::OutOfMemory);
        ++failures;
    }

    CHECK(failures >= 2);
}

// 보유 종류 × 단위 모양 × 해제 순서 조합(11.7절 3·4·10): 티켓·lease·묶음 lease(PrimaryAssetLease —
// 묶음 티켓은 callback 뒤 놓는다)·들어오는 하드 간선(따로 상주한 3에서) 넷을 단일·순환(1 ↔ 2)·사슬
// (1 → 2) 단위의 1에 걸고 4! = 24가지 순서로 놓는다(72 조합). 마지막 보유를 놓기 전에는 단위가
// Resident로 상주하고 남은 보유 수가 정확하며, 마지막을 놓은 Pump에서 1의 단위가 Discard된다(순환은
// 2도 같이, 사슬은 다음 Pump에 2가 이어서). 보유자 3은 끝까지 남는다.
enum class UnitShape : uint8 {
    Single,
    Cycle,
    Chain
};

enum class HoldKind : uint8 {
    Ticket,
    Lease,
    PrimaryLease,
    Edge
};

constexpr UnitShape AllShapes[] = {UnitShape::Single, UnitShape::Cycle, UnitShape::Chain};

constexpr uint32 HoldKindCount = 4;
constexpr uint32 ReleaseOrderCount = 24;

// order번째 해제 순서다(계승 진법 — 0..23이 서로 다른 순열 전부).
void ReleaseOrderAt(uint32 order, HoldKind (&out)[HoldKindCount]) noexcept {
    HoldKind remaining[HoldKindCount] = {HoldKind::Ticket, HoldKind::Lease, HoldKind::PrimaryLease,
                                         HoldKind::Edge};
    uint32 left = HoldKindCount;
    for (uint32 position = 0; position < HoldKindCount; ++position) {
        const uint32 pick = order % left;
        order /= left;
        out[position] = remaining[pick];
        for (uint32 shift = pick; shift + 1 < left; ++shift) {
            remaining[shift] = remaining[shift + 1];
        }

        --left;
    }
}

TEST(AssetManager_HoldReleaseOrderCombinations) {
    AssetEnv env;
    REQUIRE(env.Ready());
    uint32 cases = 0;
    bool seenOrders[256] = {};
    for (const UnitShape shape : AllShapes) {
        for (uint32 orderIndex = 0; orderIndex < ReleaseOrderCount; ++orderIndex) {
            HoldKind order[HoldKindCount];
            ReleaseOrderAt(orderIndex, order);
            uint32 key = 0;
            for (HoldKind kind : order) {
                key = key * HoldKindCount + static_cast<uint32>(kind);
            }

            seenOrders[key] = true;
            ManagerRig rig(env);
            rig.scene.Add(1);
            rig.scene.Add(2);
            rig.scene.Add(3);
            if (shape != UnitShape::Single) {
                rig.scene.Hard(1, 2);
            }

            if (shape == UnitShape::Cycle) {
                rig.scene.Hard(2, 1);
            }

            REQUIRE(rig.Cook());
            rig.scene.EntryOf(1).flags = static_cast<uint32>(CookedManifestFlags::PrimaryAsset);
            REQUIRE(rig.Start());
            LoadTicket owner = rig.Request(3);
            LoadTicket ticket = rig.Request(1);
            REQUIRE(rig.Settle());
            auto blocking = rig.manager->LoadBlocking(RequestOf(1), "ManagerFailureTests.Order");
            REQUIRE(blocking);
            AssetLease lease = Move(blocking.Value());
            PrimaryAssetLease primaryLease(env.allocator);
            {
                auto primary = rig.manager->LoadPrimaryAsset(FixedId(1), {});
                REQUIRE(primary);
                auto callback = PrimaryReadyCallback::TryCreate(
                    env.allocator, [&primaryLease](Result<PrimaryAssetLease> made,
                                                   const AssetDiagnostic&) noexcept {
                        if (made) {
                            primaryLease = Move(made.Value());
                        }
                    });
                REQUIRE(callback);
                REQUIRE(
                    rig.manager->OnReady(primary.Value(), TaskLane::Game, Move(callback.Value())));
                REQUIRE(rig.Pump());
                REQUIRE(primaryLease.Leases().Size() == 1);
            }

            REQUIRE(rig.SetHard(3, 1));
            REQUIRE(rig.Pump());
            uint32 tickets = 1;
            uint32 leases = 2;
            uint32 edges = 1;
            for (uint32 step = 0; step < HoldKindCount; ++step) {
                const auto unit = rig.UnitOf(1);
                REQUIRE(unit.found);
                CHECK(unit.info.state == ResidentUnitState::Resident);
                CHECK(unit.info.tickets == tickets && unit.info.leases == leases &&
                      unit.info.inboundEdges == edges);
                CHECK(unit.assets == (shape == UnitShape::Cycle ? 2u : 1u));
                switch (order[step]) {
                case HoldKind::Ticket:
                    ticket = LoadTicket();
                    tickets = 0;
                    break;
                case HoldKind::Lease:
                    lease = AssetLease();
                    --leases;
                    break;
                case HoldKind::PrimaryLease:
                    primaryLease = PrimaryAssetLease(env.allocator);
                    --leases;
                    break;
                case HoldKind::Edge:
                    REQUIRE(rig.SetHard(3, 0));
                    edges = 0;
                    break;
                }

                REQUIRE(rig.Pump());
                CHECK(rig.IsLoaded(1) == (step + 1 < HoldKindCount));
            }

            CHECK(!rig.UnitOf(1).found);
            if (shape == UnitShape::Chain) {
                CHECK(rig.IsLoaded(2));
                REQUIRE(rig.Pump());
            }

            CHECK(!rig.IsLoaded(2));
            CHECK(rig.IsLoaded(3) && rig.UnitCount() == 1);
            ++cases;
        }
    }

    // 순서 24개가 서로 다르다(빠짐 없음).
    uint32 distinct = 0;
    for (bool seen : seenOrders) {
        distinct += seen ? 1 : 0;
    }

    CHECK(distinct == ReleaseOrderCount);
    constexpr uint32 ExpectedCases = 72;
    CHECK(cases == ExpectedCases);
}

// handle 상한(11.7절 6): 살아 있는 handle이 AssetManagerMaxLiveHandles에 닿으면 새 요청·보류 명령·
// 묶음 로드·동기 로드는 CapacityExceeded이고, 이미 걸어 둔 callback·보류 명령·묶음 callback은
// lease를 만들 수 없어 그 오류를 받는다. handle 하나를 놓으면 다음 Pump 뒤 다시 된다. 모두 놓으면
// 상주 단위가 남지 않는다.
TEST(AssetManager_HandleCapacity) {
    AssetEnv env;
    REQUIRE(env.Ready());
    ManagerRig rig(env);
    rig.scene.Add(1);
    REQUIRE(rig.Cook());
    rig.scene.EntryOf(1).flags = static_cast<uint32>(CookedManifestFlags::PrimaryAsset);
    REQUIRE(rig.Start());
    auto first = rig.manager->LoadBlocking(RequestOf(1), "ManagerFailureTests.Capacity");
    REQUIRE(first);

    // 상한 직전에 걸어 둘 것들(다음 Pump에 lease를 만든다).
    ReadyRecord ready(env.allocator);
    LoadTicket ticket = rig.Request(1);
    REQUIRE(rig.manager->OnReady(ticket, TaskLane::Game, RecordReady(env.allocator, ready)));
    ErrorCode deferredError = ErrorCode::None;
    auto run = DeferredRun::TryCreate(env.allocator, [](const AssetLease&) noexcept {});
    auto fail = DeferredFail::TryCreate(
        env.allocator, [&deferredError](ErrorCode error, const AssetDiagnostic&) noexcept {
            deferredError = error;
        });
    REQUIRE(run && fail);
    auto deferred = rig.manager->RunWhenReady(FixedId(1), NotReadyPolicy::Defer, nullptr,
                                              Move(run.Value()), Move(fail.Value()));
    REQUIRE(deferred);
    ErrorCode primaryError = ErrorCode::None;
    auto primary = rig.manager->LoadPrimaryAsset(FixedId(1), {});
    REQUIRE(primary);
    auto primaryCallback = PrimaryReadyCallback::TryCreate(
        env.allocator,
        [&primaryError](Result<PrimaryAssetLease> lease, const AssetDiagnostic&) noexcept {
            primaryError = lease ? ErrorCode::None : lease.Error();
        });
    REQUIRE(primaryCallback);
    REQUIRE(rig.manager->OnReady(primary.Value(), TaskLane::Game, Move(primaryCallback.Value())));

    // 남은 칸을 lease로 채운다(티켓·보류 명령·묶음 티켓·첫 lease가 4칸을 쓴다).
    Array<AssetLease> leases(env.allocator);
    REQUIRE(leases.TryReserve(AssetManagerMaxLiveHandles));
    ErrorCode stopped = ErrorCode::None;
    while (stopped == ErrorCode::None) {
        auto lease = rig.manager->LoadBlocking(RequestOf(1), "ManagerFailureTests.Capacity");
        if (!lease) {
            stopped = lease.Error();
            break;
        }

        REQUIRE(leases.TryPushBack(Move(lease.Value())));
    }

    constexpr uint32 UsedBeforeFill = 4;
    CHECK(stopped == ErrorCode::CapacityExceeded);
    CHECK(leases.Size() == AssetManagerMaxLiveHandles - UsedBeforeFill);
    CHECK(rig.manager->RequestAsset(RequestOf(1)).Error() == ErrorCode::CapacityExceeded);
    auto refusedRun = DeferredRun::TryCreate(env.allocator, [](const AssetLease&) noexcept {});
    auto refusedFail =
        DeferredFail::TryCreate(env.allocator, [](ErrorCode, const AssetDiagnostic&) noexcept {});
    REQUIRE(refusedRun && refusedFail);
    CHECK(rig.manager
              ->RunWhenReady(FixedId(1), NotReadyPolicy::Defer, nullptr, Move(refusedRun.Value()),
                             Move(refusedFail.Value()))
              .Error() == ErrorCode::CapacityExceeded);
    CHECK(rig.manager->LoadPrimaryAsset(FixedId(1), {}).Error() == ErrorCode::CapacityExceeded);

    REQUIRE(rig.Pump());
    CHECK(ready.calls == 1 && ready.error == ErrorCode::CapacityExceeded);
    CHECK(deferredError == ErrorCode::CapacityExceeded);
    CHECK(primaryError == ErrorCode::CapacityExceeded);

    // 하나를 놓으면 다음 Pump 뒤 다시 된다.
    REQUIRE(leases.TryPopBack());
    REQUIRE(rig.Pump());
    LoadTicket again = rig.Request(1);
    CHECK(again.IsValid());

    leases.Clear();
    first.Value() = AssetLease();
    ticket = LoadTicket();
    again = LoadTicket();
    deferred.Value() = DeferredCommand();
    primary.Value() = PrimaryAssetTicket();
    REQUIRE(rig.Pump());
    REQUIRE(rig.Pump());
    CHECK(rig.UnitCount() == 0 && !rig.IsLoaded(1));
}

} // namespace manager_failure_tests_file
