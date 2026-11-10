#include <Tests/Support/Test.h>
#include <Tests/Support/HostFailure.h>
#include <Core/Memory/SystemAllocator.h>
#include <Object/ConstructionContext.h>
#include <Object/ObjectRegistry.h>
#include <Platform/Fatal.h>
#include <Platform/Thread.h>
#include <Task/WorkerScheduler.h>
#include <new>

using namespace rocks;

namespace {

constexpr Guid LifecycleType{0x4c49464553544154ULL, 1};
static_assert(!__is_constructible(ObjectAccessScope, const ObjectAccessScope&));
static_assert(!__is_constructible(ObjectAccessScope, ObjectAccessScope&&));
ObjectRegistry* callbackRegistry = nullptr;
ErrorCode constructorQuery = ErrorCode::None;
ErrorCode constructorDestroy = ErrorCode::None;
ErrorCode destructorQuery = ErrorCode::None;
ErrorCode destructorDestroy = ErrorCode::None;
ErrorCode traceQuery = ErrorCode::None;
uint32 destructions = 0;
bool inspectTrace = false;

struct ResetCallbacks {
    ~ResetCallbacks() noexcept {
        callbackRegistry = nullptr;
        inspectTrace = false;
    }
};

struct LifecycleProbe final : Object {
    LifecycleProbe() noexcept {
        if (callbackRegistry) {
            constructorQuery = InspectLifecycle().Error();
            constructorDestroy = Destroy().Error();
        }
    }

    ~LifecycleProbe() noexcept {
        ++destructions;
        if (callbackRegistry) {
            destructorQuery = InspectLifecycle().Error();
            destructorDestroy = Destroy().Error();
        }
    }

    void Trace(TraceVisitor&) const noexcept {
        if (callbackRegistry && inspectTrace) {
            traceQuery = InspectLifecycle().Error();
        }
    }
};

struct ForeignQuery {
    const ObjectRegistry* registry;
    ObjectAccessContext access;
    ObjectHandle handle;
    const Borrow<LifecycleProbe>* borrow;
    ErrorCode result = ErrorCode::None;
    ErrorCode borrowResult = ErrorCode::None;
    ErrorCode scopedBorrowResult = ErrorCode::None;
    ErrorCode scopeResult = ErrorCode::None;
};

void QueryOnForeignThread(void* raw) noexcept {
    auto& query = *static_cast<ForeignQuery*>(raw);
    query.result = query.registry->InspectLifecycle(query.access, query.handle).Error();
    query.borrowResult = query.borrow->InspectLifecycle().Error();
    query.scopeResult = ObjectAccessScope::Current().Error();
    {
        ObjectAccessScope foreignScope(query.access);
        query.scopedBorrowResult = query.borrow->InspectLifecycle().Error();
    }
}

bool FinishCollection(ObjectRegistry& registry, const ObjectAccessContext& access) noexcept {
    for (uint32 index = 0; index < 1000; ++index) {
        if (registry.CollectionPhase() == GcPhase::Idle) {
            return true;
        }
        if (!registry.StepCollection(access, {16, 0})) {
            return false;
        }
    }
    return false;
}

} // namespace

namespace rocks::test {

void RegisterObjectScopeNegativeControl(int (*function)(bool) noexcept) noexcept;

// 이 함수는 오직 외부 watchdog이 감시하는 별도 child에서 호출한다. 정상 실행이면 Fatal이다.
static void DestroyScopeOnForeignThread(void* raw) noexcept {
    static_cast<ObjectAccessScope*>(raw)->~ObjectAccessScope();
}

int RunObjectScopeNegativeControl(bool foreignThread) noexcept {
    PrepareFatalTestProcess();
    alignas(ObjectAccessScope) unsigned char firstStorage[sizeof(ObjectAccessScope)];
    auto* first = ::new (firstStorage) ObjectAccessScope({{305, 1}});
    if (foreignThread) {
        platform::Thread worker;
        if (worker.Start(DestroyScopeOnForeignThread, first) != platform::ThreadStatus::Success ||
            worker.WaitForCompletion(10000) != platform::WaitResult::Signaled ||
            worker.Join() != platform::ThreadStatus::Success) {
            return 3;
        }
        return 3; // Fatal이 발생하지 않으면 음성 대조 실패다. 파괴된 포인터를 다시 쓰지 않는다.
    }
    alignas(ObjectAccessScope) unsigned char secondStorage[sizeof(ObjectAccessScope)];
    auto* second = ::new (secondStorage) ObjectAccessScope({{305, 2}});
    (void)second;
    first->~ObjectAccessScope(); // 안쪽 scope보다 먼저 파괴하면 Fatal이어야 한다.
    return 3;
}

namespace {
struct ScopeNegativeRegistration {
    ScopeNegativeRegistration() noexcept {
        RegisterObjectScopeNegativeControl(&RunObjectScopeNegativeControl);
    }
};
ScopeNegativeRegistration scopeNegativeRegistration;
} // namespace

} // namespace rocks::test

TEST(ObjectLifecycleAccessDestroyLeasesAndGeneration) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(types.Register({LifecycleType, 1, {}, {}}, MakeRuntimeTypeOps<LifecycleProbe>()));
        ObjectRegistry registry(allocator, types, {301, 1}, {1, 4});
        ObjectRegistry other(allocator, types, {301, 1});
        const auto access = registry.OwnerAccess();
        const ObjectAccessContext wrong{{301, 2}};
        auto root = registry.NewObject<LifecycleProbe>(access, LifecycleType);
        REQUIRE(root);
        const ObjectHandle handle = root.Value().Handle();
        WeakRef<LifecycleProbe> weak(handle);
        auto borrow = registry.Resolve<LifecycleProbe>(access, handle);
        auto backend = registry.AcquireBackendLease(access, handle);
        REQUIRE(borrow && backend);
        auto object = borrow.Value().Get();
        REQUIRE(object);
        CHECK(object.Value()->InspectLifecycle().Error() == ErrorCode::WrongContext);
        CHECK(object.Value()->IsDestroyRequested().Error() == ErrorCode::WrongContext);
        CHECK(object.Value()->Destroy().Error() == ErrorCode::WrongContext);
        CHECK(borrow.Value().InspectLifecycle().Error() == ErrorCode::WrongContext);
        CHECK(other.RequestDestroy(access, handle).Error() == ErrorCode::StaleHandle);
        CHECK(registry.InspectLifecycle(wrong, handle).Error() == ErrorCode::WrongContext);
        CHECK(other.InspectLifecycle(access, handle).Error() == ErrorCode::StaleHandle);
        CHECK(registry.InspectLifecycle(access, {}).Error() == ErrorCode::StaleHandle);

        ForeignQuery query{&registry, access, handle, &borrow.Value()};
        platform::Thread worker;
        if (worker.Start(QueryOnForeignThread, &query) != platform::ThreadStatus::Success ||
            worker.WaitForCompletion(10000) != platform::WaitResult::Signaled ||
            worker.Join() != platform::ThreadStatus::Success) {
            platform::Fatal("Object lifecycle worker did not finish");
        }
        CHECK(query.result == ErrorCode::WrongThread);
        CHECK(query.borrowResult == ErrorCode::WrongThread);
        CHECK(query.scopedBorrowResult == ErrorCode::WrongThread);
        CHECK(query.scopeResult == ErrorCode::WrongContext);

        {
            ObjectAccessScope scope(access);
            auto alive = object.Value()->InspectLifecycle();
            REQUIRE(alive);
            CHECK(alive.Value() == ObjectLifecycle::Alive);
            auto initial = object.Value()->IsDestroyRequested();
            REQUIRE(initial);
            CHECK(!initial.Value());
            {
                ObjectAccessScope otherScope(wrong);
                CHECK(object.Value()->InspectLifecycle().Error() == ErrorCode::WrongContext);
                CHECK(object.Value()->Destroy().Error() == ErrorCode::WrongContext);
                CHECK(borrow.Value().InspectLifecycle().Error() == ErrorCode::WrongContext);
            }
            auto restored = object.Value()->InspectLifecycle();
            REQUIRE(restored);
            CHECK(restored.Value() == ObjectLifecycle::Alive);
            // 음성 대조: root가 물리 수명을 유지하는 테스트에서만 Borrow 미보유를 주입한다.
            LifecycleProbe* withoutBorrow = object.Value();
            borrow.Value().Reset();
            CHECK(withoutBorrow->InspectLifecycle().Error() == ErrorCode::InvalidArgument);
            CHECK(withoutBorrow->Destroy().Error() == ErrorCode::InvalidArgument);
            borrow = registry.Resolve<LifecycleProbe>(access, handle);
            REQUIRE(borrow);
            object = borrow.Value().Get();
            REQUIRE(object);
            REQUIRE(object.Value()->Destroy());
            // 동일 Borrow 유지 중에는 상태 조회와 중복 파괴 요청만 허용한다.
            auto requested = object.Value()->IsDestroyRequested();
            REQUIRE(requested);
            CHECK(requested.Value());
            REQUIRE(object.Value()->Destroy());
            auto borrowedState = borrow.Value().InspectLifecycle();
            REQUIRE(borrowedState);
            CHECK(borrowedState.Value() == ObjectLifecycle::DestroyRequested);
        }
        CHECK(ObjectAccessScope::Current().Error() == ErrorCode::WrongContext);
        REQUIRE(registry.RequestDestroy(access, handle));
        auto pending = registry.InspectLifecycle(access, handle);
        REQUIRE(pending);
        CHECK(pending.Value() == ObjectLifecycle::DestroyRequested);
        CHECK(borrow.Value().Get().Error() == ErrorCode::Destroyed);
        CHECK(registry.Resolve<LifecycleProbe>(access, handle).Error() == ErrorCode::Destroyed);
        CHECK(weak.TryPin(registry, access).Error() == ErrorCode::Destroyed);
        CHECK(registry.Collect(access).Error() == ErrorCode::Busy);
        auto cloned = root.Value().Clone();
        REQUIRE(cloned);
        borrow.Value().Reset();
        CHECK(borrow.Value().InspectLifecycle().Error() == ErrorCode::InvalidArgument);
        root.Value().Reset();
        REQUIRE(registry.Collect(access));
        auto held = registry.InspectLifecycle(access, handle);
        REQUIRE(held);
        CHECK(held.Value() == ObjectLifecycle::DestroyRequested);
        cloned.Value().Reset();
        REQUIRE(registry.Collect(access));
        auto retired = registry.InspectLifecycle(access, handle);
        REQUIRE(retired);
        CHECK(retired.Value() == ObjectLifecycle::Retired);
        backend.Value().Reset();
        REQUIRE(registry.DrainRetired(access));
        CHECK(registry.InspectLifecycle(access, handle).Error() == ErrorCode::StaleHandle);
        auto next = registry.NewObject<LifecycleProbe>(access, LifecycleType);
        REQUIRE(next);
        CHECK(next.Value().Handle().slot == handle.slot);
        CHECK(next.Value().Handle().generation != handle.generation);
        CHECK(registry.InspectLifecycle(access, handle).Error() == ErrorCode::StaleHandle);
        CHECK(weak.TryPin(registry, access).Error() == ErrorCode::StaleHandle);
        next.Value().Reset();
        REQUIRE(other.Shutdown(access));
        REQUIRE(registry.Shutdown(access));
        CHECK(registry.InspectLifecycle(access, handle).Error() == ErrorCode::ShuttingDown);
        CHECK(registry.RequestDestroy(access, handle).Error() == ErrorCode::ShuttingDown);
        REQUIRE(types.Shutdown());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(ObjectLifecycleConstructionAndCallbackReentry) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(types.Register({LifecycleType, 1, {}, {}}, MakeRuntimeTypeOps<LifecycleProbe>()));
        ObjectRegistry registry(allocator, types, {302, 1});
        const auto access = registry.OwnerAccess();
        ObjectAccessScope scope(access);
        callbackRegistry = &registry;
        ResetCallbacks reset;
        destructions = 0;
        constructorQuery = ErrorCode::None;
        constructorDestroy = ErrorCode::None;
        destructorQuery = ErrorCode::None;
        destructorDestroy = ErrorCode::None;
        traceQuery = ErrorCode::None;
        inspectTrace = true;
        {
            ConstructionContext construction(registry, access);
            auto created = construction.Create<LifecycleProbe>(LifecycleType);
            REQUIRE(created);
            CHECK(constructorQuery == ErrorCode::StaleHandle);
            CHECK(constructorDestroy == ErrorCode::StaleHandle);
            const ObjectHandle staged = created.Value();
            CHECK(registry.InspectLifecycle(access, staged).Error() == ErrorCode::StaleHandle);
            auto stagedObject = construction.Get<LifecycleProbe>(staged);
            REQUIRE(stagedObject);
            CHECK(stagedObject.Value()->InspectLifecycle().Error() == ErrorCode::StaleHandle);
            CHECK(stagedObject.Value()->Destroy().Error() == ErrorCode::StaleHandle);
            construction.Rollback();
            CHECK(destructorQuery == ErrorCode::Busy);
            CHECK(destructorDestroy == ErrorCode::Busy);
            CHECK(destructions == 1);
            CHECK(registry.InspectLifecycle(access, staged).Error() == ErrorCode::StaleHandle);
        }
        auto root = registry.NewObject<LifecycleProbe>(access, LifecycleType);
        REQUIRE(root);
        REQUIRE(registry.Collect(access));
        CHECK(traceQuery == ErrorCode::Busy);
        root.Value().Reset();
        destructorQuery = ErrorCode::None;
        destructorDestroy = ErrorCode::None;
        REQUIRE(registry.Collect(access));
        CHECK(destructorQuery == ErrorCode::Busy);
        CHECK(destructorDestroy == ErrorCode::Busy);
        CHECK(destructions == 2);
        callbackRegistry = nullptr;
        inspectTrace = false;
        REQUIRE(registry.Shutdown(access));
        REQUIRE(types.Shutdown());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(ObjectLifecycleDistinguishesGcCutoffAndRetirement) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(types.Register({LifecycleType, 1, {}, {}}, MakeRuntimeTypeOps<LifecycleProbe>()));
        ObjectRegistry registry(allocator, types, {303, 1});
        const auto access = registry.OwnerAccess();
        auto first = registry.NewObject<LifecycleProbe>(access, LifecycleType);
        auto requested = registry.NewObject<LifecycleProbe>(access, LifecycleType);
        auto white = registry.NewObject<LifecycleProbe>(access, LifecycleType);
        auto anchor = registry.NewObject<LifecycleProbe>(access, LifecycleType);
        REQUIRE(first && requested && white && anchor);
        const ObjectHandle requestedHandle = requested.Value().Handle();
        const ObjectHandle whiteHandle = white.Value().Handle();
        auto requestedBackend = registry.AcquireBackendLease(access, requestedHandle);
        auto backend = registry.AcquireBackendLease(access, whiteHandle);
        REQUIRE(requestedBackend && backend);
        REQUIRE(registry.RequestDestroy(access, requestedHandle));
        first.Value().Reset();
        requested.Value().Reset();
        white.Value().Reset();
        REQUIRE(registry.BeginCollection(access));
        REQUIRE(registry.StepCollection(access, {1, 0}));
        auto step = registry.StepCollection(access, {1, 0});
        REQUIRE(step);
        CHECK(step.Value().phase == GcPhase::Sweeping);
        auto cutoff = registry.InspectLifecycle(access, whiteHandle);
        REQUIRE(cutoff);
        CHECK(cutoff.Value() == ObjectLifecycle::Unreachable);
        CHECK(registry.RequestDestroy(access, whiteHandle).Error() == ErrorCode::Destroyed);
        auto requestedCutoff = registry.InspectLifecycle(access, requestedHandle);
        REQUIRE(requestedCutoff);
        CHECK(requestedCutoff.Value() == ObjectLifecycle::Unreachable);
        CHECK(registry.RequestDestroy(access, requestedHandle).IsOk());
        REQUIRE(registry.StepCollection(access, {1, 0}));
        auto requestedRetired = registry.InspectLifecycle(access, requestedHandle);
        REQUIRE(requestedRetired);
        CHECK(requestedRetired.Value() == ObjectLifecycle::Retired);
        CHECK(registry.RequestDestroy(access, requestedHandle).IsOk());
        REQUIRE(registry.StepCollection(access, {1, 0}));
        auto retired = registry.InspectLifecycle(access, whiteHandle);
        REQUIRE(retired);
        CHECK(retired.Value() == ObjectLifecycle::Retired);
        REQUIRE(FinishCollection(registry, access));
        requestedBackend.Value().Reset();
        backend.Value().Reset();
        REQUIRE(registry.DrainRetired(access));
        CHECK(registry.InspectLifecycle(access, whiteHandle).Error() == ErrorCode::StaleHandle);
        anchor.Value().Reset();
        REQUIRE(registry.Shutdown(access));
        REQUIRE(types.Shutdown());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(ObjectLifecycleParallelTraceRejectsForeignThreadBeforeReentry) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        auto ops = MakeRuntimeTypeOps<LifecycleProbe>();
        ops.parallelTraceSafe = true;
        REQUIRE(types.Register({LifecycleType, 1, {}, {}}, Move(ops)));
        WorkerScheduler scheduler(allocator);
        REQUIRE(scheduler.Start({2, 64, 16}));
        ObjectRegistry registry(allocator, types, {304, 1});
        const auto access = registry.OwnerAccess();
        ObjectAccessScope scope(access);
        callbackRegistry = &registry;
        ResetCallbacks reset;
        inspectTrace = true;
        traceQuery = ErrorCode::None;
        auto root = registry.NewObject<LifecycleProbe>(access, LifecycleType);
        REQUIRE(root);
        REQUIRE(registry.BeginCollection(access));
        REQUIRE(registry.StepCollection(access, {1, 0}, &scheduler));
        CHECK(traceQuery == ErrorCode::WrongThread);
        inspectTrace = false;
        REQUIRE(FinishCollection(registry, access));
        root.Value().Reset();
        callbackRegistry = nullptr;
        REQUIRE(registry.Shutdown(access));
        REQUIRE(scheduler.Shutdown());
        REQUIRE(types.Shutdown());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}
