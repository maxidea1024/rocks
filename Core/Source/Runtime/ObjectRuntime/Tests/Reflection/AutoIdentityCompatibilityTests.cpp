#include <Object/GeneratedFieldCodec.h>
#include <ReflectionTestFixtures/AutoIdentityFixture.generated.h>
#include <TestSupport/Test.h>
#include <Core/Memory/SystemAllocator.h>
#include <Object/GeneratedFieldAccess.h>
#include <ObjectSerialization/ObjectGraph.h>

using namespace rocks;

namespace {
constexpr Guid SavedTypeId{0xc010000000000000ull, 1};
constexpr Guid SavedFieldId{0xc010000000000000ull, 2};

// 이름 변경 전 사용자가 생성하던 네이티브 schema의 독립 재현이다. 새로운 생성 코드에서
// ID를 가져오지 않아, migration이 새 ID를 잘못 발급해도 테스트가 실패한다.
struct OriginalActor final : Object {
    int32 health = 0;
};

Result<TypeRegistrationToken> RegisterOriginal(TypeRegistry& types) noexcept {
    FieldSchema field;
    field.id = SavedFieldId;
    field.name = {"health", 6};
    field.value.kind = ValueKind::Int32;
    const RuntimeFieldOps accessors[] = {
        {SavedFieldId,
         [](ObjectRegistry& registry, const ObjectAccessContext& access, ObjectHandle object,
            FieldValue& value) noexcept {
             return detail::ReadGeneratedField<OriginalActor>(registry, access, object,
                                                              &OriginalActor::health, value);
         },
         [](ObjectRegistry& registry, const ObjectAccessContext& access, ObjectHandle object,
            const FieldValue& value) noexcept {
             return detail::WriteGeneratedField<OriginalActor>(registry, access, object,
                                                               &OriginalActor::health, value);
         },
         [](ConstructionContext& transaction, ObjectHandle object,
            const FieldValue& value) noexcept {
             return detail::WriteGeneratedStagedField<OriginalActor>(transaction, object,
                                                                     &OriginalActor::health, value);
         },
         &detail::EncodeThroughFieldValue, &detail::DecodeThroughFieldValue}};
    auto ops = MakeRuntimeTypeOps<OriginalActor>();
    ops.fields = accessors;
    ops.completeFieldLayout = true;
    return types.Register({SavedTypeId, 1, {&field, 1}, {"OriginalActor", 11}}, Move(ops));
}

bool SameBytes(Span<const uint8> left, Span<const uint8> right) noexcept {
    if (left.Size() != right.Size()) {
        return false;
    }
    for (usize index = 0; index < left.Size(); ++index) {
        if (left[index] != right[index]) {
            return false;
        }
    }
    return true;
}
} // namespace

TEST(AutoIdentityRenamedClassAndFieldPreserveSavedObjectGraphWire) {
    SystemAllocator allocator;
    {
        TypeRegistry oldTypes(allocator);
        TypeRegistry newTypes(allocator);
        REQUIRE(RegisterOriginal(oldTypes));
        REQUIRE(GeneratedType<identity_fixture::RenamedActor>::Register(newTypes));
        CHECK(GeneratedType<identity_fixture::RenamedActor>::Id() == SavedTypeId);
        ObjectRegistry oldObjects(allocator, oldTypes, {913, 1});
        ObjectRegistry newObjects(allocator, newTypes, {913, 2});
        auto original = oldObjects.NewObject<OriginalActor>(oldObjects.OwnerAccess(), SavedTypeId);
        REQUIRE(original);
        {
            auto borrowed = oldObjects.Resolve<OriginalActor>(oldObjects.OwnerAccess(),
                                                              original.Value().Handle());
            REQUIRE(borrowed);
            auto object = borrowed.Value().Get();
            REQUIRE(object);
            object.Value()->health = -1234567;
        }
        auto saved = SaveObjectGraph(allocator, oldObjects, oldObjects.OwnerAccess(),
                                     original.Value().Handle());
        REQUIRE(saved);

        // 파일에 실제 쓰인 타입 ID, 레코드 필드 ID와 payload를 별도 reader로 검사한다.
        auto archive = ArchiveReader::Create(saved.Value().View());
        REQUIRE(archive);
        auto magic = archive.Value().ReadRaw(8);
        REQUIRE(magic);
        const uint8 expectedMagic[] = {'R', 'O', 'C', 'K', 'S', 'G', 'R', 'F'};
        CHECK(SameBytes(magic.Value(), expectedMagic));
        auto format = archive.Value().ReadU32();
        auto count = archive.Value().ReadU32();
        auto root = archive.Value().ReadU32();
        auto local = archive.Value().ReadU64();
        auto type = archive.Value().ReadGuid();
        auto schemaVersion = archive.Value().ReadU32();
        auto recordBytes = archive.Value().ReadBytes();
        REQUIRE(format && count && root && local && type && schemaVersion && recordBytes);
        CHECK(format.Value() == 1 && count.Value() == 1 && root.Value() == 1 &&
              local.Value() == 1 && type.Value() == SavedTypeId && schemaVersion.Value() == 1);
        CHECK(archive.Value().AtEnd());
        auto record = RecordReader::Open(allocator, recordBytes.Value(), SavedTypeId, 1, 1);
        REQUIRE(record);
        CHECK(record.Value().FieldCount() == 1);
        auto field = record.Value().Find(SavedFieldId);
        REQUIRE(field);
        CHECK(field.Value().wireType == static_cast<uint32>(WireType::I64));
        auto payload = ArchiveReader::Create(field.Value().payload);
        REQUIRE(payload);
        auto health = payload.Value().ReadI64();
        REQUIRE(health);
        CHECK(health.Value() == -1234567 && payload.Value().AtEnd());

        auto loaded = LoadObjectGraph(allocator, newObjects, newTypes, newObjects.OwnerAccess(),
                                      saved.Value().View());
        REQUIRE(loaded);
        {
            auto borrowed = newObjects.Resolve<identity_fixture::RenamedActor>(
                newObjects.OwnerAccess(), loaded.Value().Handle());
            REQUIRE(borrowed);
            auto actor = borrowed.Value().Get();
            REQUIRE(actor);
            CHECK(actor.Value()->hitPoints == -1234567);
        }
        auto resaved = SaveObjectGraph(allocator, newObjects, newObjects.OwnerAccess(),
                                       loaded.Value().Handle());
        REQUIRE(resaved);
        CHECK(SameBytes(saved.Value().View(), resaved.Value().View()));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
    CHECK(allocator.Stats().liveBytes == 0);
}
