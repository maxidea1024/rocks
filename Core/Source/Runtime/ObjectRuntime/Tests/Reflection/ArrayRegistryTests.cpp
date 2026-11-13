#include <Core/Memory/SystemAllocator.h>
#include <Object/ObjectRegistry.h>
#include <Object/TypeRegistry.h>
#include <TestSupport/Test.h>

// 리플렉션 배열 필드의 등록 규칙 시험이다(Docs/Design/ReflectionFoundations.ko.md 5.6절 레지스트리
// B5b). 레지스트리는 값 메모리를 만지지 않으므로 가짜 native 키와 빈 함수로 규칙만 검증한다: 배열
// 필드 서술 규칙 전부, 원소 enum·struct 의존(Busy)·NotFound·TypeMismatch·FieldEnum·FieldStruct,
// 배열을 거친 깊이 17, 계산 형질(containsReferenceArray 포함), StructRefArray 원소 컨테이너 거부,
// WorldOnly 원소, 접근기를 가진 배열 거부, FieldWriteOperation Unsupported, 상속 maxElements 대조.
using namespace rocks;

namespace {

uint8 NativeKeys[40];

const void* Key(usize index) noexcept {
    return &NativeKeys[index];
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

void Trace(const void*, TraceVisitor&) noexcept {
}

Result<uint32> ClassEncode(const RuntimeFieldOps&, ObjectRegistry&, const ObjectAccessContext&,
                           ObjectHandle, ArchiveWriter&, FieldEncodeContext&) noexcept {
    return Result<uint32>::Failure(ErrorCode::Unsupported);
}

Result<void> ClassDecode(const RuntimeFieldOps&, ConstructionContext*, ObjectHandle,
                         const FieldView&, FieldDecodeContext&) noexcept {
    return Result<void>::Failure(ErrorCode::Unsupported);
}

constexpr uint32 Flag(FieldFlags flag) noexcept {
    return static_cast<uint32>(flag);
}

// 단일 값 필드다.
FieldSchema Field(Guid id, const char* name, usize nameBytes, ValueKind kind, TypeId type = {},
                  uint32 flags = 0) noexcept {
    FieldSchema field;
    field.id = id;
    field.kind = kind == ValueKind::ObjectReference ? FieldKind::ObjectReference : FieldKind::Value;
    field.flags = flags;
    field.name = Span<const char>(name, nameBytes);
    field.value = ValueType{kind, type};
    return field;
}

// 배열 필드다(value = {Array, 0}, FieldKind는 원소가 참조면 ObjectReference).
FieldSchema ArrayFieldSchema(Guid id, const char* name, usize nameBytes, ValueKind element,
                             TypeId type = {}, uint32 maxElements = DefaultMaxElements,
                             uint32 flags = 0) noexcept {
    FieldSchema field;
    field.id = id;
    field.kind =
        element == ValueKind::ObjectReference ? FieldKind::ObjectReference : FieldKind::Value;
    field.flags = flags;
    field.name = Span<const char>(name, nameBytes);
    field.value = ValueType{ValueKind::Array, {}};
    field.element = ValueType{element, type};
    field.maxElements = maxElements;
    return field;
}

RuntimeStructFieldOps Member(Guid id, const void* valueNative = nullptr) noexcept {
    RuntimeStructFieldOps ops;
    ops.fieldId = id;
    ops.encode = &MemberEncode;
    ops.decode = &MemberDecode;
    ops.valueNativeType = valueNative;
    return ops;
}

RuntimeStructOps StructOps(usize key, Span<const RuntimeStructFieldOps> fields,
                           bool traced = false) noexcept {
    RuntimeStructOps ops;
    ops.size = 16;
    ops.alignment = 8;
    ops.nativeType = Key(key);
    ops.construct = &Construct;
    ops.destroy = &Destroy;
    ops.trace = traced ? &Trace : nullptr;
    ops.fields = fields;
    return ops;
}

TypeId StructId(usize index) noexcept {
    return TypeId{0x415252, 100 + index};
}

constexpr Guid ItemsId{0x4152, 1};
constexpr Guid ValueId{0x4152, 2};
constexpr Guid TargetId{0x4152, 3};
constexpr Guid InnerId{0x4152, 4};

// 필드 하나짜리 struct를 등록한다.
Result<TypeRegistrationToken> RegisterOne(TypeRegistry& types, usize index,
                                          const FieldSchema& field,
                                          const void* valueNative = nullptr,
                                          bool traced = false) noexcept {
    const RuntimeStructFieldOps ops[] = {Member(field.id, valueNative)};
    return types.RegisterStruct({StructId(index), 1, Span<const FieldSchema>(&field, 1), {"S", 1}},
                                StructOps(index, ops, traced));
}

// 값 멤버 하나짜리 참조 없는 struct다.
Result<TypeRegistrationToken> RegisterPlain(TypeRegistry& types, usize index) noexcept {
    return RegisterOne(types, index, Field(ValueId, "value", 5, ValueKind::UInt32));
}

// 강한 참조 멤버 하나짜리 참조 struct다(required면 Required).
Result<TypeRegistrationToken> RegisterReference(TypeRegistry& types, usize index,
                                                bool required = false) noexcept {
    return RegisterOne(types, index,
                       Field(TargetId, "target", 6, ValueKind::ObjectReference, {},
                             required ? Flag(FieldFlags::Required) : 0),
                       nullptr, true);
}

StructView ViewOf(const TypeLease& lease) noexcept {
    return lease.Struct();
}

struct Owner : Object {
    uint8 storage[64] = {};
};

struct Derived final : Owner {};

struct Unrelated final : Object {};

constexpr TypeId OwnerType{0x4152434c, 1};
constexpr TypeId DerivedType{0x4152434c, 2};
constexpr TypeId UnrelatedType{0x4152434c, 3};
constexpr TypeId AliasType{0x4152434c, 4};

} // namespace

// 배열 필드 서술: value.type 0, 원소는 단일 값 종류(Opaque·Array 거부), element.type은 Enum·Struct
// 필수·두 참조 선택·그 밖 0, maxElements ∈ [1, MaxArrayElements], FieldKind::ObjectReference ⇔ 원소
// ObjectReference, Required는 원소가 ObjectReference·SoftObjectReference일 때만이다.
TEST(ArrayRegistry_FieldRules) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    auto accept = [&](const FieldSchema& field, bool traced = false) noexcept {
        auto token = RegisterOne(types, 0, field, nullptr, traced);
        if (!token) {
            return false;
        }

        return types.Unregister(token.Value()).IsOk();
    };
    auto reject = [&](const FieldSchema& field, bool traced = false) noexcept {
        return RegisterOne(types, 0, field, nullptr, traced).Error();
    };

    CHECK(accept(ArrayFieldSchema(ItemsId, "items", 5, ValueKind::UInt32, {}, 1)));
    CHECK(accept(ArrayFieldSchema(ItemsId, "items", 5, ValueKind::UInt32, {}, MaxArrayElements)));
    CHECK(accept(ArrayFieldSchema(ItemsId, "items", 5, ValueKind::Utf8String)));
    CHECK(accept(ArrayFieldSchema(ItemsId, "items", 5, ValueKind::Digest256)));
    // 참조 원소의 기대 타입은 선택이다(ObjectRefArray는 강한 참조라 trace가 필요하다).
    CHECK(accept(ArrayFieldSchema(ItemsId, "items", 5, ValueKind::ObjectReference), true));
    CHECK(
        accept(ArrayFieldSchema(ItemsId, "items", 5, ValueKind::ObjectReference, OwnerType), true));
    CHECK(accept(ArrayFieldSchema(ItemsId, "items", 5, ValueKind::SoftObjectReference, OwnerType)));
    // Required는 참조·소프트 참조 원소에만 쓴다.
    CHECK(accept(ArrayFieldSchema(ItemsId, "items", 5, ValueKind::ObjectReference, {},
                                  DefaultMaxElements, Flag(FieldFlags::Required)),
                 true));
    CHECK(accept(ArrayFieldSchema(ItemsId, "items", 5, ValueKind::SoftObjectReference, {},
                                  DefaultMaxElements, Flag(FieldFlags::Required))));

    CHECK(reject(ArrayFieldSchema(ItemsId, "items", 5, ValueKind::UInt32, {}, 0)) ==
          ErrorCode::InvalidArgument);
    CHECK(reject(ArrayFieldSchema(ItemsId, "items", 5, ValueKind::UInt32, {},
                                  MaxArrayElements + 1)) == ErrorCode::InvalidArgument);
    CHECK(reject(ArrayFieldSchema(ItemsId, "items", 5, ValueKind::Opaque)) ==
          ErrorCode::InvalidArgument);
    CHECK(reject(ArrayFieldSchema(ItemsId, "items", 5, ValueKind::Array)) ==
          ErrorCode::InvalidArgument);
    CHECK(reject(ArrayFieldSchema(ItemsId, "items", 5, ValueKind::UInt32, Guid{1, 1})) ==
          ErrorCode::InvalidArgument);
    CHECK(reject(ArrayFieldSchema(ItemsId, "items", 5, ValueKind::Enum)) ==
          ErrorCode::InvalidArgument);
    CHECK(reject(ArrayFieldSchema(ItemsId, "items", 5, ValueKind::Struct)) ==
          ErrorCode::InvalidArgument);
    CHECK(reject(ArrayFieldSchema(ItemsId, "items", 5, ValueKind::UInt32, {}, DefaultMaxElements,
                                  Flag(FieldFlags::Required))) == ErrorCode::InvalidArgument);
    // 배열의 value.type은 0이다.
    FieldSchema typedValue = ArrayFieldSchema(ItemsId, "items", 5, ValueKind::UInt32);
    typedValue.value.type = Guid{2, 2};
    CHECK(reject(typedValue) == ErrorCode::InvalidArgument);
    // FieldKind는 원소 참조 여부와 대응한다.
    FieldSchema valueKindRefs = ArrayFieldSchema(ItemsId, "items", 5, ValueKind::ObjectReference);
    valueKindRefs.kind = FieldKind::Value;
    CHECK(reject(valueKindRefs, true) == ErrorCode::InvalidArgument);
    FieldSchema referenceKindValues = ArrayFieldSchema(ItemsId, "items", 5, ValueKind::UInt32);
    referenceKindValues.kind = FieldKind::ObjectReference;
    CHECK(reject(referenceKindValues) == ErrorCode::InvalidArgument);
    // 원소 enum·struct가 아닌 배열은 valueNativeType을 싣지 않는다.
    const FieldSchema plain = ArrayFieldSchema(ItemsId, "items", 5, ValueKind::UInt32);
    CHECK(RegisterOne(types, 0, plain, Key(1)).Error() == ErrorCode::InvalidArgument);
    CHECK(types.Shutdown());
}

// 원소 enum·struct: 미등록 NotFound, 종류·native 키가 다르면 TypeMismatch, valueNativeType 없음
// InvalidArgument, 의존 대상의 Unregister는 Busy, FieldEnum·FieldStruct는 원소의 enum·struct다.
TEST(ArrayRegistry_ElementTargetsAndViews) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    constexpr TypeId EnumId{0x41454e, 1};
    static constexpr EnumeratorSchema values[] = {{{"One", 3}, {}, {}, 1, 0}};
    const FieldSchema enumItems = ArrayFieldSchema(ItemsId, "items", 5, ValueKind::Enum, EnumId);
    CHECK(RegisterOne(types, 1, enumItems, Key(20)).Error() == ErrorCode::NotFound);
    auto choice = types.RegisterEnum(
        {EnumId, {"Choice", 6}, ValueKind::UInt8, 0u, Span<const EnumeratorSchema>(values, 1), {}},
        RuntimeEnumOps{Key(20), {}});
    REQUIRE(choice);
    CHECK(RegisterOne(types, 1, enumItems, nullptr).Error() == ErrorCode::InvalidArgument);
    CHECK(RegisterOne(types, 1, enumItems, Key(21)).Error() == ErrorCode::TypeMismatch);
    auto plain = RegisterPlain(types, 2);
    REQUIRE(plain);
    // Enum 원소가 struct를, Struct 원소가 enum을 가리키면 TypeMismatch다.
    CHECK(RegisterOne(types, 1, ArrayFieldSchema(ItemsId, "items", 5, ValueKind::Enum, StructId(2)),
                      Key(2))
              .Error() == ErrorCode::TypeMismatch);
    CHECK(RegisterOne(types, 1, ArrayFieldSchema(ItemsId, "items", 5, ValueKind::Struct, EnumId),
                      Key(20))
              .Error() == ErrorCode::TypeMismatch);

    // 두 배열 멤버(enum 원소·struct 원소)를 가진 struct.
    const FieldSchema both[] = {
        enumItems, ArrayFieldSchema(InnerId, "inner", 5, ValueKind::Struct, StructId(2))};
    const RuntimeStructFieldOps bothOps[] = {Member(ItemsId, Key(20)), Member(InnerId, Key(2))};
    auto holder =
        types.RegisterStruct({StructId(3), 1, both, {"Holder", 6}}, StructOps(3, bothOps));
    REQUIRE(holder);
    CHECK(types.Unregister(choice.Value()).Error() == ErrorCode::Busy);
    CHECK(types.Unregister(plain.Value()).Error() == ErrorCode::Busy);
    {
        auto lease = types.AcquireValueType(StructId(3), TypeKind::Struct);
        REQUIRE(lease);
        const StructView view = ViewOf(lease.Value());
        REQUIRE(view.FieldEnum(0) != nullptr);
        CHECK(view.FieldEnum(0)->id == EnumId);
        CHECK(view.FieldStruct(0).IsEmpty());
        CHECK(view.FieldEnum(1) == nullptr);
        CHECK(view.FieldStruct(1).Schema().id == StructId(2));
        CHECK(lease.Value().FieldEnum(0) == view.FieldEnum(0));
        CHECK(view.Depth() == 2);
        CHECK(!view.ContainsStrongReference());
        CHECK(!view.ContainsReferenceArray());
    }

    // 클래스 lease의 FieldEnum·FieldStruct도 원소다.
    const FieldSchema classFields[] = {
        enumItems, ArrayFieldSchema(InnerId, "inner", 5, ValueKind::Struct, StructId(2))};
    const RuntimeFieldOps classOps[] = {
        {ItemsId, nullptr, {}, nullptr, &ClassEncode, &ClassDecode, Key(20)},
        {InnerId, nullptr, {}, nullptr, &ClassEncode, &ClassDecode, Key(2)}};
    auto ops = MakeRuntimeTypeOps<Owner>();
    ops.fields = classOps;
    ops.completeFieldLayout = true;
    auto owner = types.Register({OwnerType, 1, classFields, {"Owner", 5}}, Move(ops));
    REQUIRE(owner);
    {
        auto lease = types.Acquire(OwnerType);
        REQUIRE(lease);
        REQUIRE(lease.Value().FieldEnum(0) != nullptr);
        CHECK(lease.Value().FieldEnum(0)->id == EnumId);
        CHECK(lease.Value().FieldStruct(1).Schema().id == StructId(2));
        CHECK(lease.Value().FieldStruct(0).IsEmpty());
    }

    CHECK(types.Shutdown());
}

// struct 원소 배열 멤버의 깊이는 원소 깊이 + 1이다: 배열만으로 이은 사슬도 MaxStructDepth(16)까지
// 등록되고 17은 InvalidArgument다.
TEST(ArrayRegistry_DepthThroughArrays) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterPlain(types, 0));
    for (usize level = 1; level < MaxStructDepth; ++level) {
        REQUIRE(RegisterOne(
            types, level,
            ArrayFieldSchema(InnerId, "inner", 5, ValueKind::Struct, StructId(level - 1)),
            Key(level - 1)));
    }

    {
        auto top = types.AcquireValueType(StructId(MaxStructDepth - 1), TypeKind::Struct);
        REQUIRE(top);
        CHECK(top.Value().Struct().Depth() == MaxStructDepth);
    }

    CHECK(RegisterOne(types, MaxStructDepth,
                      ArrayFieldSchema(InnerId, "inner", 5, ValueKind::Struct,
                                       StructId(MaxStructDepth - 1)),
                      Key(MaxStructDepth - 1))
              .Error() == ErrorCode::InvalidArgument);
    CHECK(types.Shutdown());
}

// 계산 형질: ObjectRefArray·StructRefArray 멤버는 강한 참조이자 Registry 컨테이너이고(trace 필요),
// inline struct 멤버로 전파된다. Array<참조 없는 struct>는 어느 쪽도 아니다. 배열 멤버의 Required와
// 원소 struct의 Required 후손은 ContainsRequired에 세지 않는다. Registry 컨테이너를 담은 참조
// struct는 StructRefArray 원소가 될 수 없다(struct·클래스 모두 InvalidArgument).
TEST(ArrayRegistry_ComputedTraitsAndElementRules) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterPlain(types, 0));
    REQUIRE(RegisterReference(types, 1, true));

    // ObjectRefArray 멤버(Required): trace가 필요하고, 형질은 강한 참조·Registry 컨테이너, Required
    // 아님.
    const FieldSchema refs = ArrayFieldSchema(ItemsId, "items", 5, ValueKind::ObjectReference, {},
                                              DefaultMaxElements, Flag(FieldFlags::Required));
    CHECK(RegisterOne(types, 2, refs, nullptr, false).Error() == ErrorCode::InvalidArgument);
    REQUIRE(RegisterOne(types, 2, refs, nullptr, true));
    // StructRefArray 멤버(원소 = 참조 struct 1 — Required 후손이 있어도 세지 않는다).
    REQUIRE(RegisterOne(types, 3,
                        ArrayFieldSchema(ItemsId, "items", 5, ValueKind::Struct, StructId(1)),
                        Key(1), true));
    // Array<참조 없는 struct>.
    REQUIRE(RegisterOne(
        types, 4, ArrayFieldSchema(ItemsId, "items", 5, ValueKind::Struct, StructId(0)), Key(0)));
    // ObjectRefArray를 담은 struct 2를 inline 멤버로 담는 struct.
    REQUIRE(RegisterOne(types, 5, Field(InnerId, "inner", 5, ValueKind::Struct, StructId(2)),
                        Key(2), true));
    struct Expected {
        usize index;
        bool strong;
        bool referenceArray;
        bool required;
        uint32 depth;
    };

    const Expected cases[] = {{1, true, false, true, 1},
                              {2, true, true, false, 1},
                              {3, true, true, false, 2},
                              {4, false, false, false, 2},
                              {5, true, true, false, 2}};
    for (const Expected& expected : cases) {
        auto lease = types.AcquireValueType(StructId(expected.index), TypeKind::Struct);
        REQUIRE(lease);
        const StructView view = ViewOf(lease.Value());
        CHECK(view.ContainsStrongReference() == expected.strong);
        CHECK(view.ContainsReferenceArray() == expected.referenceArray);
        CHECK(view.ContainsRequired() == expected.required);
        CHECK(view.Depth() == expected.depth);
    }

    // Registry 컨테이너를 담은 참조 struct(2·3·5)는 StructRefArray 원소가 될 수 없다.
    const usize containers[] = {2, 3, 5};
    for (usize element : containers) {
        CHECK(
            RegisterOne(types, 6,
                        ArrayFieldSchema(ItemsId, "items", 5, ValueKind::Struct, StructId(element)),
                        Key(element), true)
                .Error() == ErrorCode::InvalidArgument);
    }

    const FieldSchema classField =
        ArrayFieldSchema(ItemsId, "items", 5, ValueKind::Struct, StructId(2));
    const RuntimeFieldOps classOps[] = {
        {ItemsId, nullptr, {}, nullptr, &ClassEncode, &ClassDecode, Key(2)}};
    auto ops = MakeRuntimeTypeOps<Owner>();
    ops.fields = classOps;
    ops.completeFieldLayout = true;
    CHECK(types
              .Register({OwnerType, 1, Span<const FieldSchema>(&classField, 1), {"Owner", 5}},
                        Move(ops))
              .Error() == ErrorCode::InvalidArgument);

    // 원소 struct의 WorldOnly 형질: struct로 전파되고, WorldOnly가 아닌 클래스의 원소 배열은
    // 거부된다.
    const FieldSchema value = Field(ValueId, "value", 5, ValueKind::UInt32);
    const RuntimeStructFieldOps valueOps[] = {Member(ValueId)};
    RuntimeStructOps worldOps = StructOps(7, valueOps);
    worldOps.requiresWorldOnlyOwner = true;
    REQUIRE(types.RegisterStruct({StructId(7), 1, Span<const FieldSchema>(&value, 1), {"W", 1}},
                                 Move(worldOps)));
    REQUIRE(RegisterOne(
        types, 8, ArrayFieldSchema(ItemsId, "items", 5, ValueKind::Struct, StructId(7)), Key(7)));
    {
        auto lease = types.AcquireValueType(StructId(8), TypeKind::Struct);
        REQUIRE(lease);
        CHECK(ViewOf(lease.Value()).RequiresWorldOnlyOwner());
    }

    const FieldSchema worldField =
        ArrayFieldSchema(ItemsId, "items", 5, ValueKind::Struct, StructId(7));
    auto registerWorld = [&](uint32 flags) noexcept {
        const RuntimeFieldOps fields[] = {
            {ItemsId, nullptr, {}, nullptr, &ClassEncode, &ClassDecode, Key(7)}};
        auto worldClass = MakeRuntimeTypeOps<Owner>();
        worldClass.fields = fields;
        worldClass.completeFieldLayout = true;
        return types.Register(
            {OwnerType, 1, Span<const FieldSchema>(&worldField, 1), {"Owner", 5}, {}, flags},
            Move(worldClass));
    };
    CHECK(registerWorld(0).Error() == ErrorCode::InvalidArgument);
    CHECK(registerWorld(static_cast<uint32>(TypeFlags::WorldOnly)));
    CHECK(types.Shutdown());
}

// 클래스 배열 필드: read·write·writeStaged가 있으면 거부, Transient가 아니면 codec 필수(Transient는
// 없이 등록), 등록된 write는 문맥·대상 검사 뒤 Unsupported다. 파생 스키마의 상속 배열 필드는 원소·
// maxElements까지 같아야 한다(TypeMismatch).
TEST(ArrayRegistry_ClassArrayFieldsAndInheritance) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    const FieldSchema items = ArrayFieldSchema(ItemsId, "items", 5, ValueKind::UInt32, {}, 4);
    auto registerOwner = [&](RuntimeFieldOps field, uint32 fieldFlags) noexcept {
        FieldSchema schema = items;
        schema.flags = fieldFlags;
        auto ops = MakeRuntimeTypeOps<Owner>();
        ops.fields = Span<const RuntimeFieldOps>(&field, 1);
        ops.completeFieldLayout = true;
        return types.Register({OwnerType, 1, Span<const FieldSchema>(&schema, 1), {"Owner", 5}},
                              Move(ops));
    };
    const RuntimeFieldOps arrayField{ItemsId, nullptr, {}, nullptr, &ClassEncode, &ClassDecode};
    RuntimeFieldOps withRead = arrayField;
    withRead.read = [](ObjectRegistry&, const ObjectAccessContext&, ObjectHandle,
                       FieldValue&) noexcept { return Result<void>::Success(); };
    CHECK(registerOwner(withRead, 0).Error() == ErrorCode::InvalidArgument);
    RuntimeFieldOps withWrite = arrayField;
    withWrite.write = [](ObjectRegistry&, const ObjectAccessContext&, ObjectHandle,
                         const FieldValue&) noexcept { return Result<void>::Success(); };
    CHECK(registerOwner(withWrite, 0).Error() == ErrorCode::InvalidArgument);
    RuntimeFieldOps withStaged = arrayField;
    withStaged.writeStaged = [](ConstructionContext&, ObjectHandle, const FieldValue&) noexcept {
        return Result<void>::Success();
    };
    CHECK(registerOwner(withStaged, 0).Error() == ErrorCode::InvalidArgument);
    RuntimeFieldOps noCodec{ItemsId, nullptr, {}, nullptr, nullptr, nullptr};
    CHECK(registerOwner(noCodec, 0).Error() == ErrorCode::InvalidArgument);
    auto transient = registerOwner(noCodec, Flag(FieldFlags::Transient));
    REQUIRE(transient);
    REQUIRE(types.Unregister(transient.Value()));

    auto owner = registerOwner(arrayField, 0);
    REQUIRE(owner);
    // 다른 클래스와, 같은 native(Owner)지만 그 필드가 없는 별칭 타입.
    auto unrelatedOps = MakeRuntimeTypeOps<Unrelated>();
    unrelatedOps.completeFieldLayout = true;
    REQUIRE(types.Register({UnrelatedType, 1, {}, {"Unrelated", 9}}, Move(unrelatedOps)));
    auto aliasOps = MakeRuntimeTypeOps<Owner>();
    aliasOps.completeFieldLayout = true;
    REQUIRE(types.Register({AliasType, 1, {}, {"Alias", 5}}, Move(aliasOps)));
    {
        auto lease = types.Acquire(OwnerType);
        REQUIRE(lease);
        ObjectRegistry objects(allocator, types, {0x4152, 1});
        {
            // 문맥·대상 검사(빈 handle은 그 단계의 오류) 뒤 배열 필드 통째 쓰기는 Unsupported다.
            // 다른 클래스 객체는 그보다 먼저 TypeMismatch, 그 필드가 없는 타입의 객체는 NotFound다.
            FieldValue value(allocator);
            value.kind = ValueKind::Array;
            const auto& write = lease.Value().Ops().fields[0].write;
            CHECK(write(objects, objects.OwnerAccess(), ObjectHandle{}, value).Error() !=
                  ErrorCode::Unsupported);
            auto object = objects.NewObject<Owner>(objects.OwnerAccess(), OwnerType);
            auto unrelated = objects.NewObject<Unrelated>(objects.OwnerAccess(), UnrelatedType);
            auto alias = objects.NewObject<Owner>(objects.OwnerAccess(), AliasType);
            REQUIRE(object && unrelated && alias);
            CHECK(
                write(objects, objects.OwnerAccess(), unrelated.Value().Handle(), value).Error() ==
                ErrorCode::TypeMismatch);
            CHECK(write(objects, objects.OwnerAccess(), alias.Value().Handle(), value).Error() ==
                  ErrorCode::NotFound);
            CHECK(write(objects, objects.OwnerAccess(), object.Value().Handle(), value).Error() ==
                  ErrorCode::Unsupported);
        }

        lease.Value().Reset();
        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
    }

    // 상속: 파생은 같은 ID·원소·maxElements로 포함해야 한다.
    auto registerDerived = [&](uint32 maxElements, ValueKind element) noexcept {
        const FieldSchema schema = ArrayFieldSchema(ItemsId, "items", 5, element, {}, maxElements);
        auto ops = MakeRuntimeTypeOps<Derived>();
        ops.fields = Span<const RuntimeFieldOps>(&arrayField, 1);
        ops.completeFieldLayout = true;
        ops.nativeBaseType = NativeTypeKey<Owner>();
        return types.Register(
            {DerivedType, 1, Span<const FieldSchema>(&schema, 1), {"Derived", 7}, OwnerType},
            Move(ops));
    };
    CHECK(registerDerived(5, ValueKind::UInt32).Error() == ErrorCode::TypeMismatch);
    CHECK(registerDerived(4, ValueKind::UInt64).Error() == ErrorCode::TypeMismatch);
    CHECK(registerDerived(4, ValueKind::UInt32));
    CHECK(types.Shutdown());
}

// struct 원소: 미등록 NotFound, valueNativeType 누락 InvalidArgument, native 키 불일치
// TypeMismatch(struct·클래스 모두). Transient 배열 멤버도 GC가 추적하므로 강한 참조·Registry
// 컨테이너 형질에 들어가고(trace 필요) 저장하지 않으므로 Required 후손 형질에는 들어가지 않는다.
TEST(ArrayRegistry_StructElementTargetsAndTransientMembers) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    const FieldSchema missing =
        ArrayFieldSchema(ItemsId, "items", 5, ValueKind::Struct, StructId(9));
    CHECK(RegisterOne(types, 1, missing, Key(9)).Error() == ErrorCode::NotFound);
    REQUIRE(RegisterPlain(types, 0));
    const FieldSchema plains =
        ArrayFieldSchema(ItemsId, "items", 5, ValueKind::Struct, StructId(0));
    CHECK(RegisterOne(types, 1, plains, nullptr).Error() == ErrorCode::InvalidArgument);
    CHECK(RegisterOne(types, 1, plains, Key(5)).Error() == ErrorCode::TypeMismatch);
    auto registerClass = [&](const void* valueNative) noexcept {
        const RuntimeFieldOps fields[] = {
            {ItemsId, nullptr, {}, nullptr, &ClassEncode, &ClassDecode, valueNative}};
        auto ops = MakeRuntimeTypeOps<Owner>();
        ops.fields = fields;
        ops.completeFieldLayout = true;
        return types.Register({OwnerType, 1, Span<const FieldSchema>(&plains, 1), {"Owner", 5}},
                              Move(ops));
    };
    CHECK(registerClass(nullptr).Error() == ErrorCode::InvalidArgument);
    CHECK(registerClass(Key(5)).Error() == ErrorCode::TypeMismatch);
    CHECK(registerClass(Key(0)));

    // Transient Required ObjectRefArray 멤버와 Required 후손이 있는 참조 struct의 Transient 원소
    // 배열.
    const uint32 transientRequired = Flag(FieldFlags::Transient) | Flag(FieldFlags::Required);
    const FieldSchema refs = ArrayFieldSchema(ItemsId, "items", 5, ValueKind::ObjectReference, {},
                                              DefaultMaxElements, transientRequired);
    CHECK(RegisterOne(types, 2, refs, nullptr, false).Error() == ErrorCode::InvalidArgument);
    REQUIRE(RegisterOne(types, 2, refs, nullptr, true));
    REQUIRE(RegisterReference(types, 3, true));
    const FieldSchema slots = ArrayFieldSchema(ItemsId, "items", 5, ValueKind::Struct, StructId(3),
                                               DefaultMaxElements, Flag(FieldFlags::Transient));
    REQUIRE(RegisterOne(types, 4, slots, Key(3), true));
    const usize transientHolders[] = {2, 4};
    for (usize index : transientHolders) {
        auto lease = types.AcquireValueType(StructId(index), TypeKind::Struct);
        REQUIRE(lease);
        CHECK(ViewOf(lease.Value()).ContainsStrongReference());
        CHECK(ViewOf(lease.Value()).ContainsReferenceArray());
        CHECK(!ViewOf(lease.Value()).ContainsRequired());
    }

    CHECK(types.Shutdown());
}
