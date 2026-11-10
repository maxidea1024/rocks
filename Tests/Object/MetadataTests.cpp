#include "Tests/Support/NFailAllocator.h"
#include "Tests/Support/Test.h"

#include <Core/Memory/SystemAllocator.h>
#include <Object/TypeRegistry.h>

namespace {

using namespace rocks;

struct MetadataProbe final : Object {};

constexpr Guid MetadataType{0x4D45544144415441ULL, 1};

struct CodeOwner {
    uint32 references = 0;
    uint32 releases = 0;
    TypeRegistry* registry = nullptr;
    bool releaseObservedEmptyRegistry = false;
};

void RetainCode(void* context) noexcept {
    ++static_cast<CodeOwner*>(context)->references;
}

void ReleaseCode(void* context) noexcept {
    auto& owner = *static_cast<CodeOwner*>(context);
    --owner.references;
    ++owner.releases;
    owner.releaseObservedEmptyRegistry = owner.registry->Stats().registeredTypes == 0;
}

} // namespace

TEST(ObjectMetadataOwnsNestedSourceDataAndPreservesReaderLease) {
    SystemAllocator allocator;

    {
        TypeRegistry types(allocator);
        char typeName[] = "OriginalType";
        char fieldName[] = "OriginalField";
        uint8 attributes[] = {17, 23, 41};
        FieldSchema fields[] = {{Guid{101, 1}, FieldKind::Value, 7,
                                 Span<const char>(fieldName, sizeof(fieldName) - 1),
                                 Span<const uint8>(attributes, sizeof(attributes))}};
        TypeSchema source{MetadataType, 3, Span<const FieldSchema>(fields, 1),
                          Span<const char>(typeName, sizeof(typeName) - 1)};
        auto registered = types.Register(source, MakeRuntimeTypeOps<MetadataProbe>());
        REQUIRE(registered.IsOk());
        auto acquired = types.Acquire(MetadataType);
        REQUIRE(acquired.IsOk());

        typeName[0] = 'X';
        fieldName[0] = 'Y';
        attributes[0] = 99;
        fields[0].flags = 999;
        source.version = 99;

        const TypeSchema& saved = acquired.Value().Schema();
        CHECK(saved.version == 3);
        CHECK(saved.name.Data() != typeName);
        CHECK(saved.name[0] == 'O');
        REQUIRE(saved.fields.Size() == 1);
        CHECK(saved.fields.Data() != fields);
        CHECK(saved.fields[0].flags == 7);
        CHECK(saved.fields[0].name.Data() != fieldName);
        CHECK(saved.fields[0].name[0] == 'O');
        REQUIRE(saved.fields[0].attributes.Size() == 3);
        CHECK(saved.fields[0].attributes.Data() != attributes);
        CHECK(saved.fields[0].attributes[0] == 17);

        auto cloned = acquired.Value().Clone();
        REQUIRE(cloned.IsOk());
        CHECK(!types.Unregister(registered.Value()).IsOk());
        acquired.Value().Reset();
        CHECK(!types.Unregister(registered.Value()).IsOk());
        CHECK(cloned.Value().Schema().version == 3);
        cloned.Value().Reset();
        REQUIRE(types.Unregister(registered.Value()).IsOk());
        CHECK(!types.Acquire(MetadataType).IsOk());
        CHECK(!types.Unregister(registered.Value()).IsOk());
        REQUIRE(types.Shutdown().IsOk());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(ObjectMetadataRejectsDuplicateTypeAndForeignToken) {
    SystemAllocator allocator;

    {
        TypeRegistry first(allocator);
        TypeRegistry second(allocator);
        auto registered = first.Register(TypeSchema{MetadataType, 1, {}, {}},
                                         MakeRuntimeTypeOps<MetadataProbe>());
        REQUIRE(registered.IsOk());
        CHECK(
            !first
                 .Register(TypeSchema{MetadataType, 2, {}, {}}, MakeRuntimeTypeOps<MetadataProbe>())
                 .IsOk());
        CHECK(first.Stats().registeredTypes == 1);
        CHECK(!second.Unregister(registered.Value()).IsOk());

        auto stillOriginal = first.Acquire(MetadataType);
        REQUIRE(stillOriginal.IsOk());
        CHECK(stillOriginal.Value().Schema().version == 1);
        stillOriginal.Value().Reset();
        REQUIRE(first.Unregister(registered.Value()).IsOk());
        REQUIRE(first.Shutdown().IsOk());
        REQUIRE(second.Shutdown().IsOk());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(ObjectMetadataRegistrationAllocationFailureIsAtomicAndRetryable) {
    SystemAllocator allocator;
    uint64 allocationCount = 0;
    const char typeName[] = "AllocationProbe";
    const char fieldName[] = "Value";
    const uint8 attributes[] = {1, 2, 3, 4};
    const FieldSchema field{Guid{102, 1}, FieldKind::Value, 11,
                            Span<const char>(fieldName, sizeof(fieldName) - 1),
                            Span<const uint8>(attributes, sizeof(attributes))};
    const TypeSchema schema{MetadataType, 1, Span<const FieldSchema>(&field, 1),
                            Span<const char>(typeName, sizeof(typeName) - 1)};

    {
        rocks::test::NFailAllocator fault(allocator);
        TypeRegistry types(fault);
        auto registered = types.Register(schema, MakeRuntimeTypeOps<MetadataProbe>());
        REQUIRE(registered.IsOk());
        allocationCount = fault.Attempts();
        REQUIRE(types.Unregister(registered.Value()).IsOk());
        REQUIRE(types.Shutdown().IsOk());
    }

    REQUIRE(allocationCount != 0);
    CHECK(allocator.Stats().liveAllocations == 0);

    for (uint64 attempt = 1; attempt <= allocationCount; ++attempt) {
        {
            rocks::test::NFailAllocator fault(allocator);
            TypeRegistry types(fault);
            fault.FailOnAttempt(attempt);
            auto rejected = types.Register(schema, MakeRuntimeTypeOps<MetadataProbe>());
            CHECK(!rejected.IsOk());
            CHECK(types.Stats().registeredTypes == 0);
            CHECK(types.Stats().leases == 0);
            CHECK(!types.Acquire(MetadataType).IsOk());

            fault.DisableFailure();
            auto retry = types.Register(schema, MakeRuntimeTypeOps<MetadataProbe>());
            REQUIRE(retry.IsOk());
            auto read = types.Acquire(MetadataType);
            REQUIRE(read.IsOk());
            CHECK(read.Value().Schema().fields[0].attributes[3] == 4);
            read.Value().Reset();
            REQUIRE(types.Unregister(retry.Value()).IsOk());
            REQUIRE(types.Shutdown().IsOk());
        }

        CHECK(allocator.Stats().liveAllocations == 0);
    }
}

TEST(ObjectMetadataShutdownIsBusyUntilLastReaderReturns) {
    SystemAllocator allocator;

    {
        TypeRegistry types(allocator);
        auto registered = types.Register(TypeSchema{MetadataType, 1, {}, {}},
                                         MakeRuntimeTypeOps<MetadataProbe>());
        REQUIRE(registered.IsOk());
        auto lease = types.Acquire(MetadataType);
        REQUIRE(lease.IsOk());
        CHECK(!types.Shutdown().IsOk());
        CHECK(lease.Value().Schema().id == MetadataType);
        lease.Value().Reset();
        REQUIRE(types.Shutdown().IsOk());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(ObjectMetadataRetainsCodeUntilLastReaderAndUnregisterCompletes) {
    SystemAllocator allocator;

    {
        CodeOwner owner;
        TypeRegistry types(allocator);
        owner.registry = &types;
        auto code = ModuleLease::Acquire(&owner, RetainCode, ReleaseCode);
        REQUIRE(code.IsOk());
        auto registered = types.Register(TypeSchema{MetadataType, 1, {}, {}},
                                         MakeRuntimeTypeOps<MetadataProbe>(Move(code.Value())));
        REQUIRE(registered.IsOk());
        CHECK(owner.references == 1);
        CHECK(owner.releases == 0);
        auto reader = types.Acquire(MetadataType);
        REQUIRE(reader.IsOk());

        auto blocked = types.Unregister(registered.Value());
        CHECK(!blocked.IsOk() && blocked.Error() == ErrorCode::Busy);
        CHECK(owner.references == 1);
        CHECK(owner.releases == 0);
        reader.Value().Reset();
        REQUIRE(types.Unregister(registered.Value()).IsOk());
        CHECK(owner.references == 0);
        CHECK(owner.releases == 1);
        CHECK(owner.releaseObservedEmptyRegistry);
        REQUIRE(types.Shutdown().IsOk());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}
