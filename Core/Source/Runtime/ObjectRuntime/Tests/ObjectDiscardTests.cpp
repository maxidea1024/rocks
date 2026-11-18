#include <Core/Memory/SystemAllocator.h>
#include <Object/ConstructionContext.h>
#include <Object/TypeRegistry.h>
#include <Platform/Fatal.h>
#include <Platform/Thread.h>
#include <Task/WorkerScheduler.h>
#include <TestSupport/HostFailure.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

// A0-Ob Discard·패키지 소속 보존 시험이다(Docs/Design/ObjectFoundations.ko.md 1.7·2절, 9절 Ob).
// 관찰자 기록은 ReferenceObserverTests.cpp, 무작위 대조는 ObjectRegistryModelTests.cpp·
// ReferenceObserverModelTests.cpp가 본다.
using namespace rocks;

namespace {

constexpr TypeId NodeType{0x4f42444e4f444531ULL, 1};
constexpr TypeId PackageType{0x4f4244504b474531ULL, 1};

struct DiscardNode final : Object {
    ObjectRef<Object> link;

    void Trace(TraceVisitor& visitor) const noexcept {
        visitor.Visit(link);
    }
};

struct DiscardPackage final : Object {};

// completeFieldLayout은 Trace가 모든 참조 멤버를 방문한다는 선언이다(패키지 정책 Registry의 생성
// 조건).
bool RegisterTypes(TypeRegistry& types, bool parallel = false) noexcept {
    auto node = MakeRuntimeTypeOps<DiscardNode>();
    node.parallelTraceSafe = parallel;
    node.completeFieldLayout = true;
    auto package = MakeRuntimeTypeOps<DiscardPackage>();
    package.parallelTraceSafe = parallel;
    package.completeFieldLayout = true;
    return types.Register({NodeType, 1, {}, {}}, Move(node)).IsOk() &&
           types.Register({PackageType, 1, {}, {}}, Move(package)).IsOk();
}

RegistryOptions PackagePolicy(bool assertRetired = true) noexcept {
    RegistryOptions options;
    options.policy.packageNativeType = NativeTypeKey<DiscardPackage>();
    options.assertRetiredWithoutDiscard = assertRetired;
    return options;
}

Result<ObjectHandle> NewNode(ObjectRegistry& registry, ObjectHandle outer, StringView name,
                             RootRef<Object>* root = nullptr) noexcept {
    auto made = registry.NewObject<Object>(registry.OwnerAccess(), NodeType, outer, name);
    if (!made) {
        return Result<ObjectHandle>::Failure(made.Error());
    }

    const ObjectHandle handle = made.Value().Handle();
    if (root != nullptr) {
        *root = Move(made.Value());
    }

    return Result<ObjectHandle>::Success(handle);
}

Result<ObjectHandle> NewPackage(ObjectRegistry& registry, RootRef<Object>& root) noexcept {
    auto made = registry.NewObject<Object>(registry.OwnerAccess(), PackageType);
    if (!made) {
        return Result<ObjectHandle>::Failure(made.Error());
    }

    root = Move(made.Value());
    return Result<ObjectHandle>::Success(root.Handle());
}

bool Is(ObjectRegistry& registry, ObjectHandle handle, ObjectLifecycle expected) noexcept {
    auto state = registry.InspectLifecycle(registry.OwnerAccess(), handle);
    return state && state.Value() == expected;
}

bool Alive(ObjectRegistry& registry, ObjectHandle handle) noexcept {
    return Is(registry, handle, ObjectLifecycle::Alive);
}

bool Requested(ObjectRegistry& registry, ObjectHandle handle) noexcept {
    return Is(registry, handle, ObjectLifecycle::DestroyRequested);
}

bool Reclaimed(ObjectRegistry& registry, ObjectHandle handle) noexcept {
    return registry.InspectLifecycle(registry.OwnerAccess(), handle).Error() ==
           ErrorCode::StaleHandle;
}

bool Finds(ObjectRegistry& registry, ObjectHandle outer, StringView name,
           ObjectHandle expected) noexcept {
    auto found = registry.FindChild(registry.OwnerAccess(), outer, name);
    return found && found.Value() == expected;
}

bool NotFound(ObjectRegistry& registry, ObjectHandle outer, StringView name) noexcept {
    return registry.FindChild(registry.OwnerAccess(), outer, name).Error() == ErrorCode::NotFound;
}

ErrorCode ErrorOf(const Result<void>& result) noexcept {
    return result ? ErrorCode::None : result.Error();
}

bool Finish(ObjectRegistry& registry, uint32 budget = 1,
            WorkerScheduler* workers = nullptr) noexcept {
    for (uint32 iteration = 0; iteration < 100000; ++iteration) {
        if (registry.CollectionPhase() == GcPhase::Idle) {
            return true;
        }

        if (!registry.StepCollection(registry.OwnerAccess(), {budget, 0}, workers)) {
            return false;
        }
    }

    return false;
}

bool StepToSweeping(ObjectRegistry& registry) noexcept {
    for (uint32 iteration = 0; iteration < 100000; ++iteration) {
        if (registry.CollectionPhase() == GcPhase::Sweeping) {
            return true;
        }

        if (!registry.StepCollection(registry.OwnerAccess(), {1, 0})) {
            return false;
        }
    }

    return false;
}

// mode 0: Collect, 1: 증분 Step, 2: worker 병렬 trace Step.
bool CollectBy(ObjectRegistry& registry, uint32 mode, WorkerScheduler* workers) noexcept {
    if (mode == 0) {
        return registry.Collect(registry.OwnerAccess()).IsOk();
    }

    return registry.BeginCollection(registry.OwnerAccess()).IsOk() &&
           Finish(registry, 1, mode == 2 ? workers : nullptr);
}

bool InnersAre(ObjectRegistry& registry, ObjectHandle outer, Span<const ObjectHandle> expected,
               Allocator& allocator) noexcept {
    Array<ObjectHandle> inners(allocator);
    if (!registry.CollectInners(registry.OwnerAccess(), outer, InnerOrder::DepthFirst, inners)) {
        return false;
    }

    if (inners.Size() != expected.Size()) {
        return false;
    }

    for (usize index = 0; index < expected.Size(); ++index) {
        if (!(inners[index] == expected[index])) {
            return false;
        }
    }

    return true;
}

} // namespace

// 부분 트리 전체가 DestroyRequested가 되고 이름 색인·고리에서 바로 빠진다(GC 전 같은 이름 재생성).
// 논리 정체성(GetName·GetObjectPath·GetOuter)은 회수 전까지 남는다.
TEST(ObjectDiscard_SubtreeLeavesTheNameIndexImmediately) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterTypes(types));
        ObjectRegistry registry(allocator, types, {0x4f6201, 1});
        const auto access = registry.OwnerAccess();
        RootRef<Object> roots[5];
        auto outer = NewNode(registry, {}, {}, &roots[0]);
        REQUIRE(outer);
        auto a = NewNode(registry, outer.Value(), "A", &roots[1]);
        REQUIRE(a);
        auto b = NewNode(registry, a.Value(), "B", &roots[2]);
        REQUIRE(b);
        auto c = NewNode(registry, a.Value(), "C");
        REQUIRE(c);
        auto d = NewNode(registry, outer.Value(), "D", &roots[3]);
        REQUIRE(d);
        CHECK(registry.Stats().namedObjects == 4);

        REQUIRE(registry.Discard(access, a.Value()));
        CHECK(Requested(registry, a.Value()));
        CHECK(Requested(registry, b.Value()));
        CHECK(Requested(registry, c.Value()));
        CHECK(Alive(registry, d.Value()));
        CHECK(registry.Stats().namedObjects == 1);
        CHECK(registry.Stats().destroyRequested == 3);
        CHECK(NotFound(registry, outer.Value(), "a"));
        CHECK(NotFound(registry, a.Value(), "B"));
        auto taken = registry.IsNameTaken(access, outer.Value(), "A");
        CHECK(taken && !taken.Value());
        const ObjectHandle onlyD[] = {d.Value()};
        CHECK(InnersAre(registry, outer.Value(), onlyD, allocator));

        // 논리 정체성은 남는다.
        String name(allocator);
        CHECK(registry.GetName(access, a.Value(), name) && name.View() == StringView("A"));
        String path(allocator);
        CHECK(registry.GetObjectPath(access, b.Value(), path) && path.View() == StringView("A.B"));
        auto outerOfB = registry.GetOuter(access, b.Value());
        CHECK(outerOfB && outerOfB.Value() == a.Value());
        // Discard된 객체는 Outer로 쓸 수 없다.
        CHECK(registry.DescribeOuter(access, a.Value()).Error() == ErrorCode::StaleHandle);
        CHECK(NewNode(registry, a.Value(), "E").Error() == ErrorCode::StaleHandle);
        CHECK(registry.Resolve(access, b.Value()).Error() == ErrorCode::Destroyed);
        CHECK(WeakObjectRef<Object>(b.Value()).TryPin(registry, access).Error() ==
              ErrorCode::Destroyed);

        // GC 전에도 같은 이름을 바로 만든다.
        auto again = NewNode(registry, outer.Value(), "a", &roots[4]);
        REQUIRE(again);
        CHECK(Finds(registry, outer.Value(), "A", again.Value()));

        // 다시 Discard는 무동작이다(부분 트리 안의 객체도).
        CHECK(registry.Discard(access, a.Value()));
        CHECK(registry.Discard(access, c.Value()));
        CHECK(registry.Stats().destroyRequested == 3);

        // root가 남은 B는 남고 그 Outer A도 inner → outer로 남는다. root 없는 C는 회수된다.
        roots[1].Reset();
        REQUIRE(registry.Collect(access));
        CHECK(Requested(registry, a.Value()));
        CHECK(Requested(registry, b.Value()));
        CHECK(Reclaimed(registry, c.Value()));
        roots[2].Reset();
        REQUIRE(registry.Collect(access));
        CHECK(Reclaimed(registry, a.Value()));
        CHECK(Reclaimed(registry, b.Value()));
        CHECK(registry.Discard(access, a.Value()).Error() == ErrorCode::StaleHandle);
        for (RootRef<Object>& root : roots) {
            root.Reset();
        }

        REQUIRE(registry.Shutdown(access));
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 패키지 정책: 남은 참조가 Discard된 객체를 붙잡으면 그 Outer 사슬(패키지까지)은
// DestroyRequested로 남고 형제는 고리에서 빠졌으므로 회수된다.
TEST(ObjectDiscard_RemainingReferenceKeepsOuterChainButNotSiblings) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterTypes(types));
        ObjectRegistry registry(allocator, types, {0x4f6202, 1}, PackagePolicy());
        const auto access = registry.OwnerAccess();
        RootRef<Object> packageRoot;
        RootRef<Object> otherRoot;
        auto package = NewPackage(registry, packageRoot);
        auto other = NewPackage(registry, otherRoot);
        REQUIRE(package && other);
        auto asset = NewNode(registry, package.Value(), {});
        REQUIRE(asset);
        auto x = NewNode(registry, asset.Value(), "X");
        auto y = NewNode(registry, asset.Value(), "Y");
        auto holder = NewNode(registry, other.Value(), {});
        REQUIRE(x && y && holder);
        {
            auto borrowed = registry.Resolve<DiscardNode>(access, holder.Value());
            REQUIRE(borrowed);
            auto pointer = borrowed.Value().Get();
            REQUIRE(pointer);
            REQUIRE(
                registry.SetReference(access, holder.Value(), pointer.Value()->link, x.Value()));
        }

        // 패키지 RootRef만으로 모든 소속 객체가 산다(export 목록 없이).
        REQUIRE(registry.Collect(access));
        CHECK(Alive(registry, asset.Value()) && Alive(registry, x.Value()) &&
              Alive(registry, y.Value()));

        REQUIRE(registry.Discard(access, package.Value()));
        packageRoot.Reset();
        REQUIRE(registry.Collect(access));
        CHECK(Requested(registry, package.Value()));
        CHECK(Requested(registry, asset.Value()));
        CHECK(Requested(registry, x.Value()));
        CHECK(Reclaimed(registry, y.Value()));
        CHECK(registry.Stats().retiredWithoutDiscard == 0);

        {
            auto borrowed = registry.Resolve<DiscardNode>(access, holder.Value());
            REQUIRE(borrowed);
            auto pointer = borrowed.Value().Get();
            REQUIRE(pointer);
            REQUIRE(registry.SetReference(access, holder.Value(), pointer.Value()->link, {}));
        }

        REQUIRE(registry.Collect(access));
        CHECK(Reclaimed(registry, package.Value()));
        CHECK(Reclaimed(registry, asset.Value()));
        CHECK(Reclaimed(registry, x.Value()));
        REQUIRE(registry.Discard(access, other.Value()));
        otherRoot.Reset();
        REQUIRE(registry.Collect(access));
        CHECK(Reclaimed(registry, holder.Value()));
        CHECK(registry.Stats().retiredWithoutDiscard == 0);
        CHECK(registry.Stats().liveObjects == 0 && registry.Stats().destroyRequested == 0);
        REQUIRE(registry.Shutdown(access));
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

namespace {

struct ForeignDiscard {
    ObjectRegistry* registry;
    ObjectHandle target;
    ErrorCode error = ErrorCode::None;
};

void DiscardFromForeignThread(void* raw) noexcept {
    auto& call = *static_cast<ForeignDiscard*>(raw);
    call.error = ErrorOf(call.registry->Discard(call.registry->OwnerAccess(), call.target));
}

// 지정한 tag의 할당 중에 hook을 한 번 부른다(처리 표시 중 Registry 재진입).
class HookAllocator final : public Allocator {
public:
    explicit HookAllocator(Allocator& base) noexcept : base_(base) {
    }

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override {
        if (hook != nullptr && (hookTag == 0 || tag == hookTag)) {
            auto run = hook;
            hook = nullptr;
            run(context);
        }

        return base_.Allocate(bytes, alignment, tag);
    }

    Result<void> Deallocate(void* address) noexcept override {
        return base_.Deallocate(address);
    }

    void (*hook)(void*) noexcept = nullptr;
    void* context = nullptr;
    uint32 hookTag = 0;

private:
    Allocator& base_;
};

struct ReentrantDiscard {
    ObjectRegistry* registry;
    ObjectHandle target;
    ErrorCode error = ErrorCode::None;
};

void DiscardFromCallback(void* raw) noexcept {
    auto& call = *static_cast<ReentrantDiscard*>(raw);
    call.error = ErrorOf(call.registry->Discard(call.registry->OwnerAccess(), call.target));
}

} // namespace

// 오류 순서: 접근 → 종료·재진입 → handle(StaleHandle·이미 Discard됨은 성공·Destroyed) → 부분 트리의
// Reserved(Busy). RequestDestroy만 받은 객체도 받는다.
TEST(ObjectDiscard_ErrorOrderAndAcceptedStates) {
    SystemAllocator system;
    HookAllocator allocator(system);
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterTypes(types));
        ObjectRegistry registry(allocator, types, {0x4f6203, 1});
        ObjectRegistry otherRegistry(allocator, types, {0x4f6203, 2});
        const auto access = registry.OwnerAccess();
        RootRef<Object> roots[4];
        auto outer = NewNode(registry, {}, {}, &roots[0]);
        REQUIRE(outer);
        auto child = NewNode(registry, outer.Value(), "Child", &roots[1]);
        REQUIRE(child);
        RootRef<Object> foreignRoot;
        auto foreign = NewNode(otherRegistry, {}, {}, &foreignRoot);
        REQUIRE(foreign);

        // 접근: 다른 스레드 WrongThread, 다른 문맥 WrongContext.
        ForeignDiscard call{&registry, child.Value()};
        platform::Thread thread;
        if (thread.Start(DiscardFromForeignThread, &call) != platform::ThreadStatus::Success ||
            thread.WaitForCompletion(10000) != platform::WaitResult::Signaled ||
            thread.Join() != platform::ThreadStatus::Success) {
            platform::Fatal("Discard test worker did not finish");
        }

        CHECK(call.error == ErrorCode::WrongThread);
        CHECK(registry.Discard(otherRegistry.OwnerAccess(), child.Value()).Error() ==
              ErrorCode::WrongContext);

        // handle: null·다른 Registry·공개 전 객체는 StaleHandle.
        CHECK(registry.Discard(access, {}).Error() == ErrorCode::StaleHandle);
        CHECK(registry.Discard(access, foreign.Value()).Error() == ErrorCode::StaleHandle);
        {
            ConstructionContext transaction(registry, access);
            auto pending = transaction.Create<DiscardNode>(NodeType, child.Value(), "Pending");
            REQUIRE(pending);
            CHECK(registry.Discard(access, pending.Value()).Error() == ErrorCode::StaleHandle);
            // 부분 트리에 공개 전 객체가 있으면 Busy다(그 트랜잭션의 Commit이 실패할 상태를 만들지
            // 않는다).
            CHECK(registry.Discard(access, child.Value()).Error() == ErrorCode::Busy);
            CHECK(registry.Discard(access, outer.Value()).Error() == ErrorCode::Busy);
            CHECK(Alive(registry, child.Value()));
        }

        // 재진입: 생성의 처리 표시 중 allocator 콜백의 Discard는 Busy다.
        ReentrantDiscard reentrant{&registry, child.Value()};
        allocator.hook = &DiscardFromCallback;
        allocator.context = &reentrant;
        allocator.hookTag = 0x4f424a45u; // 객체 저장소 할당
        REQUIRE(NewNode(registry, {}, {}, &roots[2]));
        CHECK(reentrant.error == ErrorCode::Busy);
        CHECK(Alive(registry, child.Value()));

        // RequestDestroy만 받은 객체도 받는다(이름은 이때 빠진다).
        REQUIRE(registry.RequestDestroy(access, child.Value()));
        CHECK(Finds(registry, outer.Value(), "Child", child.Value()));
        REQUIRE(registry.Discard(access, child.Value()));
        CHECK(NotFound(registry, outer.Value(), "child"));
        CHECK(Requested(registry, child.Value()));

        // cutoff white는 Destroyed, Discard된 뒤 cutoff·Retire된 객체는 무동작 성공이다.
        auto white = NewNode(registry, {}, "White");
        REQUIRE(white);
        auto discardedWhite = NewNode(registry, {}, "Gone");
        REQUIRE(discardedWhite);
        REQUIRE(registry.Discard(access, discardedWhite.Value()));
        roots[1].Reset();
        REQUIRE(registry.BeginCollection(access));
        REQUIRE(StepToSweeping(registry));
        CHECK(Is(registry, white.Value(), ObjectLifecycle::Unreachable));
        CHECK(registry.Discard(access, white.Value()).Error() == ErrorCode::Destroyed);
        CHECK(registry.Discard(access, discardedWhite.Value()));
        REQUIRE(Finish(registry));

        // Retired(BackendLease로 회수 지연)도 Destroyed, Discard된 Retired는 성공이다.
        auto retired = NewNode(registry, {}, {});
        auto retiredDiscarded = NewNode(registry, {}, {});
        REQUIRE(retired && retiredDiscarded);
        auto lease = registry.AcquireBackendLease(access, retired.Value());
        auto leaseDiscarded = registry.AcquireBackendLease(access, retiredDiscarded.Value());
        REQUIRE(lease && leaseDiscarded);
        REQUIRE(registry.Discard(access, retiredDiscarded.Value()));
        REQUIRE(registry.Collect(access));
        CHECK(Is(registry, retired.Value(), ObjectLifecycle::Retired));
        CHECK(registry.Discard(access, retired.Value()).Error() == ErrorCode::Destroyed);
        CHECK(registry.Discard(access, retiredDiscarded.Value()));
        lease.Value().Reset();
        leaseDiscarded.Value().Reset();

        // 종료 뒤 ShuttingDown.
        for (RootRef<Object>& root : roots) {
            root.Reset();
        }

        foreignRoot.Reset();
        REQUIRE(registry.Shutdown(access));
        CHECK(registry.Discard(access, outer.Value()).Error() == ErrorCode::ShuttingDown);
        REQUIRE(otherRegistry.Shutdown(otherRegistry.OwnerAccess()));
        REQUIRE(types.Shutdown());
    }

    CHECK(system.Stats().liveAllocations == 0);
}

// Discard는 GC 단계와 무관하다: Marking 중 고리에서 뺀 소속 객체는 다른 참조가 없으면 그 cycle에
// 회수되고, Sweeping 중 Discard한(표식된) 객체는 다음 cycle에 회수된다.
TEST(ObjectDiscard_DuringMarkingAndSweeping) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterTypes(types));
        ObjectRegistry registry(allocator, types, {0x4f6204, 1}, PackagePolicy());
        const auto access = registry.OwnerAccess();
        RootRef<Object> packageRoot;
        auto package = NewPackage(registry, packageRoot);
        REQUIRE(package);
        auto asset = NewNode(registry, package.Value(), {});
        REQUIRE(asset);
        ObjectHandle children[4];
        const char* const names[] = {"A", "B", "C", "D"};
        for (uint32 index = 0; index < 4; ++index) {
            auto made = NewNode(registry, asset.Value(), names[index]);
            REQUIRE(made);
            children[index] = made.Value();
        }

        // Marking: 첫 slice(패키지 표식) 뒤 Discard, 새 자식은 Commit이 shade한다.
        REQUIRE(registry.BeginCollection(access));
        REQUIRE(registry.StepCollection(access, {1, 0}));
        CHECK(registry.CollectionPhase() == GcPhase::Marking);
        REQUIRE(registry.Discard(access, children[0]));
        auto born = NewNode(registry, asset.Value(), "Born");
        REQUIRE(born);
        REQUIRE(Finish(registry));
        CHECK(Reclaimed(registry, children[0]));
        CHECK(Alive(registry, born.Value()));
        CHECK(Alive(registry, children[1]));

        // Sweeping: 표식된 객체의 Discard는 이번 cycle에는 회수되지 않는다.
        REQUIRE(registry.BeginCollection(access));
        REQUIRE(StepToSweeping(registry));
        REQUIRE(registry.Discard(access, children[1]));
        auto late = NewNode(registry, asset.Value(), "Late");
        REQUIRE(late);
        CHECK(NotFound(registry, asset.Value(), "B"));
        REQUIRE(Finish(registry));
        CHECK(Requested(registry, children[1]));
        CHECK(Alive(registry, late.Value()));
        REQUIRE(registry.Collect(access));
        CHECK(Reclaimed(registry, children[1]));
        CHECK(Alive(registry, children[2]) && Alive(registry, children[3]));
        CHECK(registry.Stats().retiredWithoutDiscard == 0);

        REQUIRE(registry.Discard(access, package.Value()));
        packageRoot.Reset();
        REQUIRE(registry.Collect(access));
        CHECK(registry.Stats().liveObjects == 0 && registry.Stats().destroyRequested == 0);
        CHECK(registry.Stats().retiredWithoutDiscard == 0);
        REQUIRE(registry.Shutdown(access));
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 할당 실패 스윕: 부분 트리 목록·간선 집계(Registry allocator)·관찰자 예약(관찰자 allocator)의
// 모든 지점이 실패해도 상태·색인·고리·관찰자가 그대로다.
TEST(ObjectDiscard_AllocationFailureSweepLeavesEverythingUnchanged) {
    SystemAllocator system;
    test::NFailAllocator allocator(system);
    {
        TypeRegistry types(system);
        REQUIRE(RegisterTypes(types));
        ReferenceObserver observer(allocator);
        RegistryOptions options = PackagePolicy();
        options.referenceObserver = &observer;
        ObjectRegistry registry(allocator, types, {0x4f6205, 1}, options);
        const auto access = registry.OwnerAccess();
        RootRef<Object> roots[3];
        auto package = NewPackage(registry, roots[0]);
        auto target = NewPackage(registry, roots[1]);
        auto third = NewPackage(registry, roots[2]);
        REQUIRE(package && target && third);
        auto asset = NewNode(registry, package.Value(), {});
        auto remote = NewNode(registry, target.Value(), {});
        auto remote2 = NewNode(registry, third.Value(), {});
        REQUIRE(asset && remote && remote2);
        ObjectHandle members[6];
        for (uint32 index = 0; index < 6; ++index) {
            char name[2] = {static_cast<char>('A' + index), 0};
            auto made = NewNode(registry, asset.Value(), name);
            REQUIRE(made);
            members[index] = made.Value();
            auto borrowed = registry.Resolve<DiscardNode>(access, made.Value());
            REQUIRE(borrowed);
            auto pointer = borrowed.Value().Get();
            REQUIRE(pointer);
            REQUIRE(registry.SetReference(access, made.Value(), pointer.Value()->link,
                                          index % 2 == 0 ? remote.Value() : remote2.Value()));
        }

        // 관찰자에 남은 공간이 없게 비운다(Discard가 예약해야 한다).
        Array<ReferenceEdgeDelta> taken(allocator);
        REQUIRE(observer.TakeChanges(taken));
        const ObjectStats before = registry.Stats();
        const ObjectHandle exports[] = {asset.Value(), members[0], members[1], members[2],
                                        members[3],    members[4], members[5]};
        uint64 failed = 0;
        for (uint64 attempt = 1; attempt < 64; ++attempt) {
            allocator.FailOnAttempt(attempt);
            auto discarded = registry.Discard(access, asset.Value());
            allocator.DisableFailure();
            if (discarded) {
                break;
            }

            ++failed;
            CHECK(discarded.Error() == ErrorCode::OutOfMemory);
            CHECK(Alive(registry, asset.Value()));
            CHECK(Finds(registry, package.Value(), {}, asset.Value()));
            CHECK(Finds(registry, asset.Value(), "C", members[2]));
            CHECK(InnersAre(registry, package.Value(), exports, system));
            CHECK(observer.PendingCount() == 0);
            const ObjectStats now = registry.Stats();
            CHECK(now.namedObjects == before.namedObjects);
            CHECK(now.liveObjects == before.liveObjects && now.destroyRequested == 0);
        }

        // 목록 1 + 집계(색인·쌍 배열) + 관찰자(항목 배열·색인)가 모두 실패 지점이었다.
        CHECK(failed >= 5);
        CHECK(Requested(registry, asset.Value()));
        CHECK(NotFound(registry, package.Value(), {}));
        Array<ReferenceEdgeDelta> changes(allocator);
        REQUIRE(observer.TakeChanges(changes));
        REQUIRE(changes.Size() == 2);
        CHECK(changes[0].cause == ReferenceChangeCause::Discard && changes[0].delta == -3);
        CHECK(changes[0].targetPackage == target.Value());
        CHECK(changes[1].targetPackage == third.Value() && changes[1].delta == -3);
        REQUIRE(registry.Discard(access, package.Value()));
        REQUIRE(registry.Discard(access, target.Value()));
        REQUIRE(registry.Discard(access, third.Value()));
        for (RootRef<Object>& root : roots) {
            root.Reset();
        }

        REQUIRE(registry.Shutdown(access));
        REQUIRE(types.Shutdown());
    }

    CHECK(system.Stats().liveAllocations == 0);
}

namespace {

struct NameReentry {
    ObjectRegistry* registry;
    RootRef<Object>* root;
    ObjectHandle target;
    bool discarded = false;
    bool collected = false;
};

void DiscardAndCollect(void* raw) noexcept {
    auto& state = *static_cast<NameReentry*>(raw);
    const auto access = state.registry->OwnerAccess();
    state.discarded = state.registry->Discard(access, state.target).IsOk();
    state.root->Reset();
    state.collected = state.registry->Collect(access).IsOk();
}

} // namespace

// GetName·GetObjectPath의 out allocator 콜백이 그 객체를 Discard하고 회수해도 복사해 둔 이름을
// 붙인다(해제된 이름 기록을 읽지 않는다 — Asan으로 확인).
TEST(ObjectDiscard_ReentrantNameQueryCallbackDiscardsAndReclaims) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterTypes(types));
        ObjectRegistry registry(allocator, types, {0x4f6206, 1});
        const auto access = registry.OwnerAccess();
        for (uint32 query = 0; query < 2; ++query) {
            RootRef<Object> outerRoot;
            RootRef<Object> targetRoot;
            auto outer = NewNode(registry, {}, {}, &outerRoot);
            REQUIRE(outer);
            auto target = NewNode(registry, outer.Value(), "Doomed", &targetRoot);
            REQUIRE(target);
            NameReentry state{&registry, &targetRoot, target.Value()};
            HookAllocator hooked(allocator);
            hooked.hook = &DiscardAndCollect;
            hooked.context = &state;
            String out(hooked);
            const Result<void> got = query == 0
                                         ? registry.GetName(access, target.Value(), out)
                                         : registry.GetObjectPath(access, target.Value(), out);
            CHECK(got.IsOk());
            CHECK(out.View() == StringView("Doomed"));
            CHECK(state.discarded && state.collected);
            CHECK(Reclaimed(registry, target.Value()));
            CHECK(NotFound(registry, outer.Value(), "Doomed"));
        }

        REQUIRE(registry.Shutdown(access));
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 패키지 정책(결정 8): 패키지 RootRef만 있어도 export 목록 없이 모든 소속 객체가 산다(Collect·증분·
// 병렬). 패키지 Discard 뒤에는 전부 회수된다. 정책 없는 Registry는 outer → inner를 방문하지 않는다.
TEST(PackagePolicy_PackageRootKeepsMembersAliveAcrossCollectors) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterTypes(types, true));
        WorkerScheduler scheduler(allocator);
        REQUIRE(scheduler.Start({4, 256, 64}));
        ObjectRegistry registry(allocator, types, {0x4f6207, 1}, PackagePolicy());
        ObjectRegistry plain(allocator, types, {0x4f6207, 2});
        for (uint32 mode = 0; mode < 3; ++mode) {
            RootRef<Object> packageRoot;
            auto package = NewPackage(registry, packageRoot);
            REQUIRE(package);
            auto asset = NewNode(registry, package.Value(), {});
            REQUIRE(asset);
            auto a = NewNode(registry, asset.Value(), "A");
            REQUIRE(a);
            auto b = NewNode(registry, a.Value(), "B");
            REQUIRE(b);
            REQUIRE(CollectBy(registry, mode, &scheduler));
            CHECK(Alive(registry, asset.Value()) && Alive(registry, a.Value()) &&
                  Alive(registry, b.Value()));
            const ObjectHandle exports[] = {asset.Value(), a.Value(), b.Value()};
            CHECK(InnersAre(registry, package.Value(), exports, allocator));

            REQUIRE(registry.Discard(registry.OwnerAccess(), package.Value()));
            packageRoot.Reset();
            REQUIRE(CollectBy(registry, mode, &scheduler));
            CHECK(Reclaimed(registry, package.Value()) && Reclaimed(registry, asset.Value()) &&
                  Reclaimed(registry, a.Value()) && Reclaimed(registry, b.Value()));

            // 정책 없는 Registry: outer만 root면 inner는 회수된다.
            RootRef<Object> outerRoot;
            auto outer = NewNode(plain, {}, {}, &outerRoot);
            REQUIRE(outer);
            auto inner = NewNode(plain, outer.Value(), "Inner");
            REQUIRE(inner);
            REQUIRE(CollectBy(plain, mode, &scheduler));
            CHECK(Alive(plain, outer.Value()));
            CHECK(Reclaimed(plain, inner.Value()));
        }

        CHECK(registry.Stats().retiredWithoutDiscard == 0);
        REQUIRE(registry.Shutdown(registry.OwnerAccess()));
        REQUIRE(plain.Shutdown(plain.OwnerAccess()));
        REQUIRE(scheduler.Shutdown());
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 부분 Discard한 객체(와 그 부분 트리)만 회수되고 export 순서에서 빠진다. 같은 이름을 다시 만든다.
TEST(PackagePolicy_PartialDiscardReclaimsOnlyThatSubtree) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterTypes(types));
        ObjectRegistry registry(allocator, types, {0x4f6208, 1}, PackagePolicy());
        const auto access = registry.OwnerAccess();
        RootRef<Object> packageRoot;
        auto package = NewPackage(registry, packageRoot);
        REQUIRE(package);
        auto asset = NewNode(registry, package.Value(), {});
        REQUIRE(asset);
        auto a = NewNode(registry, asset.Value(), "A");
        auto b = NewNode(registry, asset.Value(), "B");
        REQUIRE(a && b);
        auto a1 = NewNode(registry, a.Value(), "A1");
        REQUIRE(a1);
        REQUIRE(registry.Discard(access, a.Value()));
        const ObjectHandle remaining[] = {asset.Value(), b.Value()};
        CHECK(InnersAre(registry, package.Value(), remaining, allocator));
        REQUIRE(registry.Collect(access));
        CHECK(Reclaimed(registry, a.Value()) && Reclaimed(registry, a1.Value()));
        CHECK(Alive(registry, asset.Value()) && Alive(registry, b.Value()));
        auto again = NewNode(registry, asset.Value(), "a");
        REQUIRE(again);
        REQUIRE(registry.Collect(access));
        CHECK(Alive(registry, again.Value()));
        CHECK(registry.Stats().retiredWithoutDiscard == 0);
        REQUIRE(registry.Discard(access, package.Value()));
        packageRoot.Reset();
        REQUIRE(registry.Shutdown(access));
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// RootRef를 Discard 없이 놓으면 retiredWithoutDiscard가 오른다(패키지 포함 — assert를 끈 Registry).
// 종료(Shutdown) 뒤의 회수는 세지 않는다.
TEST(PackagePolicy_RetiredWithoutDiscardIsCountedUntilShutdown) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterTypes(types));
        ObjectRegistry registry(allocator, types, {0x4f6209, 1}, PackagePolicy(false));
        const auto access = registry.OwnerAccess();
        {
            RootRef<Object> packageRoot;
            auto package = NewPackage(registry, packageRoot);
            REQUIRE(package);
            auto asset = NewNode(registry, package.Value(), {});
            REQUIRE(asset);
            REQUIRE(NewNode(registry, asset.Value(), "A"));
            REQUIRE(NewNode(registry, asset.Value(), "B"));
        }

        REQUIRE(registry.Collect(access));
        CHECK(registry.Stats().retiredWithoutDiscard == 4);
        CHECK(registry.Stats().liveObjects == 0);
        // Discard한 객체는 세지 않는다.
        {
            RootRef<Object> packageRoot;
            auto package = NewPackage(registry, packageRoot);
            REQUIRE(package);
            REQUIRE(NewNode(registry, package.Value(), {}));
            REQUIRE(registry.Discard(access, package.Value()));
        }

        REQUIRE(registry.Collect(access));
        CHECK(registry.Stats().retiredWithoutDiscard == 4);
        // 종료 뒤의 회수는 세지 않는다.
        RootRef<Object> lastRoot;
        auto last = NewPackage(registry, lastRoot);
        REQUIRE(last);
        REQUIRE(NewNode(registry, last.Value(), {}));
        lastRoot.Reset();
        REQUIRE(registry.Shutdown(access));
        CHECK(registry.Stats().retiredWithoutDiscard == 4);
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 패키지 정책 Registry의 RequestDestroy·Object::Destroy는 관찰자 유무와 무관하게 Unsupported다(접근
// 검사 다음).
TEST(PackagePolicy_RequestDestroyAndObjectDestroyAreUnsupported) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterTypes(types));
        for (uint32 withObserver = 0; withObserver < 2; ++withObserver) {
            ReferenceObserver observer(allocator);
            RegistryOptions options = PackagePolicy();
            options.referenceObserver = withObserver != 0 ? &observer : nullptr;
            ObjectRegistry registry(allocator, types, {0x4f620a, withObserver}, options);
            ObjectRegistry other(allocator, types, {0x4f620a, 9 + withObserver});
            const auto access = registry.OwnerAccess();
            RootRef<Object> packageRoot;
            auto package = NewPackage(registry, packageRoot);
            REQUIRE(package);
            auto asset = NewNode(registry, package.Value(), {});
            REQUIRE(asset);
            CHECK(registry.RequestDestroy(access, asset.Value()).Error() == ErrorCode::Unsupported);
            // 접근 검사 다음이고 handle·종료보다 먼저다(정적인 Registry 성질).
            CHECK(registry.RequestDestroy(other.OwnerAccess(), asset.Value()).Error() ==
                  ErrorCode::WrongContext);
            CHECK(registry.RequestDestroy(access, {}).Error() == ErrorCode::Unsupported);
            {
                auto borrowed = registry.Resolve<DiscardNode>(access, asset.Value());
                REQUIRE(borrowed);
                auto pointer = borrowed.Value().Get();
                REQUIRE(pointer);
                CHECK(pointer.Value()->Destroy().Error() == ErrorCode::Unsupported);
            }

            CHECK(Alive(registry, asset.Value()));
            REQUIRE(registry.Discard(access, package.Value()));
            CHECK(Requested(registry, asset.Value()));
            packageRoot.Reset();
            REQUIRE(registry.Collect(access));
            CHECK(Reclaimed(registry, asset.Value()));
            REQUIRE(registry.Shutdown(access));
            CHECK(registry.RequestDestroy(access, asset.Value()).Error() == ErrorCode::Unsupported);
            REQUIRE(other.Shutdown(other.OwnerAccess()));
        }

        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

namespace {

constexpr TypeId LooseNodeType{0x4f42444c4f4f5331ULL, 1};
constexpr TypeId LoosePackageType{0x4f42444c4f4f5332ULL, 1};

// Trace가 참조 멤버를 방문하지 않는다(MakeRuntimeTypeOps는 Trace 없는 타입을 강한 간선 없음으로
// 본다). 완전한 필드 배치를 선언하지 않았다.
struct LooseNode final : Object {
    ObjectRef<Object> link;
};

struct LoosePackage final : Object {};

} // namespace

// 패키지 정책 Registry는 완전한 필드 배치(completeFieldLayout)가 아닌 타입을 만들지 않는다(생성
// 오류 순서 4 — Unsupported). Trace가 덮지 않는 참조를 Registry API로 써서 관찰자 계수가 새는
// 경로를 생성에서 막는다. 정책 없는 Registry는 지금처럼 만든다.
TEST(PackagePolicy_IncompleteFieldLayoutTypesAreUnsupported) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterTypes(types));
        REQUIRE(types.Register({LooseNodeType, 1, {}, {}}, MakeRuntimeTypeOps<LooseNode>()));
        REQUIRE(types.Register({LoosePackageType, 1, {}, {}}, MakeRuntimeTypeOps<LoosePackage>()));
        ReferenceObserver observer(allocator);
        RegistryOptions options = PackagePolicy();
        options.referenceObserver = &observer;
        ObjectRegistry registry(allocator, types, {0x4f620c, 1}, options);
        ObjectRegistry plain(allocator, types, {0x4f620c, 2});
        // 패키지 타입 자신도 같은 규칙이다.
        RegistryOptions looseOptions;
        looseOptions.policy.packageNativeType = NativeTypeKey<LoosePackage>();
        ObjectRegistry loosePackages(allocator, types, {0x4f620c, 3}, looseOptions);
        auto loosePackage =
            loosePackages.NewObject<LoosePackage>(loosePackages.OwnerAccess(), LoosePackageType);
        CHECK(!loosePackage && loosePackage.Error() == ErrorCode::Unsupported);
        const auto access = registry.OwnerAccess();
        RootRef<Object> packageRoot;
        auto package = NewPackage(registry, packageRoot);
        REQUIRE(package);
        auto loose = registry.NewObject<LooseNode>(access, LooseNodeType, package.Value(), {});
        CHECK(!loose && loose.Error() == ErrorCode::Unsupported);
        {
            ConstructionContext transaction(registry, access);
            auto created = transaction.Create<LooseNode>(LooseNodeType, package.Value());
            CHECK(!created && created.Error() == ErrorCode::Unsupported);
        }

        // 타입 단계(3)의 TypeMismatch가 먼저다.
        auto mismatch = registry.NewObject<DiscardNode>(access, LooseNodeType, package.Value(), {});
        CHECK(!mismatch && mismatch.Error() == ErrorCode::TypeMismatch);
        CHECK(registry.Stats().liveObjects == 1);

        // 정책 없는 Registry는 완전한 필드 배치를 요구하지 않는다.
        auto unrestricted = plain.NewObject<LooseNode>(plain.OwnerAccess(), LooseNodeType);
        CHECK(unrestricted.IsOk());
        if (unrestricted) {
            unrestricted.Value().Reset();
        }

        REQUIRE(registry.Discard(access, package.Value()));
        packageRoot.Reset();
        REQUIRE(registry.Shutdown(access));
        REQUIRE(plain.Shutdown(plain.OwnerAccess()));
        REQUIRE(loosePackages.Shutdown(loosePackages.OwnerAccess()));
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

namespace rocks::test {

void RegisterObjectObserverNegativeControl(int (*function)(uint32) noexcept) noexcept;

// 전용 child process에서 실행한다. Fatal이 없으면 0으로 돌아가 도구 시험이 실패한다.
// 0: 정책 없는 Registry에 관찰자, 1: 이미 붙은 관찰자, 2: 붙은 채 관찰자 소멸.
int RunObjectObserverNegativeControl(uint32 operation) noexcept {
    PrepareFatalTestProcess();
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    if (!RegisterTypes(types)) {
        return 0;
    }

    if (operation == 0) {
        ReferenceObserver observer(allocator);
        RegistryOptions options;
        options.referenceObserver = &observer;
        ObjectRegistry registry(allocator, types, {0x4f620b, 1}, options);
        return 0;
    }

    if (operation == 1) {
        ReferenceObserver observer(allocator);
        RegistryOptions options = PackagePolicy();
        options.referenceObserver = &observer;
        ObjectRegistry first(allocator, types, {0x4f620b, 2}, options);
        ObjectRegistry second(allocator, types, {0x4f620b, 3}, options);
        return 0;
    }

    alignas(ReferenceObserver) unsigned char storage[sizeof(ReferenceObserver)];
    auto* observer = ::new (storage) ReferenceObserver(allocator);
    RegistryOptions options = PackagePolicy();
    options.referenceObserver = observer;
    ObjectRegistry registry(allocator, types, {0x4f620b, 4}, options);
    observer->~ReferenceObserver();
    return 0;
}

namespace {

struct ObjectObserverNegativeRegistration {
    ObjectObserverNegativeRegistration() noexcept {
        RegisterObjectObserverNegativeControl(&RunObjectObserverNegativeControl);
    }
};

ObjectObserverNegativeRegistration objectObserverNegativeRegistration;

} // namespace

} // namespace rocks::test
