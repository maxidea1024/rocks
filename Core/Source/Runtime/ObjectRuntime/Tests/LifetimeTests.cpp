#include <TestSupport/Test.h>

#include <Core/Memory/SystemAllocator.h>
#include <Object/ConstructionContext.h>
#include <Object/ObjectRegistry.h>
#include <Object/TypeRegistry.h>

namespace {

using namespace rocks;

uint32 lifetimeDestructions = 0;

struct LifetimeProbe final : Object {
    ObjectRef<LifetimeProbe> child;

    ~LifetimeProbe() noexcept {
        ++lifetimeDestructions;
    }

    void Trace(TraceVisitor& visitor) const noexcept {
        visitor.Visit(child);
    }
};

constexpr Guid LifetimeType{0x4C49464554494D45ULL, 1};

ObjectRegistry* destructorRegistry = nullptr;
ObjectAccessContext destructorAccess{};
bool destructorSawBusy = false;
bool destructorReadStats = false;

struct ReentrantProbe final : Object {
    ~ReentrantProbe() noexcept {
        if (destructorRegistry != nullptr) {
            const auto stats = destructorRegistry->Stats();
            destructorReadStats = stats.roots == 0;
            auto nested = destructorRegistry->Collect(destructorAccess);
            destructorSawBusy = !nested.IsOk() && nested.Error() == ErrorCode::Busy;
        }
    }
};

} // namespace

TEST(ObjectRootCloneAndWeakPromotionPreserveStorage) {
    SystemAllocator allocator;

    {
        TypeRegistry types(allocator);
        auto registered = types.Register(TypeSchema{LifetimeType, 1, {}, {}},
                                         MakeRuntimeTypeOps<LifetimeProbe>());
        REQUIRE(registered.IsOk());
        ObjectRegistry registry(allocator, types, Guid{91, 1});
        auto access = registry.OwnerAccess();
        lifetimeDestructions = 0;
        auto root = registry.NewObject<LifetimeProbe>(access, LifetimeType);
        REQUIRE(root.IsOk());
        const ObjectHandle handle = root.Value().Handle();
        WeakRef<LifetimeProbe> weak(handle);
        auto clone = root.Value().Clone();
        REQUIRE(clone.IsOk());
        root.Value().Reset();
        REQUIRE(registry.Collect(access).IsOk());
        CHECK(lifetimeDestructions == 0);

        auto pinned = weak.TryPin(registry, access);
        REQUIRE(pinned.IsOk());
        clone.Value().Reset();
        REQUIRE(registry.Collect(access).IsOk());
        CHECK(lifetimeDestructions == 0);
        pinned.Value().Reset();
        REQUIRE(registry.Collect(access).IsOk());
        CHECK(lifetimeDestructions == 1);
        CHECK(!weak.TryPin(registry, access).IsOk());
        CHECK(registry.Stats().roots == 0);

        REQUIRE(registry.Shutdown(access).IsOk());
        REQUIRE(types.Unregister(registered.Value()).IsOk());
        REQUIRE(types.Shutdown().IsOk());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(ObjectBorrowBlocksCollectionUntilExplicitReset) {
    SystemAllocator allocator;

    {
        TypeRegistry types(allocator);
        auto registered = types.Register(TypeSchema{LifetimeType, 1, {}, {}},
                                         MakeRuntimeTypeOps<LifetimeProbe>());
        REQUIRE(registered.IsOk());
        ObjectRegistry registry(allocator, types, Guid{92, 1});
        auto access = registry.OwnerAccess();
        lifetimeDestructions = 0;
        auto root = registry.NewObject<LifetimeProbe>(access, LifetimeType);
        REQUIRE(root.IsOk());
        auto borrowed = registry.Resolve<LifetimeProbe>(access, root.Value().Handle());
        REQUIRE(borrowed.IsOk());
        root.Value().Reset();

        CHECK(!registry.Collect(access).IsOk());
        CHECK(lifetimeDestructions == 0);
        CHECK(registry.Stats().borrows == 1);
        CHECK(borrowed.Value().Get().IsOk());

        borrowed.Value().Reset();
        REQUIRE(registry.Collect(access).IsOk());
        CHECK(lifetimeDestructions == 1);
        CHECK(registry.Stats().borrows == 0);
        REQUIRE(registry.Shutdown(access).IsOk());
        REQUIRE(types.Unregister(registered.Value()).IsOk());
        REQUIRE(types.Shutdown().IsOk());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(ObjectDestroyEndsAccessButRetainsRootedStorage) {
    SystemAllocator allocator;

    {
        TypeRegistry types(allocator);
        auto registered = types.Register(TypeSchema{LifetimeType, 1, {}, {}},
                                         MakeRuntimeTypeOps<LifetimeProbe>());
        REQUIRE(registered.IsOk());
        ObjectRegistry registry(allocator, types, Guid{93, 1});
        auto access = registry.OwnerAccess();
        lifetimeDestructions = 0;
        auto root = registry.NewObject<LifetimeProbe>(access, LifetimeType);
        REQUIRE(root.IsOk());
        const ObjectHandle handle = root.Value().Handle();
        auto borrowed = registry.Resolve<LifetimeProbe>(access, handle);
        REQUIRE(borrowed.IsOk());

        REQUIRE(registry.RequestDestroy(access, handle).IsOk());
        REQUIRE(registry.RequestDestroy(access, handle).IsOk());
        CHECK(!registry.Resolve<LifetimeProbe>(access, handle).IsOk());
        CHECK(!registry.TryPin<LifetimeProbe>(access, handle).IsOk());
        CHECK(!borrowed.Value().Get().IsOk());
        CHECK(lifetimeDestructions == 0);
        borrowed.Value().Reset();

        auto preserved = root.Value().Clone();
        REQUIRE(preserved.IsOk());
        root.Value().Reset();
        REQUIRE(registry.Collect(access).IsOk());
        CHECK(lifetimeDestructions == 0);
        preserved.Value().Reset();
        REQUIRE(registry.Collect(access).IsOk());
        CHECK(lifetimeDestructions == 1);

        REQUIRE(registry.Shutdown(access).IsOk());
        REQUIRE(types.Unregister(registered.Value()).IsOk());
        REQUIRE(types.Shutdown().IsOk());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(ObjectDestroyedOwnerStillTracesChildWhileRooted) {
    SystemAllocator allocator;

    {
        TypeRegistry types(allocator);
        auto registered = types.Register(TypeSchema{LifetimeType, 1, {}, {}},
                                         MakeRuntimeTypeOps<LifetimeProbe>());
        REQUIRE(registered.IsOk());
        ObjectRegistry registry(allocator, types, Guid{94, 1});
        auto access = registry.OwnerAccess();
        lifetimeDestructions = 0;
        auto owner = registry.NewObject<LifetimeProbe>(access, LifetimeType);
        auto child = registry.NewObject<LifetimeProbe>(access, LifetimeType);
        REQUIRE(owner.IsOk());
        REQUIRE(child.IsOk());
        const ObjectHandle childHandle = child.Value().Handle();
        auto borrowed = registry.Resolve<LifetimeProbe>(access, owner.Value().Handle());
        REQUIRE(borrowed.IsOk());
        auto value = borrowed.Value().Get();
        REQUIRE(value.IsOk());
        REQUIRE(
            registry.SetReference(access, owner.Value().Handle(), value.Value()->child, childHandle)
                .IsOk());
        borrowed.Value().Reset();
        child.Value().Reset();

        REQUIRE(registry.RequestDestroy(access, owner.Value().Handle()).IsOk());
        REQUIRE(registry.Collect(access).IsOk());
        CHECK(lifetimeDestructions == 0);
        CHECK(registry.Resolve<LifetimeProbe>(access, childHandle).IsOk());
        CHECK(registry.Stats().liveObjects + registry.Stats().destroyRequested == 2);

        owner.Value().Reset();
        REQUIRE(registry.Collect(access).IsOk());
        CHECK(lifetimeDestructions == 2);
        REQUIRE(registry.Shutdown(access).IsOk());
        REQUIRE(types.Unregister(registered.Value()).IsOk());
        REQUIRE(types.Shutdown().IsOk());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(ObjectBackendHoldDelaysReclaimWithoutResurrectingObject) {
    SystemAllocator allocator;

    {
        TypeRegistry types(allocator);
        auto registered = types.Register(TypeSchema{LifetimeType, 1, {}, {}},
                                         MakeRuntimeTypeOps<LifetimeProbe>());
        REQUIRE(registered.IsOk());
        ObjectRegistry registry(allocator, types, Guid{95, 1});
        auto access = registry.OwnerAccess();
        lifetimeDestructions = 0;
        auto root = registry.NewObject<LifetimeProbe>(access, LifetimeType);
        REQUIRE(root.IsOk());
        const ObjectHandle handle = root.Value().Handle();
        auto hold = registry.AcquireBackendLease(access, handle);
        REQUIRE(hold.IsOk());
        root.Value().Reset();

        REQUIRE(registry.Collect(access).IsOk());
        CHECK(!registry.TryPin<LifetimeProbe>(access, handle).IsOk());
        CHECK(!registry.Resolve<LifetimeProbe>(access, handle).IsOk());
        CHECK(lifetimeDestructions == 0);
        CHECK(registry.Stats().backendHolds == 1);
        CHECK(!types.Unregister(registered.Value()).IsOk());
        CHECK(!registry.Shutdown(access).IsOk());

        hold.Value().Reset();
        hold.Value().Reset();
        REQUIRE(registry.DrainRetired(access).IsOk());
        CHECK(lifetimeDestructions == 1);
        CHECK(registry.Stats().backendHolds == 0);
        REQUIRE(registry.Shutdown(access).IsOk());
        REQUIRE(types.Unregister(registered.Value()).IsOk());
        REQUIRE(types.Shutdown().IsOk());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(ObjectShutdownBusyClosesAdmissionAndCanRecover) {
    SystemAllocator allocator;

    {
        TypeRegistry types(allocator);
        auto registered = types.Register(TypeSchema{LifetimeType, 1, {}, {}},
                                         MakeRuntimeTypeOps<LifetimeProbe>());
        REQUIRE(registered.IsOk());
        ObjectRegistry registry(allocator, types, Guid{96, 1});
        auto access = registry.OwnerAccess();
        lifetimeDestructions = 0;
        auto root = registry.NewObject<LifetimeProbe>(access, LifetimeType);
        REQUIRE(root.IsOk());

        CHECK(!registry.Shutdown(access).IsOk());
        CHECK(!registry.NewObject<LifetimeProbe>(access, LifetimeType).IsOk());
        CHECK(lifetimeDestructions == 0);
        root.Value().Reset();
        REQUIRE(registry.Shutdown(access).IsOk());
        CHECK(lifetimeDestructions == 1);
        CHECK(registry.Stats().roots == 0);
        CHECK(registry.Stats().borrows == 0);
        CHECK(registry.Stats().backendHolds == 0);
        REQUIRE(types.Unregister(registered.Value()).IsOk());
        REQUIRE(types.Shutdown().IsOk());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(ObjectDestructorRunsOutsideGateAndRejectsNestedCollection) {
    SystemAllocator allocator;

    {
        TypeRegistry types(allocator);
        constexpr Guid type{0x5245454E5452414EULL, 1};
        auto registered =
            types.Register(TypeSchema{type, 1, {}, {}}, MakeRuntimeTypeOps<ReentrantProbe>());
        REQUIRE(registered.IsOk());
        ObjectRegistry registry(allocator, types, Guid{97, 1});
        auto access = registry.OwnerAccess();
        auto root = registry.NewObject<ReentrantProbe>(access, type);
        REQUIRE(root.IsOk());
        destructorRegistry = &registry;
        destructorAccess = access;
        destructorSawBusy = false;
        destructorReadStats = false;
        root.Value().Reset();

        auto collected = registry.Collect(access);
        destructorRegistry = nullptr;
        REQUIRE(collected.IsOk());
        CHECK(destructorReadStats);
        CHECK(destructorSawBusy);
        CHECK(registry.Stats().liveObjects == 0);
        REQUIRE(registry.Shutdown(access).IsOk());
        REQUIRE(types.Unregister(registered.Value()).IsOk());
        REQUIRE(types.Shutdown().IsOk());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}
