#include <CoreTypeBindings/CoreTypeBindings.module.generated.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Traits.h>
#include <Object/TypeRegistry.h>
#include <TestSupport/Test.h>

// 리플렉션 B9b2 struct Atomic 시험이다(Docs/Design/ReflectionExtensions.ko.md 3.1·3.2절):
// RegisterStruct의 struct 플래그 mask(Atomic만), 클래스 Atomic 비트 거부, StructView::IsAtomic과
// 비전파, Core 값 타입 미러 17종 중 Transform을 뺀 16종의 Atomic, Transform3f·Transform3d 미러와
// Core 타입의 멤버 대조. 차이 쓰기·override 단위(A5)는 여기서 다루지 않는다.
using namespace rocks;
using core_type_bindings::Transform3dBinding;
using core_type_bindings::Transform3fBinding;

// 미러 선언과 실제 Core 타입의 멤버 형식·선언 순서가 같다(생성 VerifyBinds가 실제 타입 쪽을 다시
// 확인한다 — 여기서는 미러 선언 자체를 Core 선언과 맞춘다).
static_assert(
    IsSameV<decltype(Transform3fBinding::translation), decltype(Transform3f::translation)>,
    "Transform3fBinding::translation must have the type of Transform3f::translation");
static_assert(IsSameV<decltype(Transform3fBinding::rotation), decltype(Transform3f::rotation)>,
              "Transform3fBinding::rotation must have the type of Transform3f::rotation");
static_assert(IsSameV<decltype(Transform3fBinding::scale), decltype(Transform3f::scale)>,
              "Transform3fBinding::scale must have the type of Transform3f::scale");
static_assert(
    IsSameV<decltype(Transform3dBinding::translation), decltype(Transform3d::translation)>,
    "Transform3dBinding::translation must have the type of Transform3d::translation");
static_assert(IsSameV<decltype(Transform3dBinding::rotation), decltype(Transform3d::rotation)>,
              "Transform3dBinding::rotation must have the type of Transform3d::rotation");
static_assert(IsSameV<decltype(Transform3dBinding::scale), decltype(Transform3d::scale)>,
              "Transform3dBinding::scale must have the type of Transform3d::scale");
static_assert(ROCKS_OFFSET_OF(Transform3f, translation) < ROCKS_OFFSET_OF(Transform3f, rotation) &&
                  ROCKS_OFFSET_OF(Transform3f, rotation) < ROCKS_OFFSET_OF(Transform3f, scale),
              "Transform3f members must be declared translation, rotation, scale");
static_assert(ROCKS_OFFSET_OF(Transform3d, translation) < ROCKS_OFFSET_OF(Transform3d, rotation) &&
                  ROCKS_OFFSET_OF(Transform3d, rotation) < ROCKS_OFFSET_OF(Transform3d, scale),
              "Transform3d members must be declared translation, rotation, scale");
static_assert(ROCKS_OFFSET_OF(Transform3fBinding, translation) <
                      ROCKS_OFFSET_OF(Transform3fBinding, rotation) &&
                  ROCKS_OFFSET_OF(Transform3fBinding, rotation) <
                      ROCKS_OFFSET_OF(Transform3fBinding, scale),
              "Transform3fBinding members must follow the Transform3f declaration order");
static_assert(ROCKS_OFFSET_OF(Transform3dBinding, translation) <
                      ROCKS_OFFSET_OF(Transform3dBinding, rotation) &&
                  ROCKS_OFFSET_OF(Transform3dBinding, rotation) <
                      ROCKS_OFFSET_OF(Transform3dBinding, scale),
              "Transform3dBinding members must follow the Transform3d declaration order");
// 생성 특수화는 실제 타입에 있고 멤버 미러(Vec3·Quat 등)를 담아 깊이 2다.
static_assert(GeneratedType<Transform3f>::Depth == 2 && GeneratedType<Transform3d>::Depth == 2,
              "Transform3 mirrors nest one level of member mirrors");
static_assert(!GeneratedType<Transform3f>::ContainsStrongReference &&
                  !GeneratedType<Transform3d>::ContainsStrongReference,
              "Transform3 mirrors hold no object references");

namespace {

constexpr uint32 AtomicFlag = static_cast<uint32>(TypeFlags::Atomic);

uint8 NativeKeys[8];

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

RuntimeStructFieldOps Member(Guid id, const void* valueNative = nullptr) noexcept {
    RuntimeStructFieldOps ops;
    ops.fieldId = id;
    ops.encode = &MemberEncode;
    ops.decode = &MemberDecode;
    ops.valueNativeType = valueNative;
    return ops;
}

RuntimeStructOps StructOps(usize key, Span<const RuntimeStructFieldOps> fields) noexcept {
    RuntimeStructOps ops;
    ops.size = 16;
    ops.alignment = 8;
    ops.nativeType = Key(key);
    ops.construct = &Construct;
    ops.destroy = &Destroy;
    ops.fields = fields;
    return ops;
}

TypeId StructId(usize index) noexcept {
    return TypeId{0x41544f4d, 100 + index};
}

constexpr Guid ValueId{0x41544f4d46, 1};
constexpr Guid InnerId{0x41544f4d46, 2};

// 값 멤버 하나짜리 struct(index의 ID·native 키)를 flags로 등록한다.
Result<TypeRegistrationToken> RegisterLeaf(TypeRegistry& types, usize index,
                                           uint32 flags) noexcept {
    FieldSchema value;
    value.id = ValueId;
    value.name = Span<const char>("value", 5);
    value.value = ValueType{ValueKind::UInt32, {}};
    const RuntimeStructFieldOps ops[] = {Member(ValueId)};
    return types.RegisterStruct(
        {StructId(index), 1, Span<const FieldSchema>(&value, 1), {"Leaf", 4}, {}, flags},
        StructOps(index, ops));
}

// inner(index)를 struct 멤버로 담는 struct(outer)를 flags로 등록한다.
Result<TypeRegistrationToken> RegisterWrapper(TypeRegistry& types, usize outer, usize inner,
                                              uint32 flags) noexcept {
    FieldSchema field;
    field.id = InnerId;
    field.name = Span<const char>("inner", 5);
    field.value = ValueType{ValueKind::Struct, StructId(inner)};
    const RuntimeStructFieldOps ops[] = {Member(InnerId, Key(inner))};
    return types.RegisterStruct(
        {StructId(outer), 1, Span<const FieldSchema>(&field, 1), {"Wrapper", 7}, {}, flags},
        StructOps(outer, ops));
}

struct AtomicOwner final : Object {};

constexpr TypeId OwnerId{0x41544f4d43, 1};

// 등록된 struct 레코드의 view로 Atomic 여부를 읽는다(없으면 false — 호출 쪽 REQUIRE가 등록을
// 확인한다).
bool LeaseIsAtomic(TypeRegistry& types, TypeId id) noexcept {
    auto lease = types.AcquireValueType(id, TypeKind::Struct);
    return lease && lease.Value().Struct().IsAtomic();
}

} // namespace

// RegisterStruct는 struct 플래그 Atomic만 받는다: 클래스 플래그(Abstract·WorldOnly)·예약 비트
// (4·8)·그 밖의 비트는 Atomic과 함께여도 InvalidArgument다. 받은 비트는 레코드 스키마에 남고
// IsAtomic이 읽는다.
TEST(AtomicStruct_RegisterStructAcceptsOnlyTheAtomicFlag) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    const uint32 rejected[] = {static_cast<uint32>(TypeFlags::Abstract),
                               static_cast<uint32>(TypeFlags::WorldOnly),
                               4u,
                               8u,
                               32u,
                               0x80000000u,
                               AtomicFlag | static_cast<uint32>(TypeFlags::Abstract),
                               AtomicFlag | static_cast<uint32>(TypeFlags::WorldOnly)};
    for (const uint32 flags : rejected) {
        CHECK(RegisterLeaf(types, 0, flags).Error() == ErrorCode::InvalidArgument);
    }

    auto atomic = RegisterLeaf(types, 0, AtomicFlag);
    REQUIRE(atomic);
    auto plain = RegisterLeaf(types, 1, 0);
    REQUIRE(plain);
    {
        auto lease = types.AcquireValueType(StructId(0), TypeKind::Struct);
        REQUIRE(lease);
        CHECK(lease.Value().Schema().flags == AtomicFlag);
        CHECK(HasFlag(lease.Value().Schema().flags, TypeFlags::Atomic));
        CHECK(lease.Value().Struct().IsAtomic());
        CHECK(lease.Value().Struct().Schema().flags == AtomicFlag);
    }

    CHECK(!LeaseIsAtomic(types, StructId(1)));
    REQUIRE(types.Unregister(plain.Value()));
    REQUIRE(types.Unregister(atomic.Value()));
    CHECK(types.Shutdown());
}

// Atomic은 선언 플래그라 멤버에서 바깥으로 전파하지 않는다: 비Atomic struct 안의 Atomic 멤버는 그
// 멤버 view만 Atomic이고, Atomic struct 안의 비Atomic 멤버 view는 비Atomic 그대로다(통째 쓰기는
// 바깥 Atomic이 정한다 — A5). 클래스 필드의 struct view도 같은 비트를 읽는다.
TEST(AtomicStruct_IsAtomicIsDeclaredPerStructAndNotPropagated) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    auto atomicLeaf = RegisterLeaf(types, 0, AtomicFlag);
    REQUIRE(atomicLeaf);
    auto plainLeaf = RegisterLeaf(types, 1, 0);
    REQUIRE(plainLeaf);
    auto plainOuter = RegisterWrapper(types, 2, 0, 0);
    REQUIRE(plainOuter);
    auto atomicOuter = RegisterWrapper(types, 3, 1, AtomicFlag);
    REQUIRE(atomicOuter);
    {
        auto plain = types.AcquireValueType(StructId(2), TypeKind::Struct);
        REQUIRE(plain);
        CHECK(!plain.Value().Struct().IsAtomic());
        CHECK(plain.Value().FieldStruct(0).IsAtomic());
        CHECK(plain.Value().Struct().FieldStruct(0).IsAtomic());
        auto atomic = types.AcquireValueType(StructId(3), TypeKind::Struct);
        REQUIRE(atomic);
        CHECK(atomic.Value().Struct().IsAtomic());
        CHECK(!atomic.Value().FieldStruct(0).IsAtomic());
    }

    // 클래스 필드가 가리키는 Atomic struct도 TypeLease::FieldStruct의 view로 읽는다.
    FieldSchema field;
    field.id = InnerId;
    field.name = Span<const char>("inner", 5);
    field.value = ValueType{ValueKind::Struct, StructId(0)};
    RuntimeFieldOps fieldOps;
    fieldOps.fieldId = InnerId;
    fieldOps.valueNativeType = Key(0);
    fieldOps.encode = +[](const RuntimeFieldOps&, ObjectRegistry&, const ObjectAccessContext&,
                          ObjectHandle, ArchiveWriter&, FieldEncodeContext&) noexcept {
        return Result<uint32>::Failure(ErrorCode::Unsupported);
    };
    fieldOps.decode =
        +[](const RuntimeFieldOps&, ConstructionContext*, ObjectHandle, const FieldView&,
            FieldDecodeContext&) noexcept { return Result<void>::Failure(ErrorCode::Unsupported); };
    RuntimeTypeOps ownerOps = MakeRuntimeTypeOps<AtomicOwner>();
    ownerOps.fields = Span<const RuntimeFieldOps>(&fieldOps, 1);
    auto owner = types.Register(
        {OwnerId, 1, Span<const FieldSchema>(&field, 1), {"AtomicOwner", 11}}, Move(ownerOps));
    REQUIRE(owner);
    {
        auto lease = types.Acquire(OwnerId);
        REQUIRE(lease);
        CHECK(lease.Value().FieldStruct(0).IsAtomic());
    }

    REQUIRE(types.Unregister(owner.Value()));
    REQUIRE(types.Unregister(atomicOuter.Value()));
    REQUIRE(types.Unregister(plainOuter.Value()));
    REQUIRE(types.Unregister(plainLeaf.Value()));
    REQUIRE(types.Unregister(atomicLeaf.Value()));
    CHECK(types.Shutdown());
}

// 클래스 등록은 struct 전용 Atomic 비트를 모르는 비트로 거부한다(단독·클래스 플래그와 함께). 각
// 사례는 Atomic 비트를 뺀 같은 스키마·함수표가 등록되는 짝과 대조한다 — Abstract 짝은 construct가
// 없는 함수표라 Abstract 일치 규칙이 아니라 Atomic 비트가 거부 사유임을 드러낸다.
TEST(AtomicStruct_ClassRegistrationRejectsTheAtomicBit) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    const uint32 classFlags[] = {0u, static_cast<uint32>(TypeFlags::WorldOnly),
                                 static_cast<uint32>(TypeFlags::Abstract)};
    for (const uint32 flags : classFlags) {
        auto ops = [flags]() noexcept {
            RuntimeTypeOps result = MakeRuntimeTypeOps<AtomicOwner>();
            if (HasFlag(flags, TypeFlags::Abstract)) {
                result.construct = nullptr;
            }

            return result;
        };
        TypeSchema schema{OwnerId, 1, {}, {"AtomicOwner", 11}};
        schema.flags = flags | AtomicFlag;
        CHECK(types.Register(schema, ops()).Error() == ErrorCode::InvalidArgument);
        CHECK(!types.Acquire(OwnerId));
        schema.flags = flags;
        auto accepted = types.Register(schema, ops());
        REQUIRE(accepted);
        REQUIRE(types.Unregister(accepted.Value()));
    }

    CHECK(types.Stats().registeredTypes == 0);
    CHECK(types.Shutdown());
}

// CoreTypeBindings 미러 17종이 등록되고 혼합 정밀도 Transform을 뺀 16종이 Atomic이다. Transform의
// 멤버(Vec3d·Quat·Vec3)는 각각 Atomic 미러라 멤버마다 통째다.
TEST(AtomicStruct_CoreValueMirrorsAreAtomicExceptTransform) {
    struct Mirror {
        TypeId id;
        const void* native;
        StringView name;
        bool atomic;
    };

    const Mirror mirrors[] = {
        {TypeIdOf<Vec2>(), NativeTypeKey<Vec2>(), "rocks::core_type_bindings::Vec2Binding", true},
        {TypeIdOf<Vec2d>(), NativeTypeKey<Vec2d>(), "rocks::core_type_bindings::Vec2dBinding",
         true},
        {TypeIdOf<Vec3>(), NativeTypeKey<Vec3>(), "rocks::core_type_bindings::Vec3Binding", true},
        {TypeIdOf<Vec3d>(), NativeTypeKey<Vec3d>(), "rocks::core_type_bindings::Vec3dBinding",
         true},
        {TypeIdOf<Vec4>(), NativeTypeKey<Vec4>(), "rocks::core_type_bindings::Vec4Binding", true},
        {TypeIdOf<Vec4d>(), NativeTypeKey<Vec4d>(), "rocks::core_type_bindings::Vec4dBinding",
         true},
        {TypeIdOf<Quat>(), NativeTypeKey<Quat>(), "rocks::core_type_bindings::QuatBinding", true},
        {TypeIdOf<Quatd>(), NativeTypeKey<Quatd>(), "rocks::core_type_bindings::QuatdBinding",
         true},
        {TypeIdOf<Rotator<float>>(), NativeTypeKey<Rotator<float>>(),
         "rocks::core_type_bindings::RotatorBinding", true},
        {TypeIdOf<Rotator<double>>(), NativeTypeKey<Rotator<double>>(),
         "rocks::core_type_bindings::RotatordBinding", true},
        {TypeIdOf<Color>(), NativeTypeKey<Color>(), "rocks::core_type_bindings::ColorBinding",
         true},
        {TypeIdOf<LinearColor>(), NativeTypeKey<LinearColor>(),
         "rocks::core_type_bindings::LinearColorBinding", true},
        {TypeIdOf<IntVec2>(), NativeTypeKey<IntVec2>(), "rocks::core_type_bindings::IntVec2Binding",
         true},
        {TypeIdOf<IntVec3>(), NativeTypeKey<IntVec3>(), "rocks::core_type_bindings::IntVec3Binding",
         true},
        {TypeIdOf<Transform3f>(), NativeTypeKey<Transform3f>(),
         "rocks::core_type_bindings::Transform3fBinding", true},
        {TypeIdOf<Transform3d>(), NativeTypeKey<Transform3d>(),
         "rocks::core_type_bindings::Transform3dBinding", true},
        {TypeIdOf<Transform>(), NativeTypeKey<Transform>(),
         "rocks::core_type_bindings::TransformBinding", false},
    };
    constexpr usize MirrorCount = sizeof(mirrors) / sizeof(mirrors[0]);
    static_assert(MirrorCount == 17, "CoreTypeBindings declares 17 Core value type mirrors");

    SystemAllocator allocator;
    TypeRegistry types(allocator);
    {
        ModuleRegistration registration(types, allocator);
        REQUIRE(generated::CoreTypeBindings::RegisterModuleTypes(registration));
        // 모듈이 미러 17종만 등록한다(표의 ID가 모두 다르고 빠진 미러가 없다).
        CHECK(types.Stats().registeredTypes == MirrorCount);
        usize atomic = 0;
        for (const Mirror& mirror : mirrors) {
            auto lease = types.AcquireValueType(mirror.id, TypeKind::Struct);
            REQUIRE(lease);
            CHECK(lease.Value().Name() == mirror.name);
            CHECK(lease.Value().StructOps().nativeType == mirror.native);
            CHECK(lease.Value().Struct().IsAtomic() == mirror.atomic);
            CHECK(HasFlag(lease.Value().Schema().flags, TypeFlags::Atomic) == mirror.atomic);
            atomic += mirror.atomic ? 1 : 0;
        }

        CHECK(atomic == 16);
        {
            auto transform = types.AcquireValueType(TypeIdOf<Transform>(), TypeKind::Struct);
            REQUIRE(transform);
            const StructView view = transform.Value().Struct();
            REQUIRE(view.Schema().fields.Size() == 3);
            for (usize index = 0; index < view.Schema().fields.Size(); ++index) {
                CHECK(view.FieldStruct(index).IsAtomic());
            }
        }

        REQUIRE(registration.Unregister());
    }

    CHECK(types.Shutdown());
}

// Transform3f·Transform3d 미러는 멤버 translation·rotation·scale을 정밀도에 맞는 벡터·quaternion
// 미러로 등록하고, 실제 타입의 크기·정렬을 쓴다. 스키마 필드 순서는 생성 순서(필드 ID 순)라
// 이름으로 찾는다 — 선언 순서는 파일 머리의 static_assert와 생성 VerifyBinds가 고정한다.
TEST(AtomicStruct_Transform3MirrorsMatchCoreTypes) {
    struct Expected {
        TypeId transform;
        usize size;
        usize alignment;
        TypeId vector;
        TypeId rotation;
    };

    const Expected expected[] = {
        {TypeIdOf<Transform3f>(), sizeof(Transform3f), alignof(Transform3f), TypeIdOf<Vec3>(),
         TypeIdOf<Quat>()},
        {TypeIdOf<Transform3d>(), sizeof(Transform3d), alignof(Transform3d), TypeIdOf<Vec3d>(),
         TypeIdOf<Quatd>()},
    };
    const StringView names[] = {"translation", "rotation", "scale"};

    SystemAllocator allocator;
    TypeRegistry types(allocator);
    {
        ModuleRegistration registration(types, allocator);
        REQUIRE(generated::CoreTypeBindings::RegisterModuleTypes(registration));
        for (const Expected& item : expected) {
            auto lease = types.AcquireValueType(item.transform, TypeKind::Struct);
            REQUIRE(lease);
            CHECK(lease.Value().Ops().size == item.size);
            CHECK(lease.Value().Ops().alignment == item.alignment);
            const StructView view = lease.Value().Struct();
            CHECK(view.Depth() == 2);
            CHECK(!view.ContainsStrongReference());
            const Span<const FieldSchema> fields = view.Schema().fields;
            REQUIRE(fields.Size() == 3);
            const TypeId targets[] = {item.vector, item.rotation, item.vector};
            for (usize name = 0; name < sizeof(names) / sizeof(names[0]); ++name) {
                usize found = IndexNone;
                for (usize index = 0; index < fields.Size(); ++index) {
                    if (StringView(fields[index].name.Data(), fields[index].name.Size()) ==
                        names[name]) {
                        found = index;
                    }
                }

                REQUIRE(found != IndexNone);
                const FieldSchema& field = fields[found];
                CHECK(view.FieldIndex(field.id) == found);
                CHECK(field.kind == FieldKind::Value);
                CHECK(field.value.kind == ValueKind::Struct);
                CHECK(field.value.type == targets[name]);
                CHECK(field.flags == 0);
                CHECK(view.FieldStruct(found).IsAtomic());
            }
        }

        REQUIRE(registration.Unregister());
    }

    CHECK(types.Shutdown());
}
