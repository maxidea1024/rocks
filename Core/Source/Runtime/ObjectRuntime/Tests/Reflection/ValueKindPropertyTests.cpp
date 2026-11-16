#include <Core/Memory/SystemAllocator.h>
#include <Object/Object.h>
#include <Object/TypeRegistry.h>
#include <TestSupport/Test.h>

// 값 종류 성질 표(Meta/TypeSchema.h ValueKindProperties)의 시험이다. 근거는
// Docs/Design/ReflectionExtensions.ko.md 1.3절과 17절 B9a 항목이다. 모든 종류 × 모든 성질이 1.3절
// 허용 표와 같고, 이름 있는 질의와 비트 조회가 같은 칸을 읽으며, 열거자 밖의 값은 성질이 없고,
// TypeRegistry 등록이 숫자 범위가 아니라 표로 종류를 판정한다(Array 원소·struct 멤버·클래스 필드).
// MetaCompiler C# 표와의 대조는 ToolTests의 Meta.ValueKindTable* 시험이 맡는다.
using namespace rocks;

namespace {

using Query = bool (*)(ValueKind) noexcept;

// 1.3절 성질 순서다. 아래 기대 행의 글자 하나가 이 순서의 성질 하나다.
constexpr ValueKindProperty Properties[] = {ValueKindProperty::Field,
                                            ValueKindProperty::StructMember,
                                            ValueKindProperty::ArrayElement,
                                            ValueKindProperty::StaticArrayElement,
                                            ValueKindProperty::MapKey,
                                            ValueKindProperty::MapValue,
                                            ValueKindProperty::SetElement,
                                            ValueKindProperty::OptionalInner,
                                            ValueKindProperty::InstancedStructContent,
                                            ValueKindProperty::AcceptsRequired,
                                            ValueKindProperty::HasTargetType,
                                            ValueKindProperty::RequiresTargetId,
                                            ValueKindProperty::Container,
                                            ValueKindProperty::WholeFieldValue};

// Properties와 같은 순서의 이름 있는 질의다.
constexpr Query Queries[] = {&ValueKindCanBeField,
                             &ValueKindCanBeStructMember,
                             &ValueKindCanBeArrayElement,
                             &ValueKindCanBeStaticArrayElement,
                             &ValueKindCanBeMapKey,
                             &ValueKindCanBeMapValue,
                             &ValueKindCanBeSetElement,
                             &ValueKindCanBeOptionalInner,
                             &ValueKindCanBeInstancedStructContent,
                             &ValueKindAcceptsRequired,
                             &ValueKindHasTargetType,
                             &ValueKindRequiresTargetId,
                             &ValueKindIsContainer,
                             &ValueKindIsWholeFieldValue};

constexpr usize PropertyCount = sizeof(Properties) / sizeof(Properties[0]);
static_assert(sizeof(Queries) / sizeof(Queries[0]) == PropertyCount);

struct ExpectedRow {
    ValueKind kind;
    // PropertyCount글자의 '1'(성질 있음)·'0'(없음)이다.
    const char* cells;
};

// 1.3절 허용 표를 옮긴 기대값이다. 열: 필드·멤버·Array 원소·StaticArray 원소·Map 키·Map 값·Set
// 원소·Optional 내부·InstancedStruct 내용·Required·대상 타입·대상 ID 필수·컨테이너·통째 FieldValue.
// Struct는 값 struct 기준이다(참조 struct 제한은 struct 형질로 따로 본다). 새 종류는 행을 더한다.
constexpr ExpectedRow Expected[] = {{ValueKind::Opaque, "00000000000000"},
                                    {ValueKind::Bool, "11111111000001"},
                                    {ValueKind::UInt8, "11111111000001"},
                                    {ValueKind::UInt16, "11111111000001"},
                                    {ValueKind::UInt32, "11111111000001"},
                                    {ValueKind::UInt64, "11111111000001"},
                                    {ValueKind::Int32, "11111111000001"},
                                    {ValueKind::Int64, "11111111000001"},
                                    {ValueKind::Float32, "11110101000001"},
                                    {ValueKind::Float64, "11110101000001"},
                                    {ValueKind::Guid, "11111111000001"},
                                    {ValueKind::Utf8String, "11111111000001"},
                                    {ValueKind::ObjectReference, "11110100011001"},
                                    {ValueKind::Enum, "11111111001101"},
                                    {ValueKind::Struct, "11110101101100"},
                                    {ValueKind::Array, "11000000000010"},
                                    {ValueKind::AssetId, "11111111000001"},
                                    {ValueKind::SoftObjectReference, "11111110011001"},
                                    {ValueKind::Digest256, "11111111000001"},
                                    {ValueKind::Int8, "11111111000001"},
                                    {ValueKind::Int16, "11111111000001"},
                                    {ValueKind::Name, "11111111000001"},
                                    {ValueKind::WeakObjectReference, "11110100001001"},
                                    {ValueKind::ClassReference, "11111110011101"},
                                    {ValueKind::Optional, "11000000000010"},
                                    {ValueKind::Map, "11000000000010"},
                                    {ValueKind::Set, "11000000000010"}};

static_assert(sizeof(Expected) / sizeof(Expected[0]) == ValueKindCount);
static_assert(ValueKindCount == 27);
static_assert(ValueKindCanBeMapKey(ValueKind::Bool) && !ValueKindCanBeMapKey(ValueKind::Float32));

usize CellCount(const char* cells) noexcept {
    usize count = 0;
    while (cells[count] != '\0') {
        ++count;
    }

    return count;
}

uint8 NativeKey = 0;

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

// 멤버 field 하나짜리 struct를 등록해 보고 곧바로 해제한다. 등록되면 true다.
bool RegistersAsStructMember(TypeRegistry& types, const FieldSchema& field) noexcept {
    RuntimeStructFieldOps member;
    member.fieldId = field.id;
    member.encode = &MemberEncode;
    member.decode = &MemberDecode;
    RuntimeStructOps ops;
    ops.size = 16;
    ops.alignment = 8;
    ops.nativeType = &NativeKey;
    ops.construct = &Construct;
    ops.destroy = &Destroy;
    ops.fields = Span<const RuntimeStructFieldOps>(&member, 1);
    auto token = types.RegisterStruct(
        {TypeId{0x564b50, 1}, 1, Span<const FieldSchema>(&field, 1), {"S", 1}}, ops);
    if (!token) {
        return false;
    }

    return types.Unregister(token.Value()).IsOk();
}

struct Holder final : Object {};

constexpr TypeId HolderType{0x564b43, 1};

Result<uint32> ClassEncode(const RuntimeFieldOps&, ObjectRegistry&, const ObjectAccessContext&,
                           ObjectHandle, ArchiveWriter&, FieldEncodeContext&) noexcept {
    return Result<uint32>::Failure(ErrorCode::Unsupported);
}

Result<void> ClassDecode(const RuntimeFieldOps&, ConstructionContext*, ObjectHandle,
                         const FieldView&, FieldDecodeContext&) noexcept {
    return Result<void>::Failure(ErrorCode::Unsupported);
}

// 필드 field 하나짜리 클래스를 등록해 보고 곧바로 해제한다. 등록되면 true다. typed면 함수표를 하나
// 싣는다(aggregate는 codec만, 그 밖은 read·write·writeStaged). typed가 아니면(Opaque) 함수표가
// 없다.
bool RegistersAsClassField(TypeRegistry& types, const FieldSchema& field, bool typed) noexcept {
    RuntimeFieldOps ops;
    ops.fieldId = field.id;
    if (FieldIsAggregate(field)) {
        ops.encode = &ClassEncode;
        ops.decode = &ClassDecode;
    } else {
        ops.read = [](ObjectRegistry&, const ObjectAccessContext&, ObjectHandle,
                      FieldValue&) noexcept { return Result<void>::Success(); };
        ops.write = [](ObjectRegistry&, const ObjectAccessContext&, ObjectHandle,
                       const FieldValue&) noexcept { return Result<void>::Success(); };
        ops.writeStaged = [](ConstructionContext&, ObjectHandle, const FieldValue&) noexcept {
            return Result<void>::Success();
        };
    }

    auto typeOps = MakeRuntimeTypeOps<Holder>();
    typeOps.fields = typed ? Span<const RuntimeFieldOps>(&ops, 1) : Span<const RuntimeFieldOps>();
    auto token = types.Register({HolderType, 1, Span<const FieldSchema>(&field, 1), {"Holder", 6}},
                                Move(typeOps));
    if (!token) {
        return false;
    }

    return types.Unregister(token.Value()).IsOk();
}

} // namespace

// 모든 종류 × 모든 성질이 1.3절 표와 같다. 이름 있는 질의·HasValueKindProperty·비트 표가 같은 칸을
// 읽고, 표 밖의 비트는 없으며, 0..ValueKindCount-1의 모든 열거자 값에 기대 행이 하나씩 있다.
TEST(ValueKindProperties_MatchSpecTable) {
    uint32 known = 0;
    for (ValueKindProperty property : Properties) {
        known |= static_cast<uint32>(property);
    }

    bool seen[ValueKindCount] = {};
    for (const ExpectedRow& row : Expected) {
        const uint32 value = static_cast<uint32>(row.kind);
        REQUIRE(value < ValueKindCount);
        CHECK(!seen[value]);
        seen[value] = true;
        REQUIRE(CellCount(row.cells) == PropertyCount);
        CHECK((ValueKindProperties(row.kind) & ~known) == 0);
        for (usize column = 0; column < PropertyCount; ++column) {
            const bool expected = row.cells[column] == '1';
            CHECK(Queries[column](row.kind) == expected);
            CHECK(HasValueKindProperty(row.kind, Properties[column]) == expected);
            CHECK(((ValueKindProperties(row.kind) & static_cast<uint32>(Properties[column])) !=
                   0) == expected);
        }
    }

    for (bool covered : seen) {
        CHECK(covered);
    }
}

// 열거자 밖의 값은 성질이 없고 aggregate도 아니다. aggregate는 typed 필드 종류 중 통째 FieldValue가
// 아닌 것(Struct·Array·Optional·Map·Set)이고, FieldTargetType은 컨테이너 종류에서 원소 형식을 준다.
// 필드 단위 FieldIsAggregate는 Optional을 내부로 가른다(단일 값 내부는 비aggregate, 값 struct는
// aggregate — 리플렉션 B9e).
TEST(ValueKindProperties_OutOfRangeAggregateAndTarget) {
    const ValueKind beyond[] = {static_cast<ValueKind>(ValueKindCount),
                                static_cast<ValueKind>(NumericLimits<uint8>::Max())};
    for (ValueKind kind : beyond) {
        CHECK(ValueKindProperties(kind) == 0);
        CHECK(!ValueKindCanBeField(kind));
        CHECK(!ValueKindIsAggregate(kind));
    }

    for (const ExpectedRow& row : Expected) {
        const bool aggregate = row.kind == ValueKind::Struct || row.kind == ValueKind::Array ||
                               row.kind == ValueKind::Optional || row.kind == ValueKind::Map ||
                               row.kind == ValueKind::Set;
        CHECK(ValueKindIsAggregate(row.kind) == aggregate);
    }

    FieldSchema optional;
    optional.value = ValueType{ValueKind::Optional, {}};
    optional.element = ValueType{ValueKind::Utf8String, {}};
    CHECK(&FieldTargetType(optional) == &optional.element);
    CHECK(!FieldIsAggregate(optional));
    optional.element = ValueType{ValueKind::Struct, {}};
    CHECK(FieldIsAggregate(optional));

    FieldSchema array;
    array.value = ValueType{ValueKind::Array, {}};
    array.element = ValueType{ValueKind::UInt16, {}};
    CHECK(&FieldTargetType(array) == &array.element);
    FieldSchema single;
    single.value = ValueType{ValueKind::UInt16, {}};
    CHECK(&FieldTargetType(single) == &single.value);

    // Map은 값 형식이 대상이고 키는 FieldKeyType이다(리플렉션 B9f2). Map·Set은 늘 aggregate다.
    FieldSchema map;
    map.value = ValueType{ValueKind::Map, {}};
    map.element = ValueType{ValueKind::Float32, {}};
    map.key = ValueType{ValueKind::Name, {}};
    CHECK(&FieldTargetType(map) == &map.element);
    CHECK(&FieldKeyType(map) == &map.key);
    CHECK(FieldIsAggregate(map));
    FieldSchema set;
    set.value = ValueType{ValueKind::Set, {}};
    set.element = ValueType{ValueKind::Name, {}};
    CHECK(&FieldTargetType(set) == &set.element);
    CHECK(FieldIsAggregate(set));
}

// TypeRegistry는 숫자 범위가 아니라 표로 종류를 판정한다. 대상 ID·GC 간선이 필요 없는 모든 종류 값
// (열거자 밖의 값 ValueKindCount 포함)에서 Array 원소 등록 성공 ⇔ ValueKindCanBeArrayElement,
// struct 멤버 등록 성공 ⇔ ValueKindCanBeStructMember(Opaque 멤버는 struct의 typed 필드 규칙도
// 거부한다), 클래스 필드 등록 성공 ⇔ ValueKindCanBeField다. 클래스의 Opaque 필드는 표 밖의 비typed
// 수동 메타데이터로 함수표 없이 받는다.
TEST(ValueKindProperties_TypeRegistryFollowsTable) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    for (uint32 value = 0; value <= ValueKindCount; ++value) {
        const ValueKind kind = static_cast<ValueKind>(value);
        if (ValueKindRequiresTargetId(kind) || kind == ValueKind::ObjectReference) {
            continue;
        }

        FieldSchema element;
        element.id = Guid{0x564b46, 1};
        element.name = Span<const char>("items", 5);
        element.value = ValueType{ValueKind::Array, {}};
        element.element = ValueType{kind, {}};
        element.maxElements = DefaultMaxElements;
        CHECK(RegistersAsStructMember(types, element) == ValueKindCanBeArrayElement(kind));

        // 같은 종류의 단일 값 자리다(Array는 원소 UInt16의 배열 필드, Optional은 내부 UInt16).
        FieldSchema single;
        single.id = Guid{0x564b46, 2};
        single.name = Span<const char>("value", 5);
        single.value = ValueType{kind, {}};
        if (kind == ValueKind::Array || kind == ValueKind::Set) {
            single.element = ValueType{ValueKind::UInt16, {}};
            single.maxElements = DefaultMaxElements;
        } else if (kind == ValueKind::Map) {
            single.element = ValueType{ValueKind::UInt16, {}};
            single.key = ValueType{ValueKind::UInt16, {}};
            single.maxElements = DefaultMaxElements;
        } else if (kind == ValueKind::Optional) {
            single.element = ValueType{ValueKind::UInt16, {}};
        }

        CHECK(RegistersAsStructMember(types, single) == ValueKindCanBeStructMember(kind));
        if (kind == ValueKind::Opaque) {
            CHECK(!ValueKindCanBeField(kind));
            CHECK(RegistersAsClassField(types, single, false));
            CHECK(!RegistersAsClassField(types, single, true));
        } else {
            CHECK(RegistersAsClassField(types, single, true) == ValueKindCanBeField(kind));
        }

        // Optional<kind>(리플렉션 B9e)의 내부 자리다: 등록 성공 ⇔ ValueKindCanBeOptionalInner(참조·
        // 컨테이너·Optional·Opaque 거부). 클래스 필드는 내부에 따라 함수표 모양이 다르다
        // (FieldIsAggregate).
        FieldSchema optional;
        optional.id = Guid{0x564b46, 3};
        optional.name = Span<const char>("maybe", 5);
        optional.value = ValueType{ValueKind::Optional, {}};
        optional.element = ValueType{kind, {}};
        CHECK(RegistersAsStructMember(types, optional) == ValueKindCanBeOptionalInner(kind));
        CHECK(RegistersAsClassField(types, optional, true) == ValueKindCanBeOptionalInner(kind));

        // Map 키·Map 값·Set 원소 자리다(리플렉션 B9f2): 등록 성공 ⇔ 성질 표의 그 칸(ObjectReference
        // 값은 ObjectRefMap이라 B9f4 — 이 반복은 참조를 건너뛴다).
        FieldSchema mapKey;
        mapKey.id = Guid{0x564b46, 5};
        mapKey.name = Span<const char>("byKey", 5);
        mapKey.value = ValueType{ValueKind::Map, {}};
        mapKey.key = ValueType{kind, {}};
        mapKey.element = ValueType{ValueKind::UInt16, {}};
        mapKey.maxElements = DefaultMaxElements;
        CHECK(RegistersAsStructMember(types, mapKey) == ValueKindCanBeMapKey(kind));
        CHECK(RegistersAsClassField(types, mapKey, true) == ValueKindCanBeMapKey(kind));
        FieldSchema mapValue = mapKey;
        mapValue.key = ValueType{ValueKind::UInt16, {}};
        mapValue.element = ValueType{kind, {}};
        CHECK(RegistersAsStructMember(types, mapValue) == ValueKindCanBeMapValue(kind));
        FieldSchema setElement;
        setElement.id = Guid{0x564b46, 6};
        setElement.name = Span<const char>("items", 5);
        setElement.value = ValueType{ValueKind::Set, {}};
        setElement.element = ValueType{kind, {}};
        setElement.maxElements = DefaultMaxElements;
        CHECK(RegistersAsStructMember(types, setElement) == ValueKindCanBeSetElement(kind));
    }

    CHECK(types.Shutdown());
}

// Optional 필드의 모양 규칙이다(리플렉션 B9e): value.type 0, maxElements 0, Required는 내부 종류와
// 무관하게 거부, aggregate 여부에 맞는 함수표(단일 값 내부는 read·write·writeStaged, 값 struct
// 내부는 codec만 — 반대 모양은 InvalidArgument).
TEST(ValueKindProperties_OptionalFieldShape) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    FieldSchema field;
    field.id = Guid{0x564b46, 4};
    field.name = Span<const char>("maybe", 5);
    field.value = ValueType{ValueKind::Optional, {}};
    field.element = ValueType{ValueKind::UInt32, {}};
    CHECK(RegistersAsStructMember(types, field));

    FieldSchema typed = field;
    typed.value.type = TypeId{0x564b54, 1};
    CHECK(!RegistersAsStructMember(types, typed));

    FieldSchema bounded = field;
    bounded.maxElements = 1;
    CHECK(!RegistersAsStructMember(types, bounded));

    // Required는 대상 종류(Optional 내부)로 판정하고 Optional 내부 종류는 모두 Required를 받지
    // 않는다 (성질 표 불변식 — Meta/TypeSchema.h static_assert NoOptionalInnerAcceptsRequired).
    // 그래서 모든 내부 종류에서 거부된다.
    static_assert(detail::NoOptionalInnerAcceptsRequired());
    for (uint32 value = 0; value < ValueKindCount; ++value) {
        const ValueKind inner = static_cast<ValueKind>(value);
        if (!ValueKindCanBeOptionalInner(inner) || ValueKindRequiresTargetId(inner)) {
            continue;
        }

        FieldSchema required = field;
        required.element = ValueType{inner, {}};
        CHECK(RegistersAsStructMember(types, required));
        required.flags = static_cast<uint32>(FieldFlags::Required);
        CHECK(!RegistersAsStructMember(types, required));
        CHECK(!RegistersAsClassField(types, required, true));
    }

    // 단일 값 내부에 codec만 있는 함수표(aggregate 모양)는 거부된다.
    RuntimeFieldOps ops;
    ops.fieldId = field.id;
    ops.encode = &ClassEncode;
    ops.decode = &ClassDecode;
    auto typeOps = MakeRuntimeTypeOps<Holder>();
    typeOps.fields = Span<const RuntimeFieldOps>(&ops, 1);
    CHECK(types
              .Register({HolderType, 1, Span<const FieldSchema>(&field, 1), {"Holder", 6}},
                        Move(typeOps))
              .Error() == ErrorCode::InvalidArgument);
    CHECK(types.Shutdown());
}

namespace {

uint8 ValueTargetKey = 0;
uint8 ReferenceTargetKey = 0;
uint8 OptionalOwnerKey = 0;

void TraceNothing(const void*, TraceVisitor&) noexcept {
}

// field 하나짜리 struct(id·nativeType)를 등록한다. valueNativeType은 멤버 함수표의 대상 native 키,
// strong이면 참조 struct로 trace를 싣는다.
Result<TypeRegistrationToken> RegisterOneFieldStruct(TypeRegistry& types, TypeId id,
                                                     const void* nativeType,
                                                     const FieldSchema& field,
                                                     const void* valueNativeType,
                                                     bool strong) noexcept {
    RuntimeStructFieldOps member;
    member.fieldId = field.id;
    member.encode = &MemberEncode;
    member.decode = &MemberDecode;
    member.valueNativeType = valueNativeType;
    RuntimeStructOps ops;
    ops.size = 16;
    ops.alignment = 8;
    ops.nativeType = nativeType;
    ops.construct = &Construct;
    ops.destroy = &Destroy;
    ops.trace = strong ? &TraceNothing : nullptr;
    ops.fields = Span<const RuntimeStructFieldOps>(&member, 1);
    return types.RegisterStruct({id, 1, Span<const FieldSchema>(&field, 1), {"S", 1}}, ops);
}

} // namespace

// Optional의 내부 struct는 값 struct다(리플렉션 B9e —
// Docs/Design/ReflectionExtensions.ko.md 1.3절): 강한 참조를 담는 struct(trace 있는 참조 struct)를
// 내부로 둔 struct 멤버·클래스 필드 등록은 InvalidArgument이고(TypeRegistry ResolveValueTypes),
// 같은 모양의 값 struct 내부는 등록된다.
TEST(ValueKindProperties_OptionalInnerStructMustBeValueStruct) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    constexpr TypeId ValueTarget{0x564b4f, 1};
    constexpr TypeId ReferenceTarget{0x564b4f, 2};
    FieldSchema plain;
    plain.id = Guid{0x564b4f, 10};
    plain.name = Span<const char>("count", 5);
    plain.value = ValueType{ValueKind::UInt32, {}};
    REQUIRE(RegisterOneFieldStruct(types, ValueTarget, &ValueTargetKey, plain, nullptr, false));
    FieldSchema reference;
    reference.id = Guid{0x564b4f, 11};
    reference.kind = FieldKind::ObjectReference;
    reference.name = Span<const char>("target", 6);
    reference.value = ValueType{ValueKind::ObjectReference, {}};
    REQUIRE(RegisterOneFieldStruct(types, ReferenceTarget, &ReferenceTargetKey, reference, nullptr,
                                   true));

    const struct {
        TypeId inner;
        const void* native;
        bool accepted;
    } cases[] = {{ValueTarget, &ValueTargetKey, true},
                 {ReferenceTarget, &ReferenceTargetKey, false}};
    for (const auto& item : cases) {
        FieldSchema optional;
        optional.id = Guid{0x564b4f, 12};
        optional.name = Span<const char>("maybe", 5);
        optional.value = ValueType{ValueKind::Optional, {}};
        optional.element = ValueType{ValueKind::Struct, item.inner};
        auto member = RegisterOneFieldStruct(types, TypeId{0x564b4f, 3}, &OptionalOwnerKey,
                                             optional, item.native, false);
        CHECK(static_cast<bool>(member) == item.accepted);
        CHECK(item.accepted || member.Error() == ErrorCode::InvalidArgument);
        if (member) {
            REQUIRE(types.Unregister(member.Value()));
        }

        RuntimeFieldOps ops;
        ops.fieldId = optional.id;
        ops.encode = &ClassEncode;
        ops.decode = &ClassDecode;
        ops.valueNativeType = item.native;
        auto typeOps = MakeRuntimeTypeOps<Holder>();
        typeOps.fields = Span<const RuntimeFieldOps>(&ops, 1);
        auto holder = types.Register(
            {HolderType, 1, Span<const FieldSchema>(&optional, 1), {"Holder", 6}}, Move(typeOps));
        CHECK(static_cast<bool>(holder) == item.accepted);
        CHECK(item.accepted || holder.Error() == ErrorCode::InvalidArgument);
        if (holder) {
            REQUIRE(types.Unregister(holder.Value()));
        }
    }

    CHECK(types.Shutdown());
}
