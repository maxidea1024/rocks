#include "ReflectionFixture.generated.h"
#include <TestSupport/Test.h>
#include <Core/Memory/SystemAllocator.h>
#include <ObjectSerialization/ObjectGraph.h>

using namespace rocks;
using reflection_fixture::AbstractBase;
using reflection_fixture::AbstractByPolicy;
using reflection_fixture::PropertyNode;

namespace {
constexpr Guid PropertyId(uint64 low) noexcept {
    return {0xad10000000000000ull, low};
}

bool RegisterProperties(TypeRegistry& types) noexcept {
    return GeneratedType<AbstractBase>::Register(types).IsOk() &&
           GeneratedType<PropertyNode>::Register(types).IsOk() &&
           GeneratedType<AbstractByPolicy>::Register(types).IsOk();
}

const RuntimeFieldOps& Property(const TypeLease& lease, uint64 low) noexcept {
    for (usize index = 0; index < lease.Ops().fields.Size(); ++index) {
        if (lease.Ops().fields[index].fieldId == PropertyId(low))
            return lease.Ops().fields[index];
    }
    platform::Fatal("Missing generated property");
}

Result<Array<uint8>> SingleRecordGraph(Allocator& allocator, TypeId type,
                                       Span<const uint8> record) noexcept {
    ArchiveWriter writer(allocator);
    const uint8 magic[] = {'R', 'O', 'C', 'K', 'S', 'G', 'R', 'F'};
    // 테스트는 wire 명세로 envelope를 직접 구성한다. Save 구현의 판단을 재사용하지 않는다.
    if (!writer.WriteRaw({magic, 8}) || !writer.WriteU32(1) || !writer.WriteU32(1) ||
        !writer.WriteU32(1) || !writer.WriteU64(1) || !writer.WriteGuid(type) ||
        !writer.WriteU32(1) || !writer.WriteBytes(record)) {
        return Result<Array<uint8>>::Failure(ErrorCode::OutOfMemory);
    }
    return Result<Array<uint8>>::Success(writer.TakeBytes());
}
} // namespace

TEST(ReflectionClassPropertiesPrivateInheritanceAndReadOnlyStagedPolicy) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterProperties(types));
        ObjectRegistry objects(allocator, types, {711, 1});
        auto lease = types.Acquire(GeneratedType<PropertyNode>::Id());
        REQUIRE(lease);
        CHECK(lease.Value().IsA(GeneratedType<AbstractBase>::Id()));
        CHECK(!HasFlag(lease.Value().Schema().flags, TypeFlags::Abstract));
        const auto& field = lease.Value().Schema().fields[0];
        CHECK(field.id == PropertyId(0x31));
        CHECK(HasFlag(field.flags, FieldFlags::ReadOnly));
        CHECK(StringView(field.category.Data(), field.category.Size()) == StringView("Stats"));
        CHECK(StringView(field.displayName.Data(), field.displayName.Size()) ==
              StringView("\xEC\xB2\xB4\xEB\xA0\xA5"));
        FieldValue value(allocator);
        value.kind = ValueKind::Int32;
        value.signedInteger = 123;
        ConstructionContext construction(objects, objects.OwnerAccess());
        auto staged = construction.Create<PropertyNode>(GeneratedType<PropertyNode>::Id());
        REQUIRE(staged);
        REQUIRE(Property(lease.Value(), 0x31).writeStaged(construction, staged.Value(), value));
        auto root = construction.Commit();
        REQUIRE(root);
        value.signedInteger = 456;
        CHECK(Property(lease.Value(), 0x31)
                  .write(objects, objects.OwnerAccess(), root.Value().Handle(), value)
                  .Error() == ErrorCode::Unsupported);
        CHECK(Property(lease.Value(), 0x31)
                  .write(objects, ObjectAccessContext{{999, 1}}, root.Value().Handle(), value)
                  .Error() == ErrorCode::WrongContext);
        FieldValue output(allocator);
        REQUIRE(Property(lease.Value(), 0x31)
                    .read(objects, objects.OwnerAccess(), root.Value().Handle(), output));
        CHECK(output.signedInteger == 123);
        auto saved =
            SaveObjectGraph(allocator, objects, objects.OwnerAccess(), root.Value().Handle());
        REQUIRE(saved);
        ObjectRegistry restored(allocator, types, {711, 2});
        auto loaded = LoadObjectGraph(allocator, restored, types, restored.OwnerAccess(),
                                      {saved.Value().Data(), saved.Value().Size()});
        REQUIRE(loaded);
        REQUIRE(Property(lease.Value(), 0x31)
                    .read(restored, restored.OwnerAccess(), loaded.Value().Handle(), output));
        CHECK(output.signedInteger == 123);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(ReflectionDisplayNamePreservesUtf8ControlsAndEmbeddedNull) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterProperties(types));
        auto lease = types.Acquire(GeneratedType<PropertyNode>::Id());
        REQUIRE(lease);
        const FieldSchema* found = nullptr;
        for (usize index = 0; index < lease.Value().Schema().fields.Size(); ++index) {
            const auto& field = lease.Value().Schema().fields[index];
            if (field.id == PropertyId(0x43)) {
                found = &field;
            }
        }
        REQUIRE(found);
        const char expected[] = "한글\"\\\n\0007";
        CHECK(found->displayName.Size() == sizeof(expected) - 1);
        CHECK(StringView(found->displayName.Data(), found->displayName.Size()) ==
              StringView(expected, sizeof(expected) - 1));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(ReflectionTransientReferenceTracesButIsNotSavedAndDefaultsSurviveLoad) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterProperties(types));
        ObjectRegistry objects(allocator, types, {712, 1});
        auto lease = types.Acquire(GeneratedType<PropertyNode>::Id());
        auto root = objects.NewObject<PropertyNode>(objects.OwnerAccess(),
                                                    GeneratedType<PropertyNode>::Id());
        auto child = objects.NewObject<PropertyNode>(objects.OwnerAccess(),
                                                     GeneratedType<PropertyNode>::Id());
        REQUIRE(lease && root && child);
        const auto childHandle = child.Value().Handle();
        FieldValue value(allocator);
        value.kind = ValueKind::ObjectReference;
        value.reference = childHandle;
        REQUIRE(Property(lease.Value(), 0x41)
                    .write(objects, objects.OwnerAccess(), root.Value().Handle(), value));
        value.kind = ValueKind::Int32;
        value.signedInteger = 999;
        REQUIRE(Property(lease.Value(), 0x43)
                    .write(objects, objects.OwnerAccess(), root.Value().Handle(), value));
        child.Value().Reset();
        REQUIRE(objects.Collect(objects.OwnerAccess()));
        CHECK(objects.Stats().liveObjects ==
              2); // Transient라도 strong edge는 GC에 반드시 참여한다.
        auto saved =
            SaveObjectGraph(allocator, objects, objects.OwnerAccess(), root.Value().Handle());
        REQUIRE(saved);
        ObjectRegistry restored(allocator, types, {712, 2});
        auto loaded = LoadObjectGraph(allocator, restored, types, restored.OwnerAccess(),
                                      {saved.Value().Data(), saved.Value().Size()});
        REQUIRE(loaded);
        CHECK(restored.Stats().liveObjects ==
              1); // 저장 도달성에서는 transient-only 자식이 제외된다.
        FieldValue output(allocator);
        REQUIRE(Property(lease.Value(), 0x41)
                    .read(restored, restored.OwnerAccess(), loaded.Value().Handle(), output));
        CHECK(output.reference.IsNull());
        REQUIRE(Property(lease.Value(), 0x43)
                    .read(restored, restored.OwnerAccess(), loaded.Value().Handle(), output));
        CHECK(output.signedInteger == 73);
        value.kind = ValueKind::ObjectReference;
        value.reference = {};
        REQUIRE(Property(lease.Value(), 0x41)
                    .write(objects, objects.OwnerAccess(), root.Value().Handle(), value));
        REQUIRE(objects.Collect(objects.OwnerAccess()));
        CHECK(objects.Stats().liveObjects == 1);
        CHECK(!objects.TryPin(objects.OwnerAccess(), childHandle));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(ReflectionTransientWireAndAbstractGraphAreRejectedBeforeConstructors) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterProperties(types));
        ObjectRegistry objects(allocator, types, {713, 1});
        AbstractBase::constructions = 0;
        AbstractByPolicy::constructions = 0;
        CHECK(objects
                  .NewObject<AbstractBase>(objects.OwnerAccess(), GeneratedType<AbstractBase>::Id())
                  .Error() == ErrorCode::Unsupported);
        CHECK(objects
                  .NewObject<AbstractByPolicy>(objects.OwnerAccess(),
                                               GeneratedType<AbstractByPolicy>::Id())
                  .Error() == ErrorCode::Unsupported);
        CHECK(AbstractBase::constructions == 0 && AbstractByPolicy::constructions == 0);
        const TypeId abstractTypes[] = {GeneratedType<AbstractBase>::Id(),
                                        GeneratedType<AbstractByPolicy>::Id()};
        for (TypeId type : abstractTypes) {
            RecordBuilder builder(allocator, type, 1);
            auto record = builder.Build();
            REQUIRE(record);
            auto graph =
                SingleRecordGraph(allocator, type, {record.Value().Data(), record.Value().Size()});
            REQUIRE(graph);
            CHECK(LoadObjectGraph(allocator, objects, types, objects.OwnerAccess(),
                                  {graph.Value().Data(), graph.Value().Size()})
                      .Error() == ErrorCode::Unsupported);
        }
        RecordBuilder transient(allocator, GeneratedType<PropertyNode>::Id(), 1);
        ArchiveWriter payload(allocator);
        REQUIRE(payload.WriteI64(777));
        REQUIRE(transient.AddField(PropertyId(0x43), WireType::I64, payload.View()));
        auto record = transient.Build();
        REQUIRE(record);
        auto graph = SingleRecordGraph(allocator, GeneratedType<PropertyNode>::Id(),
                                       {record.Value().Data(), record.Value().Size()});
        REQUIRE(graph);
        ObjectGraphLoadOptions options;
        options.allowMissingFields = true;
        CHECK(LoadObjectGraph(allocator, objects, types, objects.OwnerAccess(),
                              {graph.Value().Data(), graph.Value().Size()}, options)
                  .Error() == ErrorCode::Unsupported);
        CHECK(AbstractBase::constructions == 0 && AbstractByPolicy::constructions == 0);
        CHECK(objects.Stats().liveObjects == 0 && objects.Stats().reservedObjects == 0);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(ReflectionPropertyDisplayMetadataIsDeepCopiedAndUnknownFlagsAreRejected) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(GeneratedType<AbstractBase>::Register(types));
        TypeSchema schema = GeneratedType<PropertyNode>::Schema();
        FieldSchema fields[4];
        for (usize index = 0; index < 4; ++index)
            fields[index] = schema.fields[index];
        char category[] = "Custom";
        char display[] = "Health";
        fields[0].category = {category, 6};
        fields[0].displayName = {display, 6};
        schema.fields = {fields, 4};
        auto registration = types.Register(schema, GeneratedType<PropertyNode>::Ops());
        REQUIRE(registration);
        category[0] = 'X';
        display[0] = 'X';
        auto lease = types.Acquire(schema.id);
        REQUIRE(lease);
        const auto& copied = lease.Value().Schema().fields[0];
        CHECK(StringView(copied.category.Data(), copied.category.Size()) == StringView("Custom"));
        CHECK(StringView(copied.displayName.Data(), copied.displayName.Size()) ==
              StringView("Health"));
        lease.Value().Reset();
        REQUIRE(types.Unregister(registration.Value()));
        fields[0].flags |= 0x80000000u;
        CHECK(types.Register(schema, GeneratedType<PropertyNode>::Ops()).Error() ==
              ErrorCode::InvalidArgument);
        schema = GeneratedType<PropertyNode>::Schema();
        schema.flags = 0x80000000u;
        CHECK(types.Register(schema, GeneratedType<PropertyNode>::Ops()).Error() ==
              ErrorCode::InvalidArgument);
        schema.flags = static_cast<uint32>(TypeFlags::Abstract);
        CHECK(types.Register(schema, GeneratedType<PropertyNode>::Ops()).Error() ==
              ErrorCode::InvalidArgument);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(ReflectionAutomaticIdentityProvidesPrivateFieldOperations) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(GeneratedType<reflection_fixture::AliasPrivate>::Register(types));
        ObjectRegistry objects(allocator, types, {714, 2});
        auto created = objects.NewObject<reflection_fixture::AliasPrivate>(
            objects.OwnerAccess(), GeneratedType<reflection_fixture::AliasPrivate>::Id());
        REQUIRE(created);
        auto lease = types.Acquire(GeneratedType<reflection_fixture::AliasPrivate>::Id());
        REQUIRE(lease);
        FieldValue value(allocator);
        REQUIRE(Property(lease.Value(), 0x51)
                    .read(objects, objects.OwnerAccess(), created.Value().Handle(), value));
        CHECK(value.kind == ValueKind::Int32);
        CHECK(value.signedInteger == 41);
        value.signedInteger = 92;
        REQUIRE(Property(lease.Value(), 0x51)
                    .write(objects, objects.OwnerAccess(), created.Value().Handle(), value));
        auto borrowed = objects.Resolve<reflection_fixture::AliasPrivate>(objects.OwnerAccess(),
                                                                          created.Value().Handle());
        REQUIRE(borrowed);
        auto object = borrowed.Value().Get();
        REQUIRE(object);
        CHECK(object.Value()->Read() == 92);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}
