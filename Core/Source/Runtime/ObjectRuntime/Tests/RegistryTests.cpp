#include <TestSupport/Test.h>

#include <Core/Memory/SystemAllocator.h>
#include <Object/ConstructionContext.h>
#include <Object/ObjectRegistry.h>
#include <Object/TypeRegistry.h>

namespace {

using namespace rocks;

struct IdentityProbe final : Object {
    uint32 value = 0x12345678;
    ObjectRef<IdentityProbe> edge;

    void Trace(TraceVisitor& visitor) const noexcept {
        visitor.Visit(edge);
    }
};

struct UnrelatedProbe final : Object {
    uint64 differentLayout[4]{};
};

template <class T>
concept PublicCopyAssignment = requires(T& left, const T& right) { left = right; };

template <class T>
concept PublicMoveAssignment = requires(T& left, T&& right) { left = static_cast<T&&>(right); };

static_assert(!PublicCopyAssignment<ObjectRef<IdentityProbe>>);
static_assert(!PublicMoveAssignment<ObjectRef<IdentityProbe>>);
static_assert(!PublicCopyAssignment<IdentityProbe>);
static_assert(!PublicMoveAssignment<IdentityProbe>);
static_assert(!PublicCopyAssignment<UnrelatedProbe>);
static_assert(!PublicMoveAssignment<UnrelatedProbe>);

constexpr Guid IdentityType{0x4944454E54495459ULL, 1};

} // namespace

TEST(ObjectRegistryRejectsNullForeignAndWrongNativeType) {
    SystemAllocator allocator;

    {
        TypeRegistry types(allocator);
        auto registration = types.Register(TypeSchema{IdentityType, 1, {}, {}},
                                           MakeRuntimeTypeOps<IdentityProbe>());
        REQUIRE(registration.IsOk());

        ObjectRegistry first(allocator, types, Guid{81, 1});
        ObjectRegistry second(allocator, types, Guid{81, 1});
        auto firstAccess = first.OwnerAccess();
        auto secondAccess = second.OwnerAccess();
        auto firstRoot = first.NewObject<IdentityProbe>(firstAccess, IdentityType);
        auto secondRoot = second.NewObject<IdentityProbe>(secondAccess, IdentityType);
        REQUIRE(firstRoot.IsOk());
        REQUIRE(secondRoot.IsOk());

        CHECK(!first.Resolve<IdentityProbe>(firstAccess, ObjectHandle{}).IsOk());
        CHECK(!first.Resolve<IdentityProbe>(firstAccess, secondRoot.Value().Handle()).IsOk());
        CHECK(!second.TryPin<IdentityProbe>(secondAccess, firstRoot.Value().Handle()).IsOk());
        CHECK(!first.Resolve<UnrelatedProbe>(firstAccess, firstRoot.Value().Handle()).IsOk());
        CHECK(first.Resolve<Object>(firstAccess, firstRoot.Value().Handle()).IsOk());
        CHECK(!first.NewObject<UnrelatedProbe>(firstAccess, IdentityType).IsOk());

        firstRoot.Value().Reset();
        secondRoot.Value().Reset();
        REQUIRE(first.Collect(firstAccess).IsOk());
        REQUIRE(second.Collect(secondAccess).IsOk());
        REQUIRE(first.Shutdown(firstAccess).IsOk());
        REQUIRE(second.Shutdown(secondAccess).IsOk());
        REQUIRE(types.Unregister(registration.Value()).IsOk());
        REQUIRE(types.Shutdown().IsOk());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(ObjectRegistryRejectsAccessFromAnotherContext) {
    SystemAllocator allocator;

    {
        TypeRegistry types(allocator);
        auto registration = types.Register(TypeSchema{IdentityType, 1, {}, {}},
                                           MakeRuntimeTypeOps<IdentityProbe>());
        REQUIRE(registration.IsOk());
        ObjectRegistry first(allocator, types, Guid{82, 1});
        ObjectRegistry second(allocator, types, Guid{82, 2});
        auto firstAccess = first.OwnerAccess();
        auto secondAccess = second.OwnerAccess();
        auto root = first.NewObject<IdentityProbe>(firstAccess, IdentityType);
        REQUIRE(root.IsOk());
        const ObjectHandle handle = root.Value().Handle();

        CHECK(!first.Resolve<IdentityProbe>(secondAccess, handle).IsOk());
        CHECK(!first.TryPin<IdentityProbe>(secondAccess, handle).IsOk());
        CHECK(!first.RequestDestroy(secondAccess, handle).IsOk());
        CHECK(!first.Collect(secondAccess).IsOk());
        CHECK(first.Resolve<IdentityProbe>(firstAccess, handle).IsOk());

        root.Value().Reset();
        REQUIRE(first.Collect(firstAccess).IsOk());
        REQUIRE(first.Shutdown(firstAccess).IsOk());
        REQUIRE(second.Shutdown(secondAccess).IsOk());
        REQUIRE(types.Unregister(registration.Value()).IsOk());
        REQUIRE(types.Shutdown().IsOk());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(ObjectRegistryQuarantinesGenerationExhaustion) {
    SystemAllocator allocator;

    {
        TypeRegistry types(allocator);
        auto registration = types.Register(TypeSchema{IdentityType, 1, {}, {}},
                                           MakeRuntimeTypeOps<IdentityProbe>());
        REQUIRE(registration.IsOk());
        ObjectRegistry registry(allocator, types, Guid{83, 1}, RegistryOptions{1, 2});
        auto access = registry.OwnerAccess();
        auto first = registry.NewObject<IdentityProbe>(access, IdentityType);
        REQUIRE(first.IsOk());
        const ObjectHandle old = first.Value().Handle();
        WeakRef<IdentityProbe> weak(old);
        first.Value().Reset();
        REQUIRE(registry.Collect(access).IsOk());

        auto second = registry.NewObject<IdentityProbe>(access, IdentityType);
        REQUIRE(second.IsOk());
        CHECK(second.Value().Handle().slot == old.slot);
        CHECK(second.Value().Handle().generation != old.generation);
        CHECK(!weak.TryPin(registry, access).IsOk());
        CHECK(!registry.Resolve<IdentityProbe>(access, old).IsOk());
        second.Value().Reset();
        REQUIRE(registry.Collect(access).IsOk());

        CHECK(!registry.NewObject<IdentityProbe>(access, IdentityType).IsOk());
        CHECK(registry.Stats().liveObjects == 0);
        CHECK(registry.Stats().reservedObjects == 0);
        REQUIRE(registry.Shutdown(access).IsOk());
        REQUIRE(types.Unregister(registration.Value()).IsOk());
        REQUIRE(types.Shutdown().IsOk());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(ObjectRegistryGrowthPreservesAddressesAcrossChunks) {
    SystemAllocator allocator;

    {
        TypeRegistry types(allocator);
        auto registration = types.Register(TypeSchema{IdentityType, 1, {}, {}},
                                           MakeRuntimeTypeOps<IdentityProbe>());
        REQUIRE(registration.IsOk());
        ObjectRegistry registry(allocator, types, Guid{84, 1});
        auto access = registry.OwnerAccess();
        auto root = registry.NewObject<IdentityProbe>(access, IdentityType);
        REQUIRE(root.IsOk());
        auto firstBorrow = registry.Resolve<IdentityProbe>(access, root.Value().Handle());
        REQUIRE(firstBorrow.IsOk());
        auto firstView = firstBorrow.Value().Get();
        REQUIRE(firstView.IsOk());
        IdentityProbe* const originalAddress = firstView.Value();
        firstBorrow.Value().Reset();

        for (uint32 index = 0; index < 96; ++index) {
            auto extra = registry.NewObject<IdentityProbe>(access, IdentityType);
            REQUIRE(extra.IsOk());
            extra.Value().Reset();
        }

        CHECK(registry.Stats().chunks >= 2);
        auto secondBorrow = registry.Resolve<IdentityProbe>(access, root.Value().Handle());
        REQUIRE(secondBorrow.IsOk());
        auto secondView = secondBorrow.Value().Get();
        REQUIRE(secondView.IsOk());
        CHECK(secondView.Value() == originalAddress);
        CHECK(secondView.Value()->value == 0x12345678);
        secondBorrow.Value().Reset();

        REQUIRE(registry.Collect(access).IsOk());
        CHECK(registry.Stats().liveObjects == 1);
        root.Value().Reset();
        REQUIRE(registry.Collect(access).IsOk());
        REQUIRE(registry.Shutdown(access).IsOk());
        REQUIRE(types.Unregister(registration.Value()).IsOk());
        REQUIRE(types.Shutdown().IsOk());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(ObjectReferenceRejectsWrongOwnerAndForeignTargetWithoutChangingEdge) {
    SystemAllocator allocator;

    {
        TypeRegistry types(allocator);
        auto registration = types.Register(TypeSchema{IdentityType, 1, {}, {}},
                                           MakeRuntimeTypeOps<IdentityProbe>());
        REQUIRE(registration.IsOk());
        ObjectRegistry registry(allocator, types, Guid{85, 1});
        ObjectRegistry foreign(allocator, types, Guid{85, 1});
        auto access = registry.OwnerAccess();
        auto foreignAccess = foreign.OwnerAccess();
        auto owner = registry.NewObject<IdentityProbe>(access, IdentityType);
        auto child = registry.NewObject<IdentityProbe>(access, IdentityType);
        auto outsider = foreign.NewObject<IdentityProbe>(foreignAccess, IdentityType);
        REQUIRE(owner.IsOk());
        REQUIRE(child.IsOk());
        REQUIRE(outsider.IsOk());

        auto borrowed = registry.Resolve<IdentityProbe>(access, owner.Value().Handle());
        REQUIRE(borrowed.IsOk());
        auto object = borrowed.Value().Get();
        REQUIRE(object.IsOk());
        REQUIRE(registry
                    .SetReference(access, owner.Value().Handle(), object.Value()->edge,
                                  child.Value().Handle())
                    .IsOk());
        CHECK(!registry
                   .SetReference(access, child.Value().Handle(), object.Value()->edge,
                                 child.Value().Handle())
                   .IsOk());
        CHECK(!registry
                   .SetReference(access, owner.Value().Handle(), object.Value()->edge,
                                 outsider.Value().Handle())
                   .IsOk());
        CHECK(object.Value()->edge.Handle() == child.Value().Handle());
        borrowed.Value().Reset();

        owner.Value().Reset();
        child.Value().Reset();
        outsider.Value().Reset();
        REQUIRE(registry.Collect(access).IsOk());
        REQUIRE(foreign.Collect(foreignAccess).IsOk());
        REQUIRE(registry.Shutdown(access).IsOk());
        REQUIRE(foreign.Shutdown(foreignAccess).IsOk());
        REQUIRE(types.Unregister(registration.Value()).IsOk());
        REQUIRE(types.Shutdown().IsOk());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}
