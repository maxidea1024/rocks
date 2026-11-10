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
        // 공통 거부 callback은 타입 정보를 받지 않아 다른 live 타입도 Unsupported다.
        CHECK(cache->write(objects, objects.OwnerAccess(), other.Value().Handle(), input).Error() ==
              ErrorCode::Unsupported);
        CHECK(cache->write(objects, objects.OwnerAccess(), root.Value().Handle(), input).Error() ==
              ErrorCode::Unsupported);
        CHECK(cache->write(objects, ObjectAccessContext{{999, 27}}, root.Value().Handle(), input)
                  .Error() == ErrorCode::WrongContext);
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
