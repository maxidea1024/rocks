#include "Tests/Support/Test.h"
#include "Tests/Support/NFailAllocator.h"

#include <Core/Memory/SystemAllocator.h>
#include <Object/ObjectRegistry.h>

namespace {

using namespace rocks;

struct DiagnosticProbe final : Object {};

constexpr Guid DiagnosticType{0x444941474e4f5354ULL, 1};

} // namespace

TEST(ObjectDiagnosticsTrackLifetimeWithoutAllocation) {
    SystemAllocator backing;

    {
        test::NFailAllocator allocator(backing);
        TypeRegistry types(allocator);
        auto type =
            types.Register({DiagnosticType, 1, {}, {}}, MakeRuntimeTypeOps<DiagnosticProbe>());
        REQUIRE(type.IsOk());
        ObjectRegistry registry(allocator, types, {101, 1});
        auto access = registry.OwnerAccess();
        auto root = registry.NewObject<DiagnosticProbe>(access, DiagnosticType);
        REQUIRE(root.IsOk());
        const auto handle = root.Value().Handle();
        auto borrow = registry.Resolve<DiagnosticProbe>(access, handle);
        REQUIRE(borrow.IsOk());
        auto backend = registry.AcquireBackendLease(access, handle);
        REQUIRE(backend.IsOk());
        ObjectDiagnosticRecord records[1];

        allocator.FailOnAttempt(1);
        auto snapshot = registry.CaptureDiagnostics(records);
        REQUIRE(snapshot.IsOk());
        CHECK(allocator.Attempts() == 0);
        CHECK(snapshot.Value() == 1);
        CHECK(records[0].handle == handle);
        CHECK(records[0].context == access.context);
        CHECK(records[0].type == DiagnosticType);
        CHECK(records[0].phase == ObjectPhase::Alive);
        CHECK(records[0].roots == 1);
        CHECK(records[0].borrows == 1);
        CHECK(records[0].backendHolds == 1);

        REQUIRE(registry.RequestDestroy(access, handle).IsOk());
        REQUIRE(registry.CaptureDiagnostics(records).IsOk());
        CHECK(records[0].phase == ObjectPhase::DestroyRequested);
        borrow.Value().Reset();
        root.Value().Reset();
        auto failedCollection = registry.Collect(access);
        CHECK(!failedCollection.IsOk());
        CHECK(failedCollection.Error() == ErrorCode::OutOfMemory);
        CHECK(registry.LastGcReport().examined == 0);
        REQUIRE(registry.CaptureDiagnostics(records).IsOk());
        CHECK(records[0].phase == ObjectPhase::DestroyRequested);

        allocator.DisableFailure();
        auto collected = registry.Collect(access);
        REQUIRE(collected.IsOk());
        CHECK(collected.Value().reclaimed == 0);
        CHECK(registry.LastGcReport().retired == 1);
        CHECK(registry.LastGcReport().durationNanoseconds == collected.Value().durationNanoseconds);
        REQUIRE(registry.CaptureDiagnostics(records).IsOk());
        CHECK(records[0].phase == ObjectPhase::Retired);
        CHECK(records[0].roots == 0);
        CHECK(records[0].backendHolds == 1);
        backend.Value().Reset();
        auto drained = registry.DrainRetired(access);
        REQUIRE(drained.IsOk());
        CHECK(drained.Value() == 1);
        auto empty = registry.CaptureDiagnostics({});
        REQUIRE(empty.IsOk());
        CHECK(empty.Value() == 0);
        REQUIRE(registry.Shutdown(access).IsOk());
        REQUIRE(types.Shutdown().IsOk());
    }

    CHECK(backing.Stats().liveAllocations == 0);
}

TEST(ObjectDiagnosticsCapacityFailurePreservesCallerStorage) {
    SystemAllocator allocator;

    {
        TypeRegistry types(allocator);
        REQUIRE(types.Register({DiagnosticType, 1, {}, {}}, MakeRuntimeTypeOps<DiagnosticProbe>())
                    .IsOk());
        ObjectRegistry registry(allocator, types, {102, 1});
        auto access = registry.OwnerAccess();
        auto first = registry.NewObject<DiagnosticProbe>(access, DiagnosticType);
        auto second = registry.NewObject<DiagnosticProbe>(access, DiagnosticType);
        REQUIRE(first.IsOk());
        REQUIRE(second.IsOk());

        ObjectDiagnosticRecord tooSmall[1];
        tooSmall[0].roots = 12345;
        const auto allocations = allocator.Stats().totalAllocations;
        auto result = registry.CaptureDiagnostics(tooSmall);
        CHECK(!result.IsOk());
        CHECK(result.Error() == ErrorCode::CapacityExceeded);
        CHECK(tooSmall[0].roots == 12345);
        CHECK(allocator.Stats().totalAllocations == allocations);
        first.Value().Reset();
        second.Value().Reset();
        REQUIRE(registry.Shutdown(access).IsOk());
        REQUIRE(types.Shutdown().IsOk());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}
