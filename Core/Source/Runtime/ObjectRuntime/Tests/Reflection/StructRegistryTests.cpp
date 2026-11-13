#include <Core/Memory/SystemAllocator.h>
#include <Object/ObjectRegistry.h>
#include <Object/TypeRegistry.h>
#include <TestSupport/Test.h>

// 리플렉션 struct 등록 규칙 시험이다(Docs/Design/ReflectionFoundations.ko.md 4.6절). 레지스트리는
// struct 메모리를 만지지 않으므로 가짜 native 키와 빈 함수로 규칙만 검증한다: 공유 필드 검증,
// 함수표 1:1, trace와 강한 참조의 일치, 깊이 MaxStructDepth, native 대조, 의존 Busy, WorldOnly 형질
// 전파, 클래스 struct 필드의 접근기 규칙과 write Unsupported, StructView.
using namespace rocks;

namespace {

uint8 NativeKeys[24];

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
    return TypeId{0x535452, 100 + index};
}

constexpr Guid ValueId{0x564c, 1};
constexpr Guid InnerId{0x564c, 2};
constexpr Guid TargetId{0x564c, 3};

// 값 멤버 하나짜리 struct(index의 ID·native 키)를 등록한다.
Result<TypeRegistrationToken> RegisterLeaf(TypeRegistry& types, usize index,
                                           bool required = false) noexcept {
    const FieldSchema fields[] = {required
                                      ? Field(TargetId, "target", 6, ValueKind::ObjectReference, {},
                                              static_cast<uint32>(FieldFlags::Required))
                                      : Field(ValueId, "value", 5, ValueKind::UInt32)};
    const RuntimeStructFieldOps ops[] = {Member(required ? TargetId : ValueId)};
    return types.RegisterStruct({StructId(index), 1, fields, {"Leaf", 4}},
                                StructOps(index, ops, required));
}

// inner(index)를 struct 멤버로 담는 struct(outer)를 등록한다.
Result<TypeRegistrationToken> RegisterWrapper(TypeRegistry& types, usize outer, usize inner,
                                              bool traced = false) noexcept {
    const FieldSchema fields[] = {Field(InnerId, "inner", 5, ValueKind::Struct, StructId(inner))};
    const RuntimeStructFieldOps ops[] = {Member(InnerId, Key(inner))};
    return types.RegisterStruct({StructId(outer), 1, fields, {"Wrapper", 7}},
                                StructOps(outer, ops, traced));
}

struct Owner final : Object {
    uint8 storage[32] = {};
};

constexpr TypeId OwnerType{0x4f574e, 1};

} // namespace

TEST(StructRegistry_RegistrationRules) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    const FieldSchema value[] = {Field(ValueId, "value", 5, ValueKind::UInt32)};
    const RuntimeStructFieldOps valueOps[] = {Member(ValueId)};
    const TypeSchema leaf{StructId(0), 1, value, {"Leaf", 4}};

    // 스키마·실행 정보의 기본 규칙.
    CHECK(types.RegisterStruct({StructId(0), 0, value, {"Leaf", 4}}, StructOps(0, valueOps))
              .Error() == ErrorCode::InvalidArgument);
    CHECK(types
              .RegisterStruct({StructId(0), 1, value, {"Leaf", 4}, StructId(9)},
                              StructOps(0, valueOps))
              .Error() == ErrorCode::InvalidArgument);
    CHECK(
        types
            .RegisterStruct(
                {StructId(0), 1, value, {"Leaf", 4}, {}, static_cast<uint32>(TypeFlags::Abstract)},
                StructOps(0, valueOps))
            .Error() == ErrorCode::InvalidArgument);
    RuntimeStructOps noConstruct = StructOps(0, valueOps);
    noConstruct.construct = nullptr;
    CHECK(types.RegisterStruct(leaf, Move(noConstruct)).Error() == ErrorCode::InvalidArgument);

    // Opaque 필드(부분 metadata)와 함수표 1:1 위반.
    FieldSchema opaque = Field(ValueId, "value", 5, ValueKind::Opaque);
    CHECK(types
              .RegisterStruct({StructId(0), 1, Span<const FieldSchema>(&opaque, 1), {"Leaf", 4}},
                              StructOps(0, valueOps))
              .Error() == ErrorCode::InvalidArgument);
    const RuntimeStructFieldOps wrongId[] = {Member(InnerId)};
    CHECK(types.RegisterStruct(leaf, StructOps(0, wrongId)).Error() == ErrorCode::InvalidArgument);
    RuntimeStructFieldOps halfCodec[] = {Member(ValueId)};
    halfCodec[0].decode = nullptr;
    CHECK(types.RegisterStruct(leaf, StructOps(0, halfCodec)).Error() ==
          ErrorCode::InvalidArgument);
    RuntimeStructFieldOps noCodec[] = {Member(ValueId)};
    noCodec[0].encode = nullptr;
    noCodec[0].decode = nullptr;
    CHECK(types.RegisterStruct(leaf, StructOps(0, noCodec)).Error() == ErrorCode::InvalidArgument);
    // Transient 필드는 codec 없이 등록된다.
    FieldSchema transient = Field(ValueId, "value", 5, ValueKind::UInt32, {},
                                  static_cast<uint32>(FieldFlags::Transient));
    auto transientToken =
        types.RegisterStruct({StructId(1), 1, Span<const FieldSchema>(&transient, 1), {"Cache", 5}},
                             StructOps(1, noCodec));
    REQUIRE(transientToken);
    REQUIRE(types.Unregister(transientToken.Value()));

    // trace는 강한 참조 멤버가 있을 때만 있어야 한다.
    CHECK(types.RegisterStruct(leaf, StructOps(0, valueOps, true)).Error() ==
          ErrorCode::InvalidArgument);
    const FieldSchema reference[] = {Field(TargetId, "target", 6, ValueKind::ObjectReference)};
    const RuntimeStructFieldOps referenceOps[] = {Member(TargetId)};
    CHECK(types
              .RegisterStruct({StructId(2), 1, reference, {"Edge", 4}},
                              StructOps(2, referenceOps, false))
              .Error() == ErrorCode::InvalidArgument);

    // 같은 ID, 같은 native의 다른 ID는 AlreadyExists다.
    auto token = types.RegisterStruct(leaf, StructOps(0, valueOps));
    REQUIRE(token);
    CHECK(types.RegisterStruct(leaf, StructOps(3, valueOps)).Error() == ErrorCode::AlreadyExists);
    CHECK(types.RegisterStruct({StructId(4), 1, value, {"Alias", 5}}, StructOps(0, valueOps))
              .Error() == ErrorCode::AlreadyExists);

    // Struct 멤버: 미등록 NotFound, native 키 다름 TypeMismatch.
    CHECK(RegisterWrapper(types, 5, 6).Error() == ErrorCode::NotFound);
    const FieldSchema wrongKey[] = {Field(InnerId, "inner", 5, ValueKind::Struct, StructId(0))};
    const RuntimeStructFieldOps wrongKeyOps[] = {Member(InnerId, Key(7))};
    CHECK(
        types.RegisterStruct({StructId(5), 1, wrongKey, {"Wrapper", 7}}, StructOps(5, wrongKeyOps))
            .Error() == ErrorCode::TypeMismatch);

    // struct는 Acquire(클래스 전용)로 열리지 않고 AcquireValueType으로 열린다.
    CHECK(types.Acquire(StructId(0)).Error() == ErrorCode::TypeMismatch);
    {
        auto lease = types.AcquireValueType(StructId(0), TypeKind::Struct);
        REQUIRE(lease);
        CHECK(lease.Value().Kind() == TypeKind::Struct);
        CHECK(lease.Value().Schema().fields.Size() == 1);
        CHECK(lease.Value().Ops().nativeType == Key(0));
        CHECK(lease.Value().StructOps().construct == &Construct);
        CHECK(lease.Value().Struct().Depth() == 1);
    }

    REQUIRE(types.Unregister(token.Value()));
    CHECK(types.Shutdown());
}

// 깊이 = 1 + 멤버 struct 깊이. MaxStructDepth(16)까지 등록되고 17은 거부된다. 의존 대상은 의존자가
// 있는 동안 Unregister가 Busy다.
TEST(StructRegistry_DepthAndDependencies) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    TypeRegistrationToken tokens[MaxStructDepth];
    auto first = RegisterLeaf(types, 0);
    REQUIRE(first);
    tokens[0] = first.Value();
    for (usize level = 1; level < MaxStructDepth; ++level) {
        auto token = RegisterWrapper(types, level, level - 1);
        REQUIRE(token);
        tokens[level] = token.Value();
    }

    {
        auto top = types.AcquireValueType(StructId(MaxStructDepth - 1), TypeKind::Struct);
        REQUIRE(top);
        CHECK(top.Value().Struct().Depth() == MaxStructDepth);
        CHECK(top.Value().Struct().FieldStruct(0).Depth() == MaxStructDepth - 1);
    }

    CHECK(RegisterWrapper(types, MaxStructDepth, MaxStructDepth - 1).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(types.Unregister(tokens[0]).Error() == ErrorCode::Busy);
    for (usize level = MaxStructDepth; level-- > 0;) {
        REQUIRE(types.Unregister(tokens[level]));
    }

    CHECK(types.Shutdown());
}

// StructView: 필드 ID 정렬 색인, 멤버 enum·struct, 계산 형질(강한 참조·Required·WorldOnly 전파).
TEST(StructRegistry_ViewAndComputedTraits) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    auto edge = RegisterLeaf(types, 0, true);
    REQUIRE(edge);
    {
        // 참조 struct를 담으면 강한 참조·Required가 전파되고 trace가 필요하다.
        CHECK(RegisterWrapper(types, 1, 0, false).Error() == ErrorCode::InvalidArgument);
        auto wrapper = RegisterWrapper(types, 1, 0, true);
        REQUIRE(wrapper);
        auto lease = types.AcquireValueType(StructId(1), TypeKind::Struct);
        REQUIRE(lease);
        const StructView view = lease.Value().Struct();
        CHECK(view.ContainsStrongReference());
        CHECK(view.ContainsRequired());
        CHECK(!view.RequiresWorldOnlyOwner());
        CHECK(view.FieldIndex(InnerId) == 0);
        CHECK(view.FieldIndex(ValueId) == StructView::NoField);
        CHECK(view.FieldEnum(0) == nullptr);
        CHECK(!view.FieldStruct(0).IsEmpty());
        CHECK(view.FieldStruct(0).Schema().id == StructId(0));
        CHECK(view.FieldOps(0).fieldId == InnerId);
        lease.Value().Reset();
        REQUIRE(types.Unregister(wrapper.Value()));
    }

    // 여러 필드의 정렬 색인.
    const FieldSchema many[] = {Field(Guid{9, 9}, "c", 1, ValueKind::UInt32),
                                Field(Guid{1, 1}, "a", 1, ValueKind::UInt32),
                                Field(Guid{5, 5}, "b", 1, ValueKind::UInt32)};
    const RuntimeStructFieldOps manyOps[] = {Member(Guid{5, 5}), Member(Guid{9, 9}),
                                             Member(Guid{1, 1})};
    auto sorted = types.RegisterStruct({StructId(2), 1, many, {"Many", 4}}, StructOps(2, manyOps));
    REQUIRE(sorted);
    {
        auto lease = types.AcquireValueType(StructId(2), TypeKind::Struct);
        REQUIRE(lease);
        const StructView view = lease.Value().Struct();
        CHECK(view.FieldIndex(Guid{9, 9}) == 0);
        CHECK(view.FieldIndex(Guid{1, 1}) == 1);
        CHECK(view.FieldIndex(Guid{5, 5}) == 2);
        // 멤버 함수표는 스키마 순서로 복사된다.
        CHECK(view.FieldOps(0).fieldId == (Guid{9, 9}));
        CHECK(view.FieldOps(2).fieldId == (Guid{5, 5}));
        CHECK(!view.ContainsRequired());
    }

    REQUIRE(types.Unregister(sorted.Value()));
    REQUIRE(types.Unregister(edge.Value()));
    CHECK(types.Shutdown());
}

// 클래스의 Struct 필드: read·write·writeStaged가 없어야 하고 encode·decode와 native 키가 있어야
// 한다. 등록된 write는 Unsupported다. WorldOnly 소유자를 요구하는 struct는 WorldOnly 클래스만 쓸 수
// 있고, 형질은 바깥 struct로 전파된다.
TEST(StructRegistry_ClassStructFields) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    auto leaf = RegisterLeaf(types, 0);
    REQUIRE(leaf);
    const FieldSchema fields[] = {Field(InnerId, "inner", 5, ValueKind::Struct, StructId(0))};
    auto registerOwner = [&](RuntimeFieldOps field, uint32 flags = 0) noexcept {
        auto ops = MakeRuntimeTypeOps<Owner>();
        ops.fields = Span<const RuntimeFieldOps>(&field, 1);
        ops.completeFieldLayout = true;
        return types.Register({OwnerType, 1, fields, {"Owner", 5}, {}, flags}, Move(ops));
    };
    RuntimeFieldOps structField{InnerId, nullptr, {}, nullptr, &ClassEncode, &ClassDecode, Key(0)};
    RuntimeFieldOps withRead = structField;
    withRead.read = [](ObjectRegistry&, const ObjectAccessContext&, ObjectHandle,
                       FieldValue&) noexcept { return Result<void>::Success(); };
    CHECK(registerOwner(withRead).Error() == ErrorCode::InvalidArgument);
    RuntimeFieldOps noCodec = structField;
    noCodec.encode = nullptr;
    noCodec.decode = nullptr;
    CHECK(registerOwner(noCodec).Error() == ErrorCode::InvalidArgument);
    RuntimeFieldOps noKey = structField;
    noKey.valueNativeType = nullptr;
    CHECK(registerOwner(noKey).Error() == ErrorCode::InvalidArgument);
    RuntimeFieldOps wrongKey = structField;
    wrongKey.valueNativeType = Key(9);
    CHECK(registerOwner(wrongKey).Error() == ErrorCode::TypeMismatch);

    auto owner = registerOwner(structField);
    REQUIRE(owner);
    {
        // struct는 클래스가 쓰는 동안 Unregister가 Busy다.
        CHECK(types.Unregister(leaf.Value()).Error() == ErrorCode::Busy);
        auto lease = types.Acquire(OwnerType);
        REQUIRE(lease);
        CHECK(lease.Value().FieldEnum(0) == nullptr);
        CHECK(lease.Value().FieldStruct(0).Schema().id == StructId(0));
        ObjectRegistry objects(allocator, types, {0x5352, 1});
        {
            // 문맥·대상 검사(빈 handle은 그 단계의 오류) 뒤 struct 필드 쓰기는 Unsupported다.
            FieldValue value(allocator);
            const auto& write = lease.Value().Ops().fields[0].write;
            CHECK(write(objects, objects.OwnerAccess(), ObjectHandle{}, value).Error() !=
                  ErrorCode::Unsupported);
            auto object = objects.NewObject<Owner>(objects.OwnerAccess(), OwnerType);
            REQUIRE(object);
            CHECK(write(objects, objects.OwnerAccess(), object.Value().Handle(), value).Error() ==
                  ErrorCode::Unsupported);
        }

        lease.Value().Reset();
        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
    }

    REQUIRE(types.Unregister(owner.Value()));

    // WorldOnly 소유자를 요구하는 struct와 그 전파.
    const FieldSchema value[] = {Field(ValueId, "value", 5, ValueKind::UInt32)};
    const RuntimeStructFieldOps valueOps[] = {Member(ValueId)};
    RuntimeStructOps worldOnly = StructOps(1, valueOps);
    worldOnly.requiresWorldOnlyOwner = true;
    auto world = types.RegisterStruct({StructId(1), 1, value, {"World", 5}}, Move(worldOnly));
    REQUIRE(world);
    auto wrapper = RegisterWrapper(types, 2, 1);
    REQUIRE(wrapper);
    {
        auto lease = types.AcquireValueType(StructId(2), TypeKind::Struct);
        REQUIRE(lease);
        CHECK(lease.Value().Struct().RequiresWorldOnlyOwner());
        CHECK(lease.Value().StructOps().requiresWorldOnlyOwner);
    }

    const FieldSchema worldFields[] = {Field(InnerId, "inner", 5, ValueKind::Struct, StructId(2))};
    auto registerWorldOwner = [&](uint32 flags) noexcept {
        RuntimeFieldOps field{InnerId, nullptr, {}, nullptr, &ClassEncode, &ClassDecode, Key(2)};
        auto ops = MakeRuntimeTypeOps<Owner>();
        ops.fields = Span<const RuntimeFieldOps>(&field, 1);
        ops.completeFieldLayout = true;
        return types.Register({OwnerType, 1, worldFields, {"Owner", 5}, {}, flags}, Move(ops));
    };
    CHECK(registerWorldOwner(0).Error() == ErrorCode::InvalidArgument);
    auto worldOwner = registerWorldOwner(static_cast<uint32>(TypeFlags::WorldOnly));
    REQUIRE(worldOwner);
    REQUIRE(types.Unregister(worldOwner.Value()));
    REQUIRE(types.Unregister(wrapper.Value()));
    REQUIRE(types.Unregister(world.Value()));
    REQUIRE(types.Unregister(leaf.Value()));
    CHECK(types.Shutdown());
}

namespace {

constexpr TypeId EnumId{0x454e55, 1};
constexpr EnumeratorSchema EnumValues[] = {{{"One", 3}, {}, {}, 1, 0}};

Result<TypeRegistrationToken> RegisterTestEnum(TypeRegistry& types) noexcept {
    return types.RegisterEnum({EnumId,
                               {"Choice", 6},
                               ValueKind::UInt8,
                               0u,
                               Span<const EnumeratorSchema>(EnumValues, 1),
                               {}},
                              RuntimeEnumOps{Key(20), {}});
}

} // namespace

// 교차검증이 짚은 규칙: Transient 참조만 있어도 trace가 필요하고, 멤버 대상의 종류가 다르면
// TypeMismatch, 함수표의 같은 ID 둘은 InvalidArgument, 공유 ID 공간 충돌은 AlreadyExists, 기본 실행
// 정보 누락은 InvalidArgument다. 필드 ID 정렬은 high가 같고 low만 달라도 맞다.
TEST(StructRegistry_CrossValidatedRules) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    auto choice = RegisterTestEnum(types);
    REQUIRE(choice);

    // Transient ObjectRef만 있는 struct도 GC가 추적하므로 trace가 필요하다.
    FieldSchema transientEdge = Field(TargetId, "target", 6, ValueKind::ObjectReference, {},
                                      static_cast<uint32>(FieldFlags::Transient));
    const RuntimeStructFieldOps edgeOps[] = {Member(TargetId)};
    CHECK(
        types
            .RegisterStruct({StructId(0), 1, Span<const FieldSchema>(&transientEdge, 1), {"E", 1}},
                            StructOps(0, edgeOps, false))
            .Error() == ErrorCode::InvalidArgument);

    // Transient Required 참조는 저장·복원하지 않으므로 ContainsRequired에 세지 않는다(trace는
    // 필요).
    FieldSchema transientRequired = Field(TargetId, "target", 6, ValueKind::ObjectReference, {},
                                          static_cast<uint32>(FieldFlags::Transient) |
                                              static_cast<uint32>(FieldFlags::Required));
    auto cacheStruct = types.RegisterStruct(
        {StructId(9), 1, Span<const FieldSchema>(&transientRequired, 1), {"C", 1}},
        StructOps(9, edgeOps, true));
    REQUIRE(cacheStruct);
    {
        auto lease = types.AcquireValueType(StructId(9), TypeKind::Struct);
        REQUIRE(lease);
        CHECK(lease.Value().Struct().ContainsStrongReference());
        CHECK(!lease.Value().Struct().ContainsRequired());
    }

    // 종류 불일치: Struct 필드가 enum ID를, Enum 멤버가 struct ID를 가리킨다.
    const FieldSchema structToEnum[] = {Field(InnerId, "inner", 5, ValueKind::Struct, EnumId)};
    const RuntimeStructFieldOps structToEnumOps[] = {Member(InnerId, Key(20))};
    CHECK(
        types
            .RegisterStruct({StructId(1), 1, structToEnum, {"S", 1}}, StructOps(1, structToEnumOps))
            .Error() == ErrorCode::TypeMismatch);
    auto leaf = RegisterLeaf(types, 2);
    REQUIRE(leaf);
    const FieldSchema enumToStruct[] = {Field(InnerId, "inner", 5, ValueKind::Enum, StructId(2))};
    const RuntimeStructFieldOps enumToStructOps[] = {Member(InnerId, Key(2))};
    CHECK(
        types
            .RegisterStruct({StructId(3), 1, enumToStruct, {"S", 1}}, StructOps(3, enumToStructOps))
            .Error() == ErrorCode::TypeMismatch);

    // 함수표의 같은 ID 둘(필드 둘).
    const FieldSchema two[] = {Field(Guid{7, 1}, "a", 1, ValueKind::UInt32),
                               Field(Guid{7, 2}, "b", 1, ValueKind::UInt32)};
    const RuntimeStructFieldOps duplicated[] = {Member(Guid{7, 1}), Member(Guid{7, 1})};
    CHECK(types.RegisterStruct({StructId(4), 1, two, {"T", 1}}, StructOps(4, duplicated)).Error() ==
          ErrorCode::InvalidArgument);

    // 공유 ID 공간: enum ID와 같은 struct ID는 AlreadyExists다.
    const FieldSchema value[] = {Field(ValueId, "value", 5, ValueKind::UInt32)};
    const RuntimeStructFieldOps valueOps[] = {Member(ValueId)};
    CHECK(types.RegisterStruct({EnumId, 1, value, {"V", 1}}, StructOps(5, valueOps)).Error() ==
          ErrorCode::AlreadyExists);

    // 기본 실행 정보 누락.
    const TypeSchema plain{StructId(6), 1, value, {"P", 1}};
    RuntimeStructOps zeroSize = StructOps(6, valueOps);
    zeroSize.size = 0;
    CHECK(types.RegisterStruct(plain, Move(zeroSize)).Error() == ErrorCode::InvalidArgument);
    RuntimeStructOps oddAlignment = StructOps(6, valueOps);
    oddAlignment.alignment = 3;
    CHECK(types.RegisterStruct(plain, Move(oddAlignment)).Error() == ErrorCode::InvalidArgument);
    RuntimeStructOps noNative = StructOps(6, valueOps);
    noNative.nativeType = nullptr;
    CHECK(types.RegisterStruct(plain, Move(noNative)).Error() == ErrorCode::InvalidArgument);
    RuntimeStructOps noDestroy = StructOps(6, valueOps);
    noDestroy.destroy = nullptr;
    CHECK(types.RegisterStruct(plain, Move(noDestroy)).Error() == ErrorCode::InvalidArgument);

    // 정렬 색인은 high가 같고 low만 다른 ID도 구분한다. struct 안 enum 멤버의 FieldEnum.
    const FieldSchema sameHigh[] = {Field(Guid{3, 9}, "c", 1, ValueKind::UInt32),
                                    Field(Guid{3, 1}, "a", 1, ValueKind::Enum, EnumId),
                                    Field(Guid{3, 5}, "b", 1, ValueKind::UInt32)};
    const RuntimeStructFieldOps sameHighOps[] = {Member(Guid{3, 9}), Member(Guid{3, 1}, Key(20)),
                                                 Member(Guid{3, 5})};
    auto ordered =
        types.RegisterStruct({StructId(7), 1, sameHigh, {"O", 1}}, StructOps(7, sameHighOps));
    REQUIRE(ordered);
    {
        auto lease = types.AcquireValueType(StructId(7), TypeKind::Struct);
        REQUIRE(lease);
        const StructView view = lease.Value().Struct();
        CHECK(view.FieldIndex(Guid{3, 9}) == 0);
        CHECK(view.FieldIndex(Guid{3, 1}) == 1);
        CHECK(view.FieldIndex(Guid{3, 5}) == 2);
        CHECK(view.FieldIndex(Guid{3, 2}) == StructView::NoField);
        CHECK(view.FieldEnum(1) == lease.Value().FieldEnum(1));
        CHECK(view.FieldEnum(1) != nullptr);
        CHECK(view.FieldEnum(0) == nullptr);
    }

    // 클래스 struct 필드: write·writeStaged가 있으면 거부, Transient struct 필드는 codec 없이
    // 등록된다.
    const FieldSchema owned[] = {Field(InnerId, "inner", 5, ValueKind::Struct, StructId(2))};
    auto registerOwner = [&](RuntimeFieldOps field, uint32 fieldFlags) noexcept {
        FieldSchema schema = owned[0];
        schema.flags = fieldFlags;
        auto ops = MakeRuntimeTypeOps<Owner>();
        ops.fields = Span<const RuntimeFieldOps>(&field, 1);
        ops.completeFieldLayout = true;
        return types.Register({OwnerType, 1, Span<const FieldSchema>(&schema, 1), {"Owner", 5}},
                              Move(ops));
    };
    RuntimeFieldOps withStaged{InnerId, nullptr, {}, nullptr, &ClassEncode, &ClassDecode, Key(2)};
    withStaged.writeStaged = [](ConstructionContext&, ObjectHandle, const FieldValue&) noexcept {
        return Result<void>::Success();
    };
    CHECK(registerOwner(withStaged, 0).Error() == ErrorCode::InvalidArgument);
    RuntimeFieldOps cache{InnerId, nullptr, {}, nullptr, nullptr, nullptr, Key(2)};
    CHECK(registerOwner(cache, 0).Error() == ErrorCode::InvalidArgument);
    auto owner = registerOwner(cache, static_cast<uint32>(FieldFlags::Transient));
    REQUIRE(owner);
    // 클래스 ID와 같은 struct ID도 AlreadyExists다.
    CHECK(types.RegisterStruct({OwnerType, 1, value, {"V", 1}}, StructOps(8, valueOps)).Error() ==
          ErrorCode::AlreadyExists);

    // 클래스 → struct → enum 사슬이 살아 있는 채로 Shutdown한다(의존자부터 파괴).
    CHECK(types.Shutdown());
}
