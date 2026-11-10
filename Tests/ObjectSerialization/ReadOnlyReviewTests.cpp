#include "ReflectionFixture.generated.h"
#include <Tests/Support/Test.h>
#include <Core/Memory/SystemAllocator.h>
#include <ObjectSerialization/ObjectGraph.h>

using namespace rocks;
using reflection_fixture::AbstractBase;
using reflection_fixture::PropertyNode;

TEST(RegisteredReadOnlyPolicyRejectsWritableCallbackWithoutChangingStagedWrites) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(GeneratedType<AbstractBase>::Register(types));
        REQUIRE(GeneratedType<reflection_fixture::Base>::Register(types));
        auto schema = GeneratedType<PropertyNode>::Schema();
        FieldSchema fields[4];
        for (usize index = 0; index < 4; ++index) {
            fields[index] = schema.fields[index];
        }
        // 생성 함수표의 runtimeCache setter는 writable이다. 소비자가 schema만 읽기 전용으로 바꿔
        // 등록하는 실수도 등록 후에는 차단되어야 한다.
        const Guid cacheId{0xad10000000000000ull, 0x43};
        bool found = false;
        for (usize index = 0; index < 4; ++index) {
            if (fields[index].id == cacheId) {
                fields[index].flags |= static_cast<uint32>(FieldFlags::ReadOnly);
                found = true;
            }
        }
        REQUIRE(found);
        schema.fields = fields;
        auto original = GeneratedType<PropertyNode>::Ops();
        REQUIRE(types.Register(schema, Move(original)));
        ObjectRegistry objects(allocator, types, {713, 27});
        auto lease = types.Acquire(schema.id);
        REQUIRE(lease);
        const RuntimeFieldOps* cache = nullptr;
        for (usize index = 0; index < lease.Value().Ops().fields.Size(); ++index) {
            if (lease.Value().Ops().fields[index].fieldId == cacheId) {
                cache = &lease.Value().Ops().fields[index];
            }
        }
        REQUIRE(cache);
        FieldValue input(allocator);
        input.kind = ValueKind::Int32;
        input.signedInteger = 123;
        ConstructionContext construction(objects, objects.OwnerAccess());
        auto staged = construction.Create<PropertyNode>(schema.id);
        REQUIRE(staged);
        REQUIRE(cache->writeStaged(construction, staged.Value(), input));
        auto root = construction.Commit();
        REQUIRE(root);
        input.signedInteger = 999;
        auto other = objects.NewObject<reflection_fixture::Base>(
            objects.OwnerAccess(), GeneratedType<reflection_fixture::Base>::Id());
        REQUIRE(other);
        // 다른 native 타입의 유효 객체는 읽기 전용 여부보다 타입 오류를 먼저 보고한다.
        CHECK(cache->write(objects, objects.OwnerAccess(), other.Value().Handle(), input).Error() ==
              ErrorCode::TypeMismatch);
        CHECK(cache->write(objects, objects.OwnerAccess(), root.Value().Handle(), input).Error() ==
              ErrorCode::Unsupported);
        CHECK(cache->write(objects, ObjectAccessContext{{999, 27}}, root.Value().Handle(), input)
                  .Error() == ErrorCode::WrongContext);
        input.kind = ValueKind::Bool;
        CHECK(cache->write(objects, objects.OwnerAccess(), root.Value().Handle(), input).Error() ==
              ErrorCode::TypeMismatch);
        CHECK(cache->write(objects, ObjectAccessContext{{999, 27}}, root.Value().Handle(), input)
                  .Error() == ErrorCode::WrongContext);
        input.kind = ValueKind::Int32;
        FieldValue output(allocator);
        REQUIRE(cache->read(objects, objects.OwnerAccess(), root.Value().Handle(), output));
        CHECK(output.signedInteger == 123);
        CHECK(cache->write(objects, objects.OwnerAccess(), {}, input).Error() ==
              ErrorCode::StaleHandle);
        REQUIRE(objects.RequestDestroy(objects.OwnerAccess(), root.Value().Handle()));
        CHECK(cache->write(objects, objects.OwnerAccess(), root.Value().Handle(), input).Error() ==
              ErrorCode::Destroyed);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(RegisteredFieldWriteUsesActualObjectSchemaAcrossRegistriesAndAliases) {
    SystemAllocator allocator;
    {
        TypeRegistry first(allocator);
        TypeRegistry second(allocator);
        REQUIRE(GeneratedType<AbstractBase>::Register(first));
        REQUIRE(GeneratedType<AbstractBase>::Register(second));
        auto writableSchema = GeneratedType<PropertyNode>::Schema();
        REQUIRE(first.Register(writableSchema, GeneratedType<PropertyNode>::Ops()));
        FieldSchema readonlyFields[4];
        const Guid cacheId{0xad10000000000000ull, 0x43};
        for (usize index = 0; index < 4; ++index) {
            readonlyFields[index] = writableSchema.fields[index];
            if (readonlyFields[index].id == cacheId) {
                readonlyFields[index].flags |= static_cast<uint32>(FieldFlags::ReadOnly);
            }
        }
        auto readonlySchema = writableSchema;
        readonlySchema.fields = readonlyFields;
        REQUIRE(second.Register(readonlySchema, GeneratedType<PropertyNode>::Ops()));
        // 같은 C++ native 타입을 같은 레지스트리에 별도 ID로도 등록한다.
        const TypeId aliasId{0xad10000000000000ull, 0x99};
        readonlySchema.id = aliasId;
        REQUIRE(first.Register(readonlySchema, GeneratedType<PropertyNode>::Ops()));
        // 수동으로 구성된 다른 schema의 필드 누락·표현 불일치도 호출 전에 거부한다.
        const TypeId missingId{0xad10000000000000ull, 0x9a};
        const TypeId kindId{0xad10000000000000ull, 0x9b};
        const Guid renamedField{0xad10000000000000ull, 0x9c};
        FieldSchema missingFields[4];
        FieldSchema kindFields[4];
        RuntimeFieldOps missingOps[4];
        auto alteredOps = GeneratedType<PropertyNode>::Ops();
        for (usize index = 0; index < 4; ++index) {
            missingFields[index] = writableSchema.fields[index];
            kindFields[index] = writableSchema.fields[index];
            missingOps[index] = alteredOps.fields[index];
            if (missingFields[index].id == cacheId) {
                missingFields[index].id = renamedField;
                kindFields[index].valueKind = ValueKind::UInt32;
            }
            if (missingOps[index].fieldId == cacheId) {
                missingOps[index].fieldId = renamedField;
            }
        }
        auto missingSchema = writableSchema;
        missingSchema.id = missingId;
        missingSchema.fields = missingFields;
        alteredOps.fields = missingOps;
        REQUIRE(first.Register(missingSchema, Move(alteredOps)));
        auto kindSchema = writableSchema;
        kindSchema.id = kindId;
        kindSchema.fields = kindFields;
        REQUIRE(first.Register(kindSchema, GeneratedType<PropertyNode>::Ops()));
        ObjectRegistry firstObjects(allocator, first, {715, 1});
        ObjectRegistry secondObjects(allocator, second, {715, 2});
        auto writable =
            firstObjects.NewObject<PropertyNode>(firstObjects.OwnerAccess(), writableSchema.id);
        auto alias = firstObjects.NewObject<PropertyNode>(firstObjects.OwnerAccess(), aliasId);
        auto foreign =
            secondObjects.NewObject<PropertyNode>(secondObjects.OwnerAccess(), writableSchema.id);
        auto missing = firstObjects.NewObject<PropertyNode>(firstObjects.OwnerAccess(), missingId);
        auto kind = firstObjects.NewObject<PropertyNode>(firstObjects.OwnerAccess(), kindId);
        REQUIRE(writable && alias && foreign && missing && kind);
        auto writableLease = first.Acquire(writableSchema.id);
        auto readonlyLease = second.Acquire(writableSchema.id);
        REQUIRE(writableLease && readonlyLease);
        const RuntimeFieldOps* source = nullptr;
        const RuntimeFieldOps* readonly = nullptr;
        for (usize index = 0; index < writableLease.Value().Ops().fields.Size(); ++index) {
            if (writableLease.Value().Ops().fields[index].fieldId == cacheId) {
                source = &writableLease.Value().Ops().fields[index];
                readonly = &readonlyLease.Value().Ops().fields[index];
            }
        }
        REQUIRE(source && readonly && readonly->fieldId == cacheId);
        FieldValue value(allocator);
        value.kind = ValueKind::Int32;
        value.signedInteger = 123;
        REQUIRE(source->write(firstObjects, firstObjects.OwnerAccess(), writable.Value().Handle(),
                              value));
        CHECK(source->write(firstObjects, firstObjects.OwnerAccess(), alias.Value().Handle(), value)
                  .Error() == ErrorCode::Unsupported);
        CHECK(
            source
                ->write(secondObjects, secondObjects.OwnerAccess(), foreign.Value().Handle(), value)
                .Error() == ErrorCode::Unsupported);
        // 소스가 읽기 전용인 경우 대상이 writable이어도 권한을 확대하지 않는다.
        CHECK(
            readonly
                ->write(firstObjects, firstObjects.OwnerAccess(), writable.Value().Handle(), value)
                .Error() == ErrorCode::Unsupported);
        // 대상 스키마에 없는 필드는 NotFound, 실제 스키마의 다른 값 종류는 TypeMismatch다.
        CHECK(
            source->write(firstObjects, firstObjects.OwnerAccess(), missing.Value().Handle(), value)
                .Error() == ErrorCode::NotFound);
        CHECK(source->write(firstObjects, firstObjects.OwnerAccess(), kind.Value().Handle(), value)
                  .Error() == ErrorCode::TypeMismatch);
        FieldValue output(allocator);
        REQUIRE(source->read(secondObjects, secondObjects.OwnerAccess(), foreign.Value().Handle(),
                             output));
        CHECK(output.signedInteger == 73);
        REQUIRE(
            source->read(firstObjects, firstObjects.OwnerAccess(), alias.Value().Handle(), output));
        CHECK(output.signedInteger == 73);
        REQUIRE(source->read(firstObjects, firstObjects.OwnerAccess(), missing.Value().Handle(),
                             output));
        CHECK(output.signedInteger == 73);
        REQUIRE(
            source->read(firstObjects, firstObjects.OwnerAccess(), kind.Value().Handle(), output));
        CHECK(output.signedInteger == 73);
        value.kind = ValueKind::Bool;
        CHECK(
            source->write(firstObjects, firstObjects.OwnerAccess(), missing.Value().Handle(), value)
                .Error() == ErrorCode::NotFound);
        CHECK(source
                  ->write(firstObjects, ObjectAccessContext{{999, 1}}, writable.Value().Handle(),
                          value)
                  .Error() == ErrorCode::WrongContext);
        CHECK(
            source
                ->write(firstObjects, firstObjects.OwnerAccess(), writable.Value().Handle(), value)
                .Error() == ErrorCode::TypeMismatch);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}
