#include <ReflectionTestFixtures/StructFixture.generated.h>
#include <ReflectionTestFixtures/SubobjectFixture.generated.h>
#include <Core/Memory/SystemAllocator.h>
#include <Object/TypeRegistry.h>
#include <TestSupport/Test.h>

// 선언 subobject 표의 생성·등록 시험이다(Docs/Design/Prefab 10.2·10.8절 PF1a): MetaCompiler가 낸
// 펼친 표(base 표 + 자기 항목, 자기 Root가 맨 앞)·소유 구조 지정자·연산표, 그리고 등록 검사(표
// 모양·base 앞부분·지정자 상속·연산표 일치·상한·struct 거부)다.
using namespace rocks;
using reflection_fixture::SubobjectOwner;
using reflection_fixture::SubobjectOwnerChild;
using reflection_fixture::SubobjectPart;
using reflection_fixture::SubobjectScenePart;

namespace subobject_schema_tests_file {

namespace {

// 별칭 등록용 TypeId다(원본 스키마를 복사해 다른 ID로 등록한다 — 도구 이름은 비운다).
constexpr TypeId AliasType{0x5355424F424A4543ULL, 1};
constexpr TypeId AliasChildType{0x5355424F424A4543ULL, 2};

Guid FieldIdOf(const TypeSchema& schema, const char* name) noexcept {
    for (const FieldSchema& field : schema.fields) {
        if (StringView(field.name.Data(), field.name.Size()) == StringView(name)) {
            return field.id;
        }
    }

    return Guid{};
}

bool RegisterParts(TypeRegistry& types) noexcept {
    return GeneratedType<SubobjectPart>::Register(types) &&
           GeneratedType<SubobjectScenePart>::Register(types);
}

// 원본 스키마의 별칭이다(id·도구 이름만 바꾼다).
TypeSchema Alias(const TypeSchema& schema, TypeId id) noexcept {
    TypeSchema alias = schema;
    alias.id = id;
    alias.toolName = {};
    return alias;
}

// 시험 준비(부품 등록) 실패를 나타내는 값이다(등록 검사가 내지 않는 오류).
constexpr ErrorCode SetupFailed = ErrorCode::WrongContext;

template <class T>
ErrorCode ErrorOf(const Result<T>& result) noexcept {
    return result ? ErrorCode::None : result.Error();
}

} // namespace

TEST(DefaultSubobject_TableGeneratedWithStableGuid) {
    const TypeSchema& owner = GeneratedType<SubobjectOwner>::Schema();
    REQUIRE(owner.subobjects.Size() == 3);
    // 자기 Root가 맨 앞, 나머지는 선언 순서. ID는 소유 필드의 FieldId(= SubobjectGuid)다.
    CHECK(owner.subobjects[0].id == FieldIdOf(owner, "body"));
    CHECK(owner.subobjects[1].id == FieldIdOf(owner, "arm"));
    CHECK(owner.subobjects[2].id == FieldIdOf(owner, "plain"));
    CHECK(owner.subobjects[0].flags == static_cast<uint32>(SubobjectFlags::Root));
    CHECK(owner.subobjects[0].attachParent == -1);
    CHECK(owner.subobjects[1].attachParent == 0);
    CHECK(owner.subobjects[2].attachParent == -1);
    CHECK(owner.subobjects[0].type == TypeIdOf<SubobjectScenePart>());
    CHECK(owner.subobjects[2].type == TypeIdOf<SubobjectPart>());
    CHECK(owner.subobjectListField == FieldIdOf(owner, "parts"));
    CHECK(owner.subobjectRootField == FieldIdOf(owner, "root"));
    CHECK(owner.subobjectAttachField == Guid{});
    // 소유 필드는 암묵 ReadOnly다.
    for (const FieldSchema& field : owner.fields) {
        if (field.id == owner.subobjects[0].id) {
            CHECK(HasFlag(field.flags, FieldFlags::ReadOnly));
        }
    }

    // 파생은 base 표를 앞부분으로 그대로 싣고 지정자를 이어받는다. hand는 도구 이름(Name)이 객체
    // 이름이고 Root에 붙는다.
    const TypeSchema& child = GeneratedType<SubobjectOwnerChild>::Schema();
    REQUIRE(child.subobjects.Size() == 4);
    for (usize index = 0; index < owner.subobjects.Size(); ++index) {
        CHECK(child.subobjects[index].id == owner.subobjects[index].id);
        CHECK(child.subobjects[index].attachParent == owner.subobjects[index].attachParent);
    }

    CHECK(child.subobjects[3].id == FieldIdOf(child, "hand_"));
    CHECK(child.subobjects[3].attachParent == 0);
    CHECK(child.subobjectListField == owner.subobjectListField);
    CHECK(child.subobjectRootField == owner.subobjectRootField);
    const TypeSchema& scene = GeneratedType<SubobjectScenePart>::Schema();
    CHECK(scene.subobjectAttachField == FieldIdOf(scene, "parent"));
    CHECK(scene.subobjects.Size() == 0);

    // 연산표: 표와 같은 수·순서, 자기 hook만.
    const RuntimeTypeOps ownerOps = GeneratedType<SubobjectOwner>::Ops();
    REQUIRE(ownerOps.subobjects.Size() == 3);
    CHECK(ownerOps.subobjects[1].id == owner.subobjects[1].id);
    CHECK(ownerOps.subobjects[1].field != nullptr);
    CHECK(ownerOps.initSubobjectDefaults == &SubobjectOwner::InitSubobjectDefaults);
    const RuntimeTypeOps childOps = GeneratedType<SubobjectOwnerChild>::Ops();
    CHECK(childOps.subobjects.Size() == 4);
    CHECK(childOps.initSubobjectDefaults == &SubobjectOwnerChild::InitSubobjectDefaults);
}

TEST(DefaultSubobject_RegistrationKeepsTable) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterParts(types));
    REQUIRE(GeneratedType<SubobjectOwner>::Register(types));
    REQUIRE(GeneratedType<SubobjectOwnerChild>::Register(types));
    auto lease = types.Acquire(TypeIdOf<SubobjectOwnerChild>());
    REQUIRE(lease.IsOk());
    const TypeSchema& registered = lease.Value().Schema();
    REQUIRE(registered.subobjects.Size() == 4);
    CHECK(registered.subobjects[3].id ==
          GeneratedType<SubobjectOwnerChild>::Schema().subobjects[3].id);
    CHECK(registered.subobjectListField ==
          GeneratedType<SubobjectOwner>::Schema().subobjectListField);
    // 등록 복사본은 입력 배열을 빌리지 않는다.
    CHECK(registered.subobjects.Data() !=
          GeneratedType<SubobjectOwnerChild>::Schema().subobjects.Data());
}

TEST(DefaultSubobject_RegistrationRejectsMalformedTables) {
    SystemAllocator allocator;
    const TypeSchema& owner = GeneratedType<SubobjectOwner>::Schema();
    SubobjectSchema entries[3] = {owner.subobjects[0], owner.subobjects[1], owner.subobjects[2]};
    const auto attempt = [&](const TypeSchema& schema, RuntimeTypeOps ops) noexcept {
        TypeRegistry types(allocator);
        if (!RegisterParts(types)) {
            return SetupFailed;
        }

        return ErrorOf(types.Register(schema, Move(ops)));
    };

    // 손대지 않은 별칭은 받는다(아래 실패가 별칭 자체 때문이 아니다).
    CHECK(attempt(Alias(owner, AliasType), GeneratedType<SubobjectOwner>::Ops()) ==
          ErrorCode::None);

    // 부착 부모가 앞쪽 항목이 아니다.
    entries[1].attachParent = 2;
    TypeSchema forward = Alias(owner, AliasType);
    forward.subobjects = Span<const SubobjectSchema>(entries, 3);
    CHECK(attempt(forward, GeneratedType<SubobjectOwner>::Ops()) == ErrorCode::InvalidArgument);
    entries[1] = owner.subobjects[1];

    // Root 둘.
    entries[2].flags = static_cast<uint32>(SubobjectFlags::Root);
    TypeSchema twoRoots = Alias(owner, AliasType);
    twoRoots.subobjects = Span<const SubobjectSchema>(entries, 3);
    CHECK(attempt(twoRoots, GeneratedType<SubobjectOwner>::Ops()) == ErrorCode::InvalidArgument);
    entries[2] = owner.subobjects[2];

    // 항목 ID가 이 타입의 ObjectRef 필드가 아니다(소유 목록 필드).
    entries[2].id = owner.subobjectListField;
    TypeSchema notOwnerField = Alias(owner, AliasType);
    notOwnerField.subobjects = Span<const SubobjectSchema>(entries, 3);
    CHECK(attempt(notOwnerField, GeneratedType<SubobjectOwner>::Ops()) ==
          ErrorCode::InvalidArgument);
    entries[2] = owner.subobjects[2];

    // 같은 ID 둘.
    entries[2].id = entries[1].id;
    TypeSchema duplicate = Alias(owner, AliasType);
    duplicate.subobjects = Span<const SubobjectSchema>(entries, 3);
    RuntimeTypeOps duplicateOps = GeneratedType<SubobjectOwner>::Ops();
    RuntimeSubobjectOps duplicateEntries[3] = {
        duplicateOps.subobjects[0], duplicateOps.subobjects[1], duplicateOps.subobjects[1]};
    duplicateOps.subobjects = Span<const RuntimeSubobjectOps>(duplicateEntries, 3);
    CHECK(attempt(duplicate, Move(duplicateOps)) == ErrorCode::AlreadyExists);
    entries[2] = owner.subobjects[2];

    // 연산표 수가 표와 다르다.
    RuntimeTypeOps shortOps = GeneratedType<SubobjectOwner>::Ops();
    shortOps.subobjects = Span<const RuntimeSubobjectOps>(shortOps.subobjects.Data(), 2);
    CHECK(attempt(Alias(owner, AliasType), Move(shortOps)) == ErrorCode::InvalidArgument);

    // 소유 목록 지정자가 없는데 표가 있다.
    TypeSchema noList = Alias(owner, AliasType);
    noList.subobjectListField = Guid{};
    CHECK(attempt(noList, GeneratedType<SubobjectOwner>::Ops()) == ErrorCode::InvalidArgument);

    // 지정자 필드의 종류가 다르다(root 필드를 소유 목록으로).
    TypeSchema wrongList = Alias(owner, AliasType);
    wrongList.subobjectListField = owner.subobjectRootField;
    CHECK(attempt(wrongList, GeneratedType<SubobjectOwner>::Ops()) == ErrorCode::InvalidArgument);
}

TEST(DefaultSubobject_RegistrationChecksBasePrefix) {
    SystemAllocator allocator;
    const TypeSchema& child = GeneratedType<SubobjectOwnerChild>::Schema();
    const RuntimeTypeOps childOps = GeneratedType<SubobjectOwnerChild>::Ops();
    // 표와 연산표를 같은 순서로 골라 파생 별칭을 등록한다(연산표 불일치가 먼저 걸리지 않게).
    const auto attempt = [&](Span<const usize> picks, Span<const int32> parents) noexcept {
        SubobjectSchema entries[4];
        RuntimeSubobjectOps entryOps[4];
        for (usize index = 0; index < picks.Size(); ++index) {
            entries[index] = child.subobjects[picks[index]];
            entries[index].attachParent = parents[index];
            entryOps[index] = childOps.subobjects[picks[index]];
        }

        TypeSchema schema = Alias(child, AliasChildType);
        schema.subobjects = Span<const SubobjectSchema>(entries, picks.Size());
        RuntimeTypeOps ops = GeneratedType<SubobjectOwnerChild>::Ops();
        ops.subobjects = Span<const RuntimeSubobjectOps>(entryOps, picks.Size());
        TypeRegistry types(allocator);
        if (!RegisterParts(types) || !GeneratedType<SubobjectOwner>::Register(types)) {
            return SetupFailed;
        }

        return ErrorOf(types.Register(schema, Move(ops)));
    };

    // 그대로(body·arm·plain·hand)는 받는다.
    const usize same[] = {0, 1, 2, 3};
    const int32 sameParents[] = {-1, 0, -1, 0};
    CHECK(attempt(same, sameParents) == ErrorCode::None);

    // base 항목 순서를 바꾼 파생 표(arm·body·plain·hand).
    const usize swapped[] = {1, 0, 2, 3};
    const int32 swappedParents[] = {-1, -1, -1, 1};
    CHECK(attempt(swapped, swappedParents) == ErrorCode::TypeMismatch);

    // base 항목 하나를 뺀 파생 표(body·arm·hand).
    const usize dropped[] = {0, 1, 3};
    const int32 droppedParents[] = {-1, 0, 0};
    CHECK(attempt(dropped, droppedParents) == ErrorCode::TypeMismatch);
}

TEST(DefaultSubobject_StructRejectsSubobjectData) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    TypeSchema schema = GeneratedType<reflection_fixture::StructLeaf>::Schema();
    schema.subobjectListField = Guid{1, 1};
    CHECK(ErrorOf(
              types.RegisterStruct(schema, GeneratedType<reflection_fixture::StructLeaf>::Ops())) ==
          ErrorCode::InvalidArgument);
}

} // namespace subobject_schema_tests_file
