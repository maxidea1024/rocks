#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

#include <Core/Memory/SystemAllocator.h>
#include <Object/ConstructionContext.h>
#include <Object/TypeRegistry.h>

namespace {

using namespace rocks;

uint32 constructed = 0;
uint32 destroyed = 0;
uint32 destructionOrder[8]{};

struct ConstructionProbe final : Object {
    uint32 ordinal = 0;
    ObjectRef<ConstructionProbe> child;

    ConstructionProbe() noexcept : ordinal(++constructed) {
    }

    ~ConstructionProbe() noexcept {
        if (destroyed < 8) {
            destructionOrder[destroyed] = ordinal;
        }

        ++destroyed;
    }

    void Trace(TraceVisitor& visitor) const noexcept {
        visitor.Visit(child);
    }
};

constexpr Guid ConstructionType{0x434F4E5354525543ULL, 1};

void ResetCounts() noexcept {
    constructed = 0;
    destroyed = 0;

    for (auto& entry : destructionOrder) {
        entry = 0;
    }
}

} // namespace

TEST(ObjectConstructionIsPrivateAndRollbackRunsInReverseOrder) {
    SystemAllocator allocator;

    {
        TypeRegistry types(allocator);
        auto registered = types.Register(TypeSchema{ConstructionType, 1, {}, {}},
                                         MakeRuntimeTypeOps<ConstructionProbe>());
        REQUIRE(registered.IsOk());
        ObjectRegistry registry(allocator, types, Guid{111, 1});
        auto access = registry.OwnerAccess();
        ObjectHandle abandoned[3]{};
        ResetCounts();

        {
            ConstructionContext construction(registry, access);

            for (uint32 index = 0; index < 3; ++index) {
                auto object = construction.Create<ConstructionProbe>(ConstructionType);
                REQUIRE(object.IsOk());
                abandoned[index] = object.Value();
                CHECK(!registry.Resolve<ConstructionProbe>(access, object.Value()).IsOk());
                CHECK(!registry.TryPin<ConstructionProbe>(access, object.Value()).IsOk());
                CHECK(construction.Get<ConstructionProbe>(object.Value()).IsOk());
            }

            CHECK(!registry.Collect(access).IsOk());
            CHECK(registry.Stats().liveObjects == 0);
            CHECK(registry.Stats().reservedObjects == 3);
            CHECK(destroyed == 0);
        }

        CHECK(constructed == 3);
        CHECK(destroyed == 3);
        CHECK(destructionOrder[0] == 3);
        CHECK(destructionOrder[1] == 2);
        CHECK(destructionOrder[2] == 1);
        CHECK(registry.Stats().reservedObjects == 0);
        CHECK(registry.Stats().liveObjects == 0);
        CHECK(registry.Stats().roots == 0);

        for (ObjectHandle handle : abandoned) {
            CHECK(!registry.Resolve<ConstructionProbe>(access, handle).IsOk());
        }

        REQUIRE(registry.Shutdown(access).IsOk());
        REQUIRE(types.Unregister(registered.Value()).IsOk());
        REQUIRE(types.Shutdown().IsOk());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(ObjectConstructionCommitPublishesConnectedGraphAndCannotRollbackIt) {
    SystemAllocator allocator;

    {
        TypeRegistry types(allocator);
        auto registered = types.Register(TypeSchema{ConstructionType, 1, {}, {}},
                                         MakeRuntimeTypeOps<ConstructionProbe>());
        REQUIRE(registered.IsOk());
        ObjectRegistry registry(allocator, types, Guid{112, 1});
        auto access = registry.OwnerAccess();
        ResetCounts();
        ConstructionContext construction(registry, access);
        auto parent = construction.Create<ConstructionProbe>(ConstructionType);
        auto child = construction.Create<ConstructionProbe>(ConstructionType);
        REQUIRE(parent.IsOk());
        REQUIRE(child.IsOk());
        auto pending = construction.Get<ConstructionProbe>(parent.Value());
        REQUIRE(pending.IsOk());
        REQUIRE(construction.SetReference(parent.Value(), pending.Value()->child, child.Value())
                    .IsOk());

        auto published = construction.Commit();
        REQUIRE(published.IsOk());
        construction.Rollback();
        CHECK(!construction.Commit().IsOk());
        CHECK(!construction.Get<ConstructionProbe>(parent.Value()).IsOk());
        CHECK(registry.Stats().liveObjects == 2);
        CHECK(registry.Resolve<ConstructionProbe>(access, parent.Value()).IsOk());
        CHECK(registry.Resolve<ConstructionProbe>(access, child.Value()).IsOk());
        REQUIRE(registry.Collect(access).IsOk());
        CHECK(destroyed == 0);

        published.Value().Reset();
        REQUIRE(registry.Collect(access).IsOk());
        CHECK(destroyed == 2);
        REQUIRE(registry.Shutdown(access).IsOk());
        REQUIRE(types.Unregister(registered.Value()).IsOk());
        REQUIRE(types.Shutdown().IsOk());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(ObjectConstructionRejectsEmptyCommitAndForeignPendingLookup) {
    SystemAllocator allocator;

    {
        TypeRegistry types(allocator);
        auto registered = types.Register(TypeSchema{ConstructionType, 1, {}, {}},
                                         MakeRuntimeTypeOps<ConstructionProbe>());
        REQUIRE(registered.IsOk());
        ObjectRegistry registry(allocator, types, Guid{113, 1});
        auto access = registry.OwnerAccess();

        {
            ConstructionContext empty(registry, access);
            CHECK(!empty.Commit().IsOk());
        }

        auto existing = registry.NewObject<ConstructionProbe>(access, ConstructionType);
        REQUIRE(existing.IsOk());

        {
            ConstructionContext context(registry, access);
            CHECK(!context.Get<ConstructionProbe>(existing.Value().Handle()).IsOk());
        }

        existing.Value().Reset();
        REQUIRE(registry.Collect(access).IsOk());
        REQUIRE(registry.Shutdown(access).IsOk());
        REQUIRE(types.Unregister(registered.Value()).IsOk());
        REQUIRE(types.Shutdown().IsOk());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(ObjectCreationAllocationFailureRollsBackAndPermitsRetry) {
    SystemAllocator allocator;
    uint64 allocationCount = 0;

    {
        TypeRegistry types(allocator);
        auto registered = types.Register(TypeSchema{ConstructionType, 1, {}, {}},
                                         MakeRuntimeTypeOps<ConstructionProbe>());
        REQUIRE(registered.IsOk());
        rocks::test::NFailAllocator fault(allocator);
        ObjectRegistry registry(fault, types, Guid{114, 1});
        auto access = registry.OwnerAccess();
        auto root = registry.NewObject<ConstructionProbe>(access, ConstructionType);
        REQUIRE(root.IsOk());
        allocationCount = fault.Attempts();
        root.Value().Reset();
        REQUIRE(registry.Shutdown(access).IsOk());
        REQUIRE(types.Unregister(registered.Value()).IsOk());
        REQUIRE(types.Shutdown().IsOk());
    }

    REQUIRE(allocationCount != 0);
    CHECK(allocator.Stats().liveAllocations == 0);

    for (uint64 attempt = 1; attempt <= allocationCount; ++attempt) {
        {
            TypeRegistry types(allocator);
            auto registered = types.Register(TypeSchema{ConstructionType, 1, {}, {}},
                                             MakeRuntimeTypeOps<ConstructionProbe>());
            REQUIRE(registered.IsOk());
            rocks::test::NFailAllocator fault(allocator);
            ObjectRegistry registry(fault, types, Guid{115, attempt});
            auto access = registry.OwnerAccess();
            ResetCounts();
            fault.FailOnAttempt(attempt);

            auto rejected = registry.NewObject<ConstructionProbe>(access, ConstructionType);
            CHECK(!rejected.IsOk());
            CHECK(registry.Stats().liveObjects == 0);
            CHECK(registry.Stats().reservedObjects == 0);
            CHECK(registry.Stats().roots == 0);
            CHECK(constructed == destroyed);

            fault.DisableFailure();
            auto retry = registry.NewObject<ConstructionProbe>(access, ConstructionType);
            REQUIRE(retry.IsOk());
            retry.Value().Reset();
            REQUIRE(registry.Shutdown(access).IsOk());
            CHECK(constructed == destroyed);
            REQUIRE(types.Unregister(registered.Value()).IsOk());
            REQUIRE(types.Shutdown().IsOk());
        }

        CHECK(allocator.Stats().liveAllocations == 0);
    }
}
