#include <Core/Memory/SystemAllocator.h>
#include <Object/AssetId.h>
#include <Object/ClassRef.h>
#include <Object/Object.h>
#include <Object/SoftObjectRef.h>
#include <Object/TypeRegistry.h>
#include <TestSupport/Test.h>

// 리플렉션 B9f2 Map·Set 필드의 TypeRegistry 등록 시험이다(Docs/Design/ReflectionExtensions.ko.md
// 1.1·7.2절): Map·Set 필드 모양(value.type 0·maxElements 범위·Map이 아닌 필드의 빈 키 칸·키 대상
// type 규칙), Map 값의 ObjectReference(ObjectRefMap — 리플렉션 B9f4: FieldKind::ObjectReference여야
// 한다)와 참조 struct 값 거부,
// Required(값·원소 종류로 판정), 키 enum의 레코드 의존(Unregister Busy)·native 키 대조
// (keyNativeType)와 키 대상 조회(TypeLease::FieldKeyEnum), ObjectRuntime 키 형식의 seed 해시와
// 같음의 일치.
using namespace rocks;
namespace map_field_registry_tests_file {

namespace {

enum class KeyMode : uint8 {
    First = 1
};

uint8 OtherEnumKey = 0;
uint8 ValueStructKey = 0;
uint8 ReferenceStructKey = 0;

constexpr TypeId KeyModeType{0x4d4652, 1};
constexpr TypeId HolderType{0x4d4652, 2};
constexpr TypeId ValueStructType{0x4d4652, 3};
constexpr TypeId ReferenceStructType{0x4d4652, 4};
constexpr TypeId BaseClassType{0x4d4652, 5};

struct Holder final : Object {};

Result<uint32> ClassEncode(const RuntimeFieldOps&, ObjectRegistry&, const ObjectAccessContext&,
                           ObjectHandle, ArchiveWriter&, FieldEncodeContext&) noexcept {
    return Result<uint32>::Failure(ErrorCode::Unsupported);
}

Result<void> ClassDecode(const RuntimeFieldOps&, ConstructionContext*, ObjectHandle,
                         const FieldView&, FieldDecodeContext&) noexcept {
    return Result<void>::Failure(ErrorCode::Unsupported);
}

Result<uint32> MemberEncode(const RuntimeStructFieldOps&, const void*, ArchiveWriter&,
                            FieldEncodeContext&) noexcept {
    return Result<uint32>::Failure(ErrorCode::Unsupported);
}

Result<void> MemberDecode(const RuntimeStructFieldOps&, ConstructionContext*, ObjectHandle, void*,
                          const FieldView&, FieldDecodeContext&) noexcept {
    return Result<void>::Failure(ErrorCode::Unsupported);
}

void Construct(void*, Allocator&) noexcept {
}

void Destroy(void*) noexcept {
}

void TraceNothing(const void*, TraceVisitor&) noexcept {
}

// UInt32 → UInt32 Map 필드 하나다(시험이 칸을 바꾼다).
FieldSchema MapField() noexcept {
    FieldSchema field;
    field.id = Guid{0x4d4652, 10};
    field.name = Span<const char>("entries", 7);
    field.value = ValueType{ValueKind::Map, {}};
    field.key = ValueType{ValueKind::UInt32, {}};
    field.element = ValueType{ValueKind::UInt32, {}};
    field.maxElements = DefaultMaxElements;
    return field;
}

// field 하나짜리 Holder 클래스를 등록한다(codec만 — Map·Set은 aggregate). valueNative·keyNative는
// 함수표의 native 키다.
Result<TypeRegistrationToken> RegisterHolder(TypeRegistry& types, const FieldSchema& field,
                                             const void* valueNative = nullptr,
                                             const void* keyNative = nullptr) noexcept {
    RuntimeFieldOps ops;
    ops.fieldId = field.id;
    ops.encode = &ClassEncode;
    ops.decode = &ClassDecode;
    ops.valueNativeType = valueNative;
    ops.keyNativeType = keyNative;
    auto typeOps = MakeRuntimeTypeOps<Holder>();
    typeOps.fields = Span<const RuntimeFieldOps>(&ops, 1);
    return types.Register({HolderType, 1, Span<const FieldSchema>(&field, 1), {"Holder", 6}},
                          Move(typeOps));
}

// field 하나짜리 struct를 등록한다(strong이면 참조 struct — trace를 싣는다).
Result<TypeRegistrationToken> RegisterStruct(TypeRegistry& types, TypeId id, const void* native,
                                             const FieldSchema& field, bool strong,
                                             const void* valueNative = nullptr,
                                             const void* keyNative = nullptr) noexcept {
    RuntimeStructFieldOps member;
    member.fieldId = field.id;
    member.encode = &MemberEncode;
    member.decode = &MemberDecode;
    member.valueNativeType = valueNative;
    member.keyNativeType = keyNative;
    RuntimeStructOps ops;
    ops.size = 16;
    ops.alignment = 8;
    ops.nativeType = native;
    ops.construct = &Construct;
    ops.destroy = &Destroy;
    ops.trace = strong ? &TraceNothing : nullptr;
    ops.fields = Span<const RuntimeStructFieldOps>(&member, 1);
    return types.RegisterStruct({id, 1, Span<const FieldSchema>(&field, 1), {"S", 1}}, ops);
}

// 등록되면 곧바로 해제하고 결과 코드를 돌려준다(성공은 None).
ErrorCode HolderCode(TypeRegistry& types, const FieldSchema& field,
                     const void* valueNative = nullptr, const void* keyNative = nullptr) noexcept {
    auto token = RegisterHolder(types, field, valueNative, keyNative);
    if (!token) {
        return token.Error();
    }

    CHECK(types.Unregister(token.Value()));
    return ErrorCode::None;
}

} // namespace

// Map·Set 필드 모양: value.type 0, maxElements 1..MaxArrayElements, Set·Map이 아닌 필드의 key는
// {Opaque, 0}, 키 대상 type은 대상 종류에만(ClassRef Base는 필수, Soft는 선택, 정수는 0), Map 값의
// ObjectReference(ObjectRefMap — B9f4)는 FieldKind::ObjectReference일 때만 받는다(강한 간선 표지 —
// Value면 InvalidArgument). Required는 값·원소 종류로 판정한다(Soft·강한 참조 값 Map·Soft 원소
// Set은 받고 정수 값 Map은 거부 — 키는 Required와 무관하다).
TEST(MapFieldRegistry_ShapeRules) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    CHECK(HolderCode(types, MapField()) == ErrorCode::None);

    FieldSchema typed = MapField();
    typed.value.type = TypeId{1, 2};
    CHECK(HolderCode(types, typed) == ErrorCode::InvalidArgument);
    FieldSchema empty = MapField();
    empty.maxElements = 0;
    CHECK(HolderCode(types, empty) == ErrorCode::InvalidArgument);
    FieldSchema huge = MapField();
    huge.maxElements = MaxArrayElements + 1;
    CHECK(HolderCode(types, huge) == ErrorCode::InvalidArgument);
    FieldSchema references = MapField();
    references.kind = FieldKind::ObjectReference;
    references.element = ValueType{ValueKind::ObjectReference, {}};
    CHECK(HolderCode(types, references) == ErrorCode::None);
    references.flags = static_cast<uint32>(FieldFlags::Required);
    CHECK(HolderCode(types, references) == ErrorCode::None);
    references.flags = 0;
    references.kind = FieldKind::Value;
    CHECK(HolderCode(types, references) == ErrorCode::InvalidArgument);

    FieldSchema set = MapField();
    set.value = ValueType{ValueKind::Set, {}};
    CHECK(HolderCode(types, set) == ErrorCode::InvalidArgument);
    set.key = ValueType{};
    CHECK(HolderCode(types, set) == ErrorCode::None);
    FieldSchema plain;
    plain.id = Guid{0x4d4652, 11};
    plain.name = Span<const char>("plain", 5);
    plain.value = ValueType{ValueKind::UInt32, {}};
    plain.key = ValueType{ValueKind::UInt32, {}};
    RuntimeFieldOps ops;
    ops.fieldId = plain.id;
    ops.read = [](ObjectRegistry&, const ObjectAccessContext&, ObjectHandle, FieldValue&) noexcept {
        return Result<void>::Success();
    };
    ops.write = [](ObjectRegistry&, const ObjectAccessContext&, ObjectHandle,
                   const FieldValue&) noexcept { return Result<void>::Success(); };
    ops.writeStaged = [](ConstructionContext&, ObjectHandle, const FieldValue&) noexcept {
        return Result<void>::Success();
    };
    auto typeOps = MakeRuntimeTypeOps<Holder>();
    typeOps.fields = Span<const RuntimeFieldOps>(&ops, 1);
    CHECK(types
              .Register({HolderType, 1, Span<const FieldSchema>(&plain, 1), {"Holder", 6}},
                        Move(typeOps))
              .Error() == ErrorCode::InvalidArgument);

    FieldSchema classKey = MapField();
    classKey.key = ValueType{ValueKind::ClassReference, {}};
    CHECK(HolderCode(types, classKey) == ErrorCode::InvalidArgument);
    classKey.key.type = BaseClassType;
    CHECK(HolderCode(types, classKey) == ErrorCode::None);
    FieldSchema softKey = MapField();
    softKey.key = ValueType{ValueKind::SoftObjectReference, {}};
    CHECK(HolderCode(types, softKey) == ErrorCode::None);
    FieldSchema typedKey = MapField();
    typedKey.key.type = TypeId{3, 4};
    CHECK(HolderCode(types, typedKey) == ErrorCode::InvalidArgument);

    FieldSchema required = MapField();
    required.flags = static_cast<uint32>(FieldFlags::Required);
    CHECK(HolderCode(types, required) == ErrorCode::InvalidArgument);
    required.element = ValueType{ValueKind::SoftObjectReference, {}};
    CHECK(HolderCode(types, required) == ErrorCode::None);
    FieldSchema requiredSet = required;
    requiredSet.value = ValueType{ValueKind::Set, {}};
    requiredSet.key = ValueType{};
    CHECK(HolderCode(types, requiredSet) == ErrorCode::None);
    CHECK(types.Shutdown());
}

// 키가 Enum인 Map은 그 enum 레코드를 의존으로 붙잡는다(값 대상과 같은 dependents — 쓰는 클래스·
// struct가 있는 동안 enum의 Unregister는 Busy). 키 enum이 없으면 NotFound, 함수표의 keyNativeType이
// 그 enum의 native 키와 다르면 TypeMismatch, 빠졌거나 키가 Enum이 아닌데 있으면 InvalidArgument다.
// TypeLease·StructView의 FieldKeyEnum이 그 enum 스키마를 준다.
TEST(MapFieldRegistry_EnumKeyIsADependency) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    FieldSchema field = MapField();
    field.key = ValueType{ValueKind::Enum, KeyModeType};
    const void* native = NativeTypeKey<KeyMode>();
    CHECK(HolderCode(types, field, nullptr, native) == ErrorCode::NotFound);

    const EnumeratorSchema values[] = {{{"First", 5}, {}, {}, 1, 0}};
    EnumSchema schema;
    schema.id = KeyModeType;
    schema.name = Span<const char>("KeyMode", 7);
    schema.underlying = ValueKind::UInt8;
    schema.values = Span<const EnumeratorSchema>(values, 1);
    auto enumeration = types.RegisterEnum(schema, RuntimeEnumOps{native, {}});
    REQUIRE(enumeration);
    CHECK(HolderCode(types, field, nullptr, &OtherEnumKey) == ErrorCode::TypeMismatch);
    CHECK(HolderCode(types, field) == ErrorCode::InvalidArgument);
    CHECK(HolderCode(types, MapField(), nullptr, native) == ErrorCode::InvalidArgument);

    auto holder = RegisterHolder(types, field, nullptr, native);
    REQUIRE(holder);
    {
        auto lease = types.Acquire(HolderType);
        REQUIRE(lease);
        const EnumSchema* key = lease.Value().FieldKeyEnum(0);
        REQUIRE(key);
        CHECK(key->id == KeyModeType);
        CHECK(lease.Value().FieldEnum(0) == nullptr);
    }

    CHECK(types.Unregister(enumeration.Value()).Error() == ErrorCode::Busy);
    auto member =
        RegisterStruct(types, ValueStructType, &ValueStructKey, field, false, nullptr, native);
    REQUIRE(member);
    {
        auto lease = types.AcquireValueType(ValueStructType, TypeKind::Struct);
        REQUIRE(lease);
        const EnumSchema* key = lease.Value().Struct().FieldKeyEnum(0);
        CHECK(key && key->id == KeyModeType);
    }

    REQUIRE(types.Unregister(holder.Value()));
    CHECK(types.Unregister(enumeration.Value()).Error() == ErrorCode::Busy);
    REQUIRE(types.Unregister(member.Value()));
    CHECK(types.Unregister(enumeration.Value()));
    CHECK(types.Shutdown());
}

// ObjectRefMap 멤버(리플렉션 B9f4 — Map 값 ObjectReference)는 struct를 참조 struct이자 Registry
// 컨테이너 담김(containsReferenceArray)으로 만든다: trace가 없으면 강한 참조 형질과 어긋나
// InvalidArgument, 그런 struct는 인라인 struct 필드로는 등록되고 Array 원소
// struct(StructRefArray)로는 InvalidArgument다(원소 블록이 owner 저장소 밖 — RF 결정 15).
TEST(MapFieldRegistry_ObjectRefMapMemberMakesARegistryContainerStruct) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    FieldSchema member = MapField();
    member.kind = FieldKind::ObjectReference;
    member.element = ValueType{ValueKind::ObjectReference, {}};
    CHECK(RegisterStruct(types, ReferenceStructType, &ReferenceStructKey, member, false).Error() ==
          ErrorCode::InvalidArgument);
    auto structure = RegisterStruct(types, ReferenceStructType, &ReferenceStructKey, member, true);
    REQUIRE(structure);
    {
        auto lease = types.AcquireValueType(ReferenceStructType, TypeKind::Struct);
        REQUIRE(lease);
        CHECK(lease.Value().Struct().ContainsStrongReference());
        CHECK(lease.Value().Struct().ContainsReferenceArray());
    }

    FieldSchema inlineField;
    inlineField.id = Guid{0x4d4652, 30};
    inlineField.name = Span<const char>("bundle", 6);
    inlineField.value = ValueType{ValueKind::Struct, ReferenceStructType};
    CHECK(HolderCode(types, inlineField, &ReferenceStructKey) == ErrorCode::None);
    FieldSchema elements;
    elements.id = Guid{0x4d4652, 31};
    elements.name = Span<const char>("slots", 5);
    elements.value = ValueType{ValueKind::Array, {}};
    elements.element = ValueType{ValueKind::Struct, ReferenceStructType};
    elements.maxElements = DefaultMaxElements;
    CHECK(HolderCode(types, elements, &ReferenceStructKey) == ErrorCode::InvalidArgument);
    REQUIRE(types.Unregister(structure.Value()));
    CHECK(types.Shutdown());
}

// Map 값 struct는 값 struct다(참조 struct 값은 InvalidArgument — Optional 내부와 같은 규칙). 값
// struct 값은 등록된다.
TEST(MapFieldRegistry_ValueStructMustHoldNoReferences) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    FieldSchema count;
    count.id = Guid{0x4d4652, 20};
    count.name = Span<const char>("count", 5);
    count.value = ValueType{ValueKind::UInt32, {}};
    REQUIRE(RegisterStruct(types, ValueStructType, &ValueStructKey, count, false));
    FieldSchema target;
    target.id = Guid{0x4d4652, 21};
    target.kind = FieldKind::ObjectReference;
    target.name = Span<const char>("target", 6);
    target.value = ValueType{ValueKind::ObjectReference, {}};
    REQUIRE(RegisterStruct(types, ReferenceStructType, &ReferenceStructKey, target, true));

    FieldSchema values = MapField();
    values.element = ValueType{ValueKind::Struct, ValueStructType};
    CHECK(HolderCode(types, values, &ValueStructKey) == ErrorCode::None);
    values.element.type = ReferenceStructType;
    CHECK(HolderCode(types, values, &ReferenceStructKey) == ErrorCode::InvalidArgument);
    CHECK(types.Shutdown());
}

// ObjectRuntime 키 형식의 seed 해시 특수화(AssetId·SoftObjectRef·ClassRef — 리플렉션 B9f2 Map
// 키·Set 원소)는 키 같음과 맞는다: 같은 키는 같은 seed에서 같은 값이고(SoftObjectRef는 allocator가
// 달라도 asset·경로 byte가 같으면 같다), 키가 다르면 같음이 거짓이며 seed가 계산 안에 들어간다.
// AssetId는 같은 Guid의 SeededHash<Guid>와 같은 값이다.
TEST(MapKeyHash_ObjectRuntimeKeysAgreeWithEquality) {
    SystemAllocator allocator;
    SystemAllocator other;
    const HashSeed seed{0x1122334455667788ull, 0x99aabbccddeeff00ull};
    const HashSeed another{1, 2};
    const AssetId asset{Guid{0x4d4b48, 1}};
    CHECK(SeededHash<AssetId>(seed)(asset) == SeededHash<AssetId>(seed)(AssetId{asset.value}));
    CHECK(SeededHash<AssetId>(seed)(asset) == SeededHash<Guid>(seed)(asset.value));
    CHECK(SeededHash<AssetId>(seed)(asset) != SeededHash<AssetId>(another)(asset));
    CHECK(!(asset == AssetId{Guid{0x4d4b48, 2}}));

    SoftObjectRef<> left(allocator);
    SoftObjectRef<> right(other);
    REQUIRE(left.TryAssign(asset, "Body.Socket"));
    REQUIRE(right.TryAssign(asset, "Body.Socket"));
    CHECK(left == right);
    CHECK(SeededHash<SoftObjectRef<>>(seed)(left) == SeededHash<SoftObjectRef<>>(seed)(right));
    CHECK(SeededHash<SoftObjectRef<>>(seed)(left) != SeededHash<SoftObjectRef<>>(another)(left));
    REQUIRE(right.TryAssign(asset, "Body.socket"));
    CHECK(!(left == right));
    REQUIRE(right.TryAssign(AssetId{Guid{0x4d4b48, 2}}, "Body.Socket"));
    CHECK(!(left == right));

    const ClassRef<Object> first(TypeId{0x4d4b48, 3});
    const ClassRef<Object> same(TypeId{0x4d4b48, 3});
    CHECK(first == same);
    CHECK(SeededHash<ClassRef<Object>>(seed)(first) == SeededHash<ClassRef<Object>>(seed)(same));
    CHECK(SeededHash<ClassRef<Object>>(seed)(first) == SeededHash<Guid>(seed)(first.Id()));
    CHECK(!(first == ClassRef<Object>(TypeId{0x4d4b48, 4})));
}
} // namespace map_field_registry_tests_file
