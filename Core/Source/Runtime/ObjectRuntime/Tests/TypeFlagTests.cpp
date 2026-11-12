#include <Core/Memory/SystemAllocator.h>
#include <Object/TypeRegistry.h>
#include <TestSupport/Test.h>

// B2 플래그·도구 이름 등록 검증 시험이다(Docs/Design/ReflectionFoundations.ko.md 2.6절): 알려진
// 플래그 마스크, Required는 참조 필드만, WorldOnly 상속, 도구 이름 문법·대소문자 무시 유일성·깊은
// 복사, 상속 필드 도구 이름 일치.
using namespace rocks;

namespace {

struct FlagBase : Object {};

struct FlagDerived final : FlagBase {};

constexpr TypeId BaseId{0x464c414742415345ULL, 1};
constexpr TypeId DerivedId{0x464c414744455256ULL, 1};

Span<const char> Text(const char* text) noexcept {
    usize size = 0;
    while (text[size] != '\0') {
        ++size;
    }

    return {text, size};
}

FieldSchema Field(uint64 id, const char* name, FieldKind kind = FieldKind::Value, uint32 flags = 0,
                  const char* toolName = "") noexcept {
    FieldSchema field;
    field.id = Guid{0x4649454c44ULL, id};
    field.kind = kind;
    field.flags = flags;
    field.name = Text(name);
    field.toolName = Text(toolName);
    return field;
}

Result<TypeRegistrationToken> RegisterBase(TypeRegistry& types, Span<const FieldSchema> fields,
                                           uint32 flags = 0) noexcept {
    TypeSchema schema{BaseId, 1, fields, Text("FlagBase")};
    schema.flags = flags;
    return types.Register(schema, MakeRuntimeTypeOps<FlagBase>());
}

Result<TypeRegistrationToken> RegisterDerived(TypeRegistry& types, Span<const FieldSchema> fields,
                                              uint32 flags = 0) noexcept {
    TypeSchema schema{DerivedId, 1, fields, Text("FlagDerived")};
    schema.baseType = BaseId;
    schema.flags = flags;
    RuntimeTypeOps ops = MakeRuntimeTypeOps<FlagDerived>();
    ops.nativeBaseType = NativeTypeKey<FlagBase>();
    return types.Register(schema, Move(ops));
}

constexpr uint32 Flag(FieldFlags flag) noexcept {
    return static_cast<uint32>(flag);
}

} // namespace

TEST(TypeFlags_KnownFlagsAreAcceptedAndUnknownBitsRejected) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    CHECK(RegisterBase(types, {}, 4u).Error() == ErrorCode::InvalidArgument);
    FieldSchema unknown[] = {Field(1, "value", FieldKind::Value, 16u)};
    CHECK(RegisterBase(types, Span<const FieldSchema>(unknown, 1)).Error() ==
          ErrorCode::InvalidArgument);

    FieldSchema fields[] = {
        Field(1, "value", FieldKind::Value, Flag(FieldFlags::EditorOnly)),
        Field(2, "target", FieldKind::ObjectReference, Flag(FieldFlags::Transient))};
    auto registered = RegisterBase(types, Span<const FieldSchema>(fields, 2),
                                   static_cast<uint32>(TypeFlags::WorldOnly));
    REQUIRE(registered);
    auto lease = types.Acquire(BaseId);
    REQUIRE(lease);
    CHECK(HasFlag(lease.Value().Schema().flags, TypeFlags::WorldOnly));
    CHECK(HasFlag(lease.Value().Schema().fields[0].flags, FieldFlags::EditorOnly));
    lease.Value().Reset();
    REQUIRE(types.Unregister(registered.Value()));
    CHECK(types.Shutdown());
}

// Required는 typed 참조 필드(ValueKind::ObjectReference)에만 쓴다. 값 필드와 접근기 없는 Opaque
// 참조는 거부한다(typed 참조의 수락은 생성 fixture로 SpecifierTests가 본다).
TEST(TypeFlags_RequiredIsOnlyForTypedReferenceFields) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    FieldSchema value[] = {Field(1, "count", FieldKind::Value, Flag(FieldFlags::Required))};
    CHECK(RegisterBase(types, Span<const FieldSchema>(value, 1)).Error() ==
          ErrorCode::InvalidArgument);
    FieldSchema opaque[] = {
        Field(1, "target", FieldKind::ObjectReference, Flag(FieldFlags::Required))};
    CHECK(RegisterBase(types, Span<const FieldSchema>(opaque, 1)).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(types.Shutdown());
}

TEST(TypeFlags_WorldOnlyIsInherited) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    auto base = RegisterBase(types, {}, static_cast<uint32>(TypeFlags::WorldOnly));
    REQUIRE(base);
    CHECK(RegisterDerived(types, {}).Error() == ErrorCode::InvalidArgument);
    auto derived = RegisterDerived(types, {}, static_cast<uint32>(TypeFlags::WorldOnly));
    REQUIRE(derived);
    REQUIRE(types.Unregister(derived.Value()));
    REQUIRE(types.Unregister(base.Value()));
    CHECK(types.Shutdown());
}

TEST(TypeFlags_ToolNamesAreIdentifiersUniqueIgnoringCaseAndDeepCopied) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    FieldSchema invalid[] = {Field(1, "value", FieldKind::Value, 0, "1bad")};
    CHECK(RegisterBase(types, Span<const FieldSchema>(invalid, 1)).Error() ==
          ErrorCode::InvalidArgument);
    FieldSchema nonAscii[] = {Field(1, "value", FieldKind::Value, 0, "ì²´ë ¥")};
    CHECK(RegisterBase(types, Span<const FieldSchema>(nonAscii, 1)).Error() ==
          ErrorCode::InvalidArgument);

    // Name="Count"의 실효 도구 이름이 다른 필드 이름 count와 대소문자만 다르다.
    FieldSchema collision[] = {Field(1, "value", FieldKind::Value, 0, "Count"), Field(2, "count")};
    CHECK(RegisterBase(types, Span<const FieldSchema>(collision, 2)).Error() ==
          ErrorCode::AlreadyExists);
    // 도구 이름을 따로 주지 않아도 필드 이름끼리 대소문자 무시로 같으면 거부한다.
    FieldSchema plain[] = {Field(1, "count"), Field(2, "Count")};
    CHECK(RegisterBase(types, Span<const FieldSchema>(plain, 2)).Error() ==
          ErrorCode::AlreadyExists);
    // 이름 없는 Opaque 필드는 유일성 검사에서 뺀다.
    FieldSchema unnamed[] = {Field(1, ""), Field(2, "")};
    auto anonymous = RegisterBase(types, Span<const FieldSchema>(unnamed, 2));
    REQUIRE(anonymous);
    REQUIRE(types.Unregister(anonymous.Value()));

    char toolName[] = "HitPoints";
    FieldSchema fields[] = {Field(1, "health"), Field(2, "armor")};
    fields[0].toolName = Span<const char>(toolName, sizeof(toolName) - 1);
    auto registered = RegisterBase(types, Span<const FieldSchema>(fields, 2));
    REQUIRE(registered);
    toolName[0] = 'X';
    auto lease = types.Acquire(BaseId);
    REQUIRE(lease);
    const FieldSchema& saved = lease.Value().Schema().fields[0];
    CHECK(saved.toolName.Data() != toolName);
    CHECK(StringView(saved.toolName.Data(), saved.toolName.Size()) == StringView("HitPoints"));
    CHECK(lease.Value().Schema().fields[1].toolName.Size() == 0);
    lease.Value().Reset();
    REQUIRE(types.Unregister(registered.Value()));
    CHECK(types.Shutdown());
}

// 상속 필드는 실효 도구 이름(toolName 또는 name)이 base와 같아야 하고, 파생 자기 필드와도 유일해야
// 한다.
TEST(TypeFlags_InheritedFieldToolNamesUseEffectiveNames) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    FieldSchema baseFields[] = {Field(1, "health", FieldKind::Value, 0, "HitPoints"),
                                Field(3, "armor", FieldKind::Value, 0, "armor")};
    auto base = RegisterBase(types, Span<const FieldSchema>(baseFields, 2));
    REQUIRE(base);

    FieldSchema renamed[] = {Field(1, "health", FieldKind::Value, 0, "Life"), Field(3, "armor")};
    CHECK(RegisterDerived(types, Span<const FieldSchema>(renamed, 2)).Error() ==
          ErrorCode::TypeMismatch);
    FieldSchema clash[] = {Field(1, "health", FieldKind::Value, 0, "HitPoints"), Field(3, "armor"),
                           Field(2, "hitpoints")};
    CHECK(RegisterDerived(types, Span<const FieldSchema>(clash, 3)).Error() ==
          ErrorCode::AlreadyExists);
    // base의 toolName "armor"는 필드 이름과 같으므로 파생의 빈 toolName과 실효 이름이 같다.
    FieldSchema same[] = {Field(1, "health", FieldKind::Value, 0, "HitPoints"), Field(3, "armor"),
                          Field(2, "depth")};
    auto derived = RegisterDerived(types, Span<const FieldSchema>(same, 3));
    REQUIRE(derived);
    REQUIRE(types.Unregister(derived.Value()));
    REQUIRE(types.Unregister(base.Value()));
    CHECK(types.Shutdown());
}
