#include <ReflectionTestFixtures/ReflectionFixture.generated.h>
#include <Core/Memory/SystemAllocator.h>
#include <Object/GeneratedFieldCodec.h>
#include <Object/GeneratedPathAccess.h>
#include <Object/PropertyPath.h>
#include <Platform/Thread.h>
#include <TestSupport/Test.h>

// 경로 연산(B6a) 레지스트리 단위 시험이다(Docs/Design/ReflectionFoundations.ko.md 7.1절 "시험").
// 생성 코드(B6b) 전이라 GeneratedType 특수화를 MetaCompiler가 낼 모양으로 손으로 썼다: 클래스
// Struct·Array 필드 함수표의 path 칸에 PathClassField를 부르는 람다(valueNativeType은 없으면
// nullptr), struct 멤버는 StructFieldOps<&S::m>(id, &PathStructMember<&S::m, Target>), 단일 값
// 필드에는 path가 없다.
using namespace rocks;
using reflection_fixture::Bias;
using reflection_fixture::Channels;
using reflection_fixture::TextureGroup;

namespace path_fixture {

struct Node;

// 참조 없는 struct다(이름·값).
struct Inner {
    explicit Inner(Allocator& allocator) noexcept : name(allocator) {
    }

    String name;
    uint32 value = 0;
};

// 참조 없는 struct 원소다(allocator 생성자 — 기본 원소의 inner.name이 배열의 allocator를 쓴다). x는
// 경로 thunk가 없는 멤버다.
struct Point {
    explicit Point(Allocator& allocator) noexcept : inner(allocator) {
    }

    uint32 x = 0;
    Inner inner;
};

// 모든 단일 값 종류와 중첩 struct·배열 멤버를 가진 참조 없는 struct다.
struct Leaf {
    explicit Leaf(Allocator& allocator) noexcept
        : text(allocator), soft(allocator), inner(allocator), names(allocator), points(allocator) {
    }

    bool flag = false;
    uint8 u8 = 0;
    uint16 u16 = 0;
    uint32 u32 = 0;
    uint64 u64 = 0;
    int32 i32 = 0;
    int64 i64 = 0;
    float f32 = 0;
    double f64 = 0;
    Guid guid{};
    String text;
    AssetId asset{};
    SoftObjectRef<Node> soft;
    Sha256Digest digest{};
    TextureGroup group = TextureGroup::World;
    Channels channels = Channels::None;
    Inner inner;
    Array<String> names;
    Array<Point> points;
};

// 참조 struct다(StructRefArray 원소).
struct Slot {
    explicit Slot(Allocator& allocator) noexcept : label(allocator) {
    }

    ObjectRef<Node> material;
    String label;
    uint32 index = 0;
};

// 인라인 참조 struct다(클래스 필드) — 참조 멤버와 Registry 컨테이너 둘.
struct Holder {
    explicit Holder(Allocator& allocator) noexcept : refs(allocator), slots(allocator) {
    }

    ObjectRef<Node> target;
    ObjectRefArray<Node> refs;
    StructRefArray<Slot> slots;
};

// 정적 대조 시험용이다: 등록 스키마가 wide를 UInt16 원소 배열로, group을 Bias enum으로 서술한다
// (수동 함수표의 정적 타입과 스키마의 어긋남).
struct Odd {
    explicit Odd(Allocator& allocator) noexcept : wide(allocator) {
    }

    Array<uint32> wide;
    TextureGroup group = TextureGroup::World;
};

struct Node final : Object {
    explicit Node(Allocator& allocator) noexcept
        : title(allocator), leaf(allocator), frozen(allocator), raw(allocator), numbers(allocator),
          labels(allocator), groups(allocator), points(allocator), softs(allocator),
          locked(allocator), refs(allocator), slots(allocator), holder(allocator), odd(allocator),
          wrong(allocator) {
    }

    void Trace(TraceVisitor& visitor) const noexcept;

    uint32 count = 0;
    String title;
    ObjectRef<Node> next;
    Leaf leaf;
    Leaf frozen;
    Leaf raw;
    Array<uint32> numbers;
    Array<String> labels;
    Array<TextureGroup> groups;
    Array<Point> points;
    Array<SoftObjectRef<Node>> softs;
    Array<uint32> locked;
    ObjectRefArray<Node> refs;
    StructRefArray<Slot> slots;
    Holder holder;
    Odd odd;
    Leaf wrong;
};

// 비정규 null 정규화 시험용 수동 함수표 클래스다(write callback이 받은 handle을 기록한다).
struct Manual final : Object {
    static inline ObjectHandle Received{1, 1, 1};
    static inline bool Reject = false;
};

constexpr TypeId NodeId{0x50544e, 1};
constexpr TypeId ManualId{0x50544e, 2};
constexpr TypeId InnerId{0x505453, 1};
constexpr TypeId PointId{0x505453, 2};
constexpr TypeId LeafId{0x505453, 3};
constexpr TypeId SlotId{0x505453, 4};
constexpr TypeId HolderId{0x505453, 5};
constexpr TypeId OddId{0x505453, 6};

constexpr Guid NameId{0x504d49, 1};
constexpr Guid ValueId{0x504d49, 2};
constexpr Guid XId{0x504d50, 1};
constexpr Guid PointInnerId{0x504d50, 2};
constexpr Guid FlagId{0x504d4c, 1};
constexpr Guid U8Id{0x504d4c, 2};
constexpr Guid U16Id{0x504d4c, 3};
constexpr Guid U32Id{0x504d4c, 4};
constexpr Guid U64Id{0x504d4c, 5};
constexpr Guid I32Id{0x504d4c, 6};
constexpr Guid I64Id{0x504d4c, 7};
constexpr Guid F32Id{0x504d4c, 8};
constexpr Guid F64Id{0x504d4c, 9};
constexpr Guid GuidId{0x504d4c, 10};
constexpr Guid TextId{0x504d4c, 11};
constexpr Guid AssetFieldId{0x504d4c, 12};
constexpr Guid SoftId{0x504d4c, 13};
constexpr Guid DigestId{0x504d4c, 14};
constexpr Guid GroupId{0x504d4c, 15};
constexpr Guid ChannelsId{0x504d4c, 16};
constexpr Guid LeafInnerId{0x504d4c, 17};
constexpr Guid NamesId{0x504d4c, 18};
constexpr Guid LeafPointsId{0x504d4c, 19};
constexpr Guid MaterialId{0x504d53, 1};
constexpr Guid LabelId{0x504d53, 2};
constexpr Guid IndexId{0x504d53, 3};
constexpr Guid TargetId{0x504d48, 1};
constexpr Guid HolderRefsId{0x504d48, 2};
constexpr Guid HolderSlotsId{0x504d48, 3};
constexpr Guid WideId{0x504d4f, 1};
constexpr Guid OddGroupId{0x504d4f, 2};
constexpr Guid CountId{0x50464e, 1};
constexpr Guid TitleId{0x50464e, 2};
constexpr Guid NextId{0x50464e, 3};
constexpr Guid LeafFieldId{0x50464e, 4};
constexpr Guid FrozenId{0x50464e, 5};
constexpr Guid RawId{0x50464e, 6};
constexpr Guid NumbersId{0x50464e, 7};
constexpr Guid LabelsId{0x50464e, 8};
constexpr Guid GroupsId{0x50464e, 9};
constexpr Guid PointsId{0x50464e, 10};
constexpr Guid SoftsId{0x50464e, 11};
constexpr Guid LockedId{0x50464e, 12};
constexpr Guid RefsId{0x50464e, 13};
constexpr Guid SlotsId{0x50464e, 14};
constexpr Guid HolderFieldId{0x50464e, 15};
constexpr Guid OddFieldId{0x50464e, 16};
constexpr Guid WrongId{0x50464e, 17};
constexpr Guid LinkId{0x50464d, 1};

constexpr uint32 ReadOnly = static_cast<uint32>(FieldFlags::ReadOnly);
constexpr uint32 Transient = static_cast<uint32>(FieldFlags::Transient);
constexpr uint32 EditorOnly = static_cast<uint32>(FieldFlags::EditorOnly);

Span<const char> Text(const char* text) noexcept {
    usize bytes = 0;
    while (text[bytes] != '\0') {
        ++bytes;
    }

    return Span<const char>(text, bytes);
}

FieldSchema Scalar(Guid id, const char* name, ValueKind kind, TypeId type = {},
                   uint32 flags = 0) noexcept {
    FieldSchema field;
    field.id = id;
    field.kind = kind == ValueKind::ObjectReference ? FieldKind::ObjectReference : FieldKind::Value;
    field.flags = flags;
    field.name = Text(name);
    field.value = ValueType{kind, type};
    return field;
}

FieldSchema ArrayOf(Guid id, const char* name, ValueKind element, TypeId type = {},
                    uint32 maxElements = DefaultMaxElements, uint32 flags = 0) noexcept {
    FieldSchema field;
    field.id = id;
    field.kind =
        element == ValueKind::ObjectReference ? FieldKind::ObjectReference : FieldKind::Value;
    field.flags = flags;
    field.name = Text(name);
    field.value = ValueType{ValueKind::Array, {}};
    field.element = ValueType{element, type};
    field.maxElements = maxElements;
    return field;
}

template <class S>
RuntimeStructOps MakeStructOps(Span<const RuntimeStructFieldOps> fields) noexcept {
    RuntimeStructOps ops = MakeRuntimeStructOps<S>();
    if (GeneratedType<S>::ContainsStrongReference) {
        ops.trace = [](const void* value, TraceVisitor& visitor) noexcept {
            GeneratedType<S>::Trace(*static_cast<const S*>(value), visitor);
        };
    }

    ops.fields = fields;
    return ops;
}

// 단일 값 클래스 필드의 함수표다(생성 코드와 같은 모양 — read·write·writeStaged·codec, path 없음).
template <auto Member>
RuntimeFieldOps ValueField(Guid id) noexcept {
    using O = typename detail::StructMemberPointer<decltype(Member)>::Owner;
    RuntimeFieldOps ops;
    ops.fieldId = id;
    ops.read = +[](ObjectRegistry& registry, const ObjectAccessContext& access, ObjectHandle object,
                   FieldValue& out) noexcept {
        return detail::ReadGeneratedField<O>(registry, access, object, Member, out);
    };
    ops.write = +[](ObjectRegistry& registry, const ObjectAccessContext& access,
                    ObjectHandle object, const FieldValue& value) noexcept {
        return detail::WriteGeneratedField<O>(registry, access, object, Member, value);
    };
    ops.writeStaged = +[](ConstructionContext& construction, ObjectHandle object,
                          const FieldValue& value) noexcept {
        return detail::WriteGeneratedStagedField<O>(construction, object, Member, value);
    };
    ops.encode =
        +[](const RuntimeFieldOps&, ObjectRegistry& registry, const ObjectAccessContext& access,
            ObjectHandle object, ArchiveWriter& out, FieldEncodeContext& context) noexcept {
            return detail::EncodeGeneratedField<O>(registry, access, object, Member, out, context);
        };
    ops.decode = +[](const RuntimeFieldOps&, ConstructionContext* construction, ObjectHandle object,
                     const FieldView& field, FieldDecodeContext& context) noexcept {
        return detail::DecodeGeneratedField<O>(construction, object, Member, field, context);
    };
    return ops;
}

// struct·배열 클래스 필드의 함수표다(B6b 생성 코드와 같은 모양 — read·write·writeStaged 없음,
// codec, valueNativeType(없으면 nullptr), path 칸의 PathClassField 람다). codec이 false면 codec을
// 비운다 (Transient 필드).
template <auto Member, ValueKind Target>
RuntimeFieldOps PathField(Guid id, const void* valueNative = nullptr, bool path = true,
                          bool codec = true) noexcept {
    using O = typename detail::StructMemberPointer<decltype(Member)>::Owner;
    RuntimeFieldOps ops;
    ops.fieldId = id;
    if (codec) {
        ops.encode = +[](const RuntimeFieldOps&, ObjectRegistry& registry,
                         const ObjectAccessContext& access, ObjectHandle object, ArchiveWriter& out,
                         FieldEncodeContext& context) noexcept {
            return detail::EncodeGeneratedField<O>(registry, access, object, Member, out, context);
        };
        ops.decode = +[](const RuntimeFieldOps&, ConstructionContext* construction,
                         ObjectHandle object, const FieldView& field,
                         FieldDecodeContext& context) noexcept {
            return detail::DecodeGeneratedField<O>(construction, object, Member, field, context);
        };
    }

    ops.valueNativeType = valueNative;
    if (path) {
        ops.path =
            +[](const RuntimeFieldOps&, detail::PathCursor& cursor) noexcept -> Result<void> {
            return detail::PathClassField<O, Target>(cursor, Member);
        };
    }

    return ops;
}

// struct 멤버 함수표(B6b 생성 코드와 같은 모양)다.
template <auto Member, ValueKind Target>
RuntimeStructFieldOps MemberOps(Guid id) noexcept {
    return detail::StructFieldOps<Member>(id, &detail::PathStructMember<Member, Target>);
}

} // namespace path_fixture

namespace rocks {

template <>
struct GeneratedType<path_fixture::Inner> {
    static constexpr bool ContainsStrongReference = false;
    static constexpr bool ContainsReferenceArray = false;

    static constexpr TypeId Id() noexcept {
        return path_fixture::InnerId;
    }

    static const TypeSchema& Schema() noexcept {
        using namespace path_fixture;
        static const FieldSchema fields[] = {Scalar(NameId, "name", ValueKind::Utf8String),
                                             Scalar(ValueId, "value", ValueKind::UInt32)};
        static const TypeSchema schema{Id(), 1, fields, {"Inner", 5}};
        return schema;
    }

    static void Trace(const path_fixture::Inner&, TraceVisitor&) noexcept {
    }

    static RuntimeStructOps Ops() noexcept {
        using namespace path_fixture;
        static const RuntimeStructFieldOps fields[] = {
            MemberOps<&Inner::name, ValueKind::Utf8String>(NameId),
            MemberOps<&Inner::value, ValueKind::UInt32>(ValueId)};
        return MakeStructOps<Inner>(fields);
    }
};

template <>
struct GeneratedType<path_fixture::Point> {
    static constexpr bool ContainsStrongReference = false;
    static constexpr bool ContainsReferenceArray = false;

    static constexpr TypeId Id() noexcept {
        return path_fixture::PointId;
    }

    static const TypeSchema& Schema() noexcept {
        using namespace path_fixture;
        static const FieldSchema fields[] = {
            Scalar(XId, "x", ValueKind::UInt32),
            Scalar(PointInnerId, "inner", ValueKind::Struct, InnerId)};
        static const TypeSchema schema{Id(), 1, fields, {"Point", 5}};
        return schema;
    }

    static void Trace(const path_fixture::Point&, TraceVisitor&) noexcept {
    }

    static RuntimeStructOps Ops() noexcept {
        using namespace path_fixture;
        // x는 경로 thunk 없이 등록한다(null path thunk 시험).
        static const RuntimeStructFieldOps fields[] = {
            detail::StructFieldOps<&Point::x>(XId),
            MemberOps<&Point::inner, ValueKind::Struct>(PointInnerId)};
        return MakeStructOps<Point>(fields);
    }
};

template <>
struct GeneratedType<path_fixture::Leaf> {
    static constexpr bool ContainsStrongReference = false;
    static constexpr bool ContainsReferenceArray = false;

    static constexpr TypeId Id() noexcept {
        return path_fixture::LeafId;
    }

    static const TypeSchema& Schema() noexcept {
        using namespace path_fixture;
        static const FieldSchema fields[] = {
            Scalar(FlagId, "flag", ValueKind::Bool), Scalar(U8Id, "u8", ValueKind::UInt8),
            Scalar(U16Id, "u16", ValueKind::UInt16), Scalar(U32Id, "u32", ValueKind::UInt32),
            Scalar(U64Id, "u64", ValueKind::UInt64), Scalar(I32Id, "i32", ValueKind::Int32),
            Scalar(I64Id, "i64", ValueKind::Int64), Scalar(F32Id, "f32", ValueKind::Float32),
            // EditorOnly·Transient는 경로 접근을 막지 않는다.
            Scalar(F64Id, "f64", ValueKind::Float64, {}, EditorOnly | Transient),
            Scalar(GuidId, "guid", ValueKind::Guid), Scalar(TextId, "text", ValueKind::Utf8String),
            Scalar(AssetFieldId, "asset", ValueKind::AssetId),
            Scalar(SoftId, "soft", ValueKind::SoftObjectReference, NodeId),
            Scalar(DigestId, "digest", ValueKind::Digest256),
            Scalar(GroupId, "group", ValueKind::Enum, TypeIdOf<TextureGroup>()),
            Scalar(ChannelsId, "channels", ValueKind::Enum, TypeIdOf<Channels>()),
            Scalar(LeafInnerId, "inner", ValueKind::Struct, InnerId),
            ArrayOf(NamesId, "names", ValueKind::Utf8String),
            ArrayOf(LeafPointsId, "points", ValueKind::Struct, PointId)};
        static const TypeSchema schema{Id(), 1, fields, {"Leaf", 4}};
        return schema;
    }

    static void Trace(const path_fixture::Leaf&, TraceVisitor&) noexcept {
    }

    static RuntimeStructOps Ops() noexcept {
        using namespace path_fixture;
        static const RuntimeStructFieldOps fields[] = {
            MemberOps<&Leaf::flag, ValueKind::Bool>(FlagId),
            MemberOps<&Leaf::u8, ValueKind::UInt8>(U8Id),
            MemberOps<&Leaf::u16, ValueKind::UInt16>(U16Id),
            MemberOps<&Leaf::u32, ValueKind::UInt32>(U32Id),
            MemberOps<&Leaf::u64, ValueKind::UInt64>(U64Id),
            MemberOps<&Leaf::i32, ValueKind::Int32>(I32Id),
            MemberOps<&Leaf::i64, ValueKind::Int64>(I64Id),
            MemberOps<&Leaf::f32, ValueKind::Float32>(F32Id),
            MemberOps<&Leaf::f64, ValueKind::Float64>(F64Id),
            MemberOps<&Leaf::guid, ValueKind::Guid>(GuidId),
            MemberOps<&Leaf::text, ValueKind::Utf8String>(TextId),
            MemberOps<&Leaf::asset, ValueKind::AssetId>(AssetFieldId),
            MemberOps<&Leaf::soft, ValueKind::SoftObjectReference>(SoftId),
            MemberOps<&Leaf::digest, ValueKind::Digest256>(DigestId),
            MemberOps<&Leaf::group, ValueKind::Enum>(GroupId),
            MemberOps<&Leaf::channels, ValueKind::Enum>(ChannelsId),
            MemberOps<&Leaf::inner, ValueKind::Struct>(LeafInnerId),
            MemberOps<&Leaf::names, ValueKind::Utf8String>(NamesId),
            MemberOps<&Leaf::points, ValueKind::Struct>(LeafPointsId)};
        return MakeStructOps<Leaf>(fields);
    }
};

template <>
struct GeneratedType<path_fixture::Slot> {
    static constexpr bool ContainsStrongReference = true;
    static constexpr bool ContainsReferenceArray = false;

    static constexpr TypeId Id() noexcept {
        return path_fixture::SlotId;
    }

    static const TypeSchema& Schema() noexcept {
        using namespace path_fixture;
        static const FieldSchema fields[] = {
            Scalar(MaterialId, "material", ValueKind::ObjectReference, NodeId),
            Scalar(LabelId, "label", ValueKind::Utf8String),
            Scalar(IndexId, "index", ValueKind::UInt32)};
        static const TypeSchema schema{Id(), 1, fields, {"Slot", 4}};
        return schema;
    }

    static void Trace(const path_fixture::Slot& value, TraceVisitor& visitor) noexcept {
        visitor.Visit(value.material);
    }

    static RuntimeStructOps Ops() noexcept {
        using namespace path_fixture;
        static const RuntimeStructFieldOps fields[] = {
            MemberOps<&Slot::material, ValueKind::ObjectReference>(MaterialId),
            MemberOps<&Slot::label, ValueKind::Utf8String>(LabelId),
            MemberOps<&Slot::index, ValueKind::UInt32>(IndexId)};
        return MakeStructOps<Slot>(fields);
    }
};

template <>
struct GeneratedType<path_fixture::Holder> {
    static constexpr bool ContainsStrongReference = true;
    static constexpr bool ContainsReferenceArray = true;

    static constexpr TypeId Id() noexcept {
        return path_fixture::HolderId;
    }

    static const TypeSchema& Schema() noexcept {
        using namespace path_fixture;
        static const FieldSchema fields[] = {
            Scalar(TargetId, "target", ValueKind::ObjectReference, NodeId),
            ArrayOf(HolderRefsId, "refs", ValueKind::ObjectReference, NodeId),
            ArrayOf(HolderSlotsId, "slots", ValueKind::Struct, SlotId)};
        static const TypeSchema schema{Id(), 1, fields, {"Holder", 6}};
        return schema;
    }

    static void Trace(const path_fixture::Holder& value, TraceVisitor& visitor) noexcept {
        visitor.Visit(value.target);
        detail::TraceArrayField(value.refs, visitor);
        detail::TraceArrayField(value.slots, visitor);
    }

    static RuntimeStructOps Ops() noexcept {
        using namespace path_fixture;
        static const RuntimeStructFieldOps fields[] = {
            MemberOps<&Holder::target, ValueKind::ObjectReference>(TargetId),
            MemberOps<&Holder::refs, ValueKind::ObjectReference>(HolderRefsId),
            MemberOps<&Holder::slots, ValueKind::Struct>(HolderSlotsId)};
        return MakeStructOps<Holder>(fields);
    }
};

template <>
struct GeneratedType<path_fixture::Odd> {
    static constexpr bool ContainsStrongReference = false;
    static constexpr bool ContainsReferenceArray = false;

    static constexpr TypeId Id() noexcept {
        return path_fixture::OddId;
    }

    static const TypeSchema& Schema() noexcept {
        using namespace path_fixture;
        static const FieldSchema fields[] = {
            ArrayOf(WideId, "wide", ValueKind::UInt16),
            Scalar(OddGroupId, "group", ValueKind::Enum, TypeIdOf<Bias>())};
        static const TypeSchema schema{Id(), 1, fields, {"Odd", 3}};
        return schema;
    }

    static void Trace(const path_fixture::Odd&, TraceVisitor&) noexcept {
    }

    static RuntimeStructOps Ops() noexcept {
        using namespace path_fixture;
        static RuntimeStructFieldOps fields[] = {
            MemberOps<&Odd::wide, ValueKind::UInt32>(WideId),
            MemberOps<&Odd::group, ValueKind::Enum>(OddGroupId)};
        // 수동 함수표가 등록 대조를 속인다(정적 타입은 TextureGroup) — 경로 thunk의 실행 시 대조가
        // 잡아야 한다.
        fields[1].valueNativeType = NativeTypeKey<Bias>();
        return MakeStructOps<Odd>(fields);
    }
};

template <>
struct GeneratedType<path_fixture::Node> {
    static constexpr TypeId Id() noexcept {
        return path_fixture::NodeId;
    }

    static const TypeSchema& Schema() noexcept {
        using namespace path_fixture;
        static const FieldSchema fields[] = {
            Scalar(CountId, "count", ValueKind::UInt32),
            Scalar(TitleId, "title", ValueKind::Utf8String),
            Scalar(NextId, "next", ValueKind::ObjectReference, NodeId),
            Scalar(LeafFieldId, "leaf", ValueKind::Struct, LeafId),
            Scalar(FrozenId, "frozen", ValueKind::Struct, LeafId, ReadOnly),
            Scalar(RawId, "raw", ValueKind::Struct, LeafId),
            ArrayOf(NumbersId, "numbers", ValueKind::UInt32, {}, 3),
            ArrayOf(LabelsId, "labels", ValueKind::Utf8String, {}, DefaultMaxElements, EditorOnly),
            ArrayOf(GroupsId, "groups", ValueKind::Enum, TypeIdOf<TextureGroup>()),
            ArrayOf(PointsId, "points", ValueKind::Struct, PointId),
            ArrayOf(SoftsId, "softs", ValueKind::SoftObjectReference, NodeId),
            ArrayOf(LockedId, "locked", ValueKind::UInt32, {}, DefaultMaxElements, ReadOnly),
            ArrayOf(RefsId, "refs", ValueKind::ObjectReference, NodeId),
            ArrayOf(SlotsId, "slots", ValueKind::Struct, SlotId),
            Scalar(HolderFieldId, "holder", ValueKind::Struct, HolderId),
            Scalar(OddFieldId, "odd", ValueKind::Struct, OddId),
            // 스키마는 Inner지만 C++ 멤버는 Leaf다(정적 대조 시험 — 저장하지 않게 Transient).
            Scalar(WrongId, "wrong", ValueKind::Struct, InnerId, Transient)};
        static const TypeSchema schema{Id(), 1, fields, {"Node", 4}};
        return schema;
    }

    static RuntimeTypeOps Ops() noexcept {
        using namespace path_fixture;
        static const RuntimeFieldOps fields[] = {
            ValueField<&Node::count>(CountId),
            ValueField<&Node::title>(TitleId),
            ValueField<&Node::next>(NextId),
            PathField<&Node::leaf, ValueKind::Struct>(LeafFieldId, NativeTypeKey<Leaf>()),
            PathField<&Node::frozen, ValueKind::Struct>(FrozenId, NativeTypeKey<Leaf>()),
            PathField<&Node::raw, ValueKind::Struct>(RawId, NativeTypeKey<Leaf>(), false),
            PathField<&Node::numbers, ValueKind::UInt32>(NumbersId),
            PathField<&Node::labels, ValueKind::Utf8String>(LabelsId),
            PathField<&Node::groups, ValueKind::Enum>(GroupsId, NativeTypeKey<TextureGroup>()),
            PathField<&Node::points, ValueKind::Struct>(PointsId, NativeTypeKey<Point>()),
            PathField<&Node::softs, ValueKind::SoftObjectReference>(SoftsId),
            PathField<&Node::locked, ValueKind::UInt32>(LockedId),
            PathField<&Node::refs, ValueKind::ObjectReference>(RefsId),
            PathField<&Node::slots, ValueKind::Struct>(SlotsId, NativeTypeKey<Slot>()),
            PathField<&Node::holder, ValueKind::Struct>(HolderFieldId, NativeTypeKey<Holder>()),
            PathField<&Node::odd, ValueKind::Struct>(OddFieldId, NativeTypeKey<Odd>()),
            PathField<&Node::wrong, ValueKind::Struct>(WrongId, NativeTypeKey<Inner>(), true,
                                                       false)};
        auto ops = MakeRuntimeTypeOps<Node>();
        ops.fields = fields;
        return ops;
    }
};

} // namespace rocks

void path_fixture::Node::Trace(TraceVisitor& visitor) const noexcept {
    visitor.Visit(next);
    detail::TraceArrayField(refs, visitor);
    detail::TraceArrayField(slots, visitor);
    GeneratedType<Holder>::Trace(holder, visitor);
}

namespace {

using namespace path_fixture;

template <class S>
bool RegisterStruct(TypeRegistry& types) noexcept {
    return static_cast<bool>(
        types.RegisterStruct(GeneratedType<S>::Schema(), GeneratedType<S>::Ops()));
}

bool RegisterAll(TypeRegistry& types) noexcept {
    return GeneratedType<TextureGroup>::Register(types) &&
           GeneratedType<Channels>::Register(types) && GeneratedType<Bias>::Register(types) &&
           GeneratedType<reflection_fixture::Base>::Register(types) &&
           RegisterStruct<Inner>(types) && RegisterStruct<Point>(types) &&
           RegisterStruct<Leaf>(types) && RegisterStruct<Slot>(types) &&
           RegisterStruct<Holder>(types) && RegisterStruct<Odd>(types) &&
           types.Register(GeneratedType<Node>::Schema(), GeneratedType<Node>::Ops());
}

ErrorCode ErrorOf(const Result<void>& result) noexcept {
    return result ? ErrorCode::None : result.Error();
}

template <usize N>
PropertyPath PathOf(const PathStep (&steps)[N]) noexcept {
    return PropertyPath{Span<const PathStep>(steps, N)};
}

PathStep At(Guid id, uint32 index) noexcept {
    return PathStep{id, index, true};
}

FieldValue Unsigned(Allocator& allocator, ValueKind kind, uint64 value) noexcept {
    FieldValue result(allocator);
    result.kind = kind;
    result.unsignedInteger = value;
    return result;
}

FieldValue Signed(Allocator& allocator, ValueKind kind, int64 value) noexcept {
    FieldValue result(allocator);
    result.kind = kind;
    result.signedInteger = value;
    return result;
}

FieldValue TextValue(Allocator& allocator, const char* text) noexcept {
    FieldValue result(allocator);
    result.kind = ValueKind::Utf8String;
    if (!result.string.TryAssign(text)) {
        platform::Fatal("fixture text allocation failed");
    }

    return result;
}

FieldValue EnumValue(Allocator& allocator, TypeId type, int64 value) noexcept {
    FieldValue result(allocator);
    result.kind = ValueKind::Enum;
    result.type = type;
    result.signedInteger = value;
    return result;
}

FieldValue Reference(Allocator& allocator, ObjectHandle handle) noexcept {
    FieldValue result(allocator);
    result.kind = ValueKind::ObjectReference;
    result.reference = handle;
    return result;
}

FieldValue Soft(Allocator& allocator, Guid asset, const char* path) noexcept {
    FieldValue result(allocator);
    result.kind = ValueKind::SoftObjectReference;
    result.guid = asset;
    if (!result.string.TryAssign(path)) {
        platform::Fatal("fixture soft path allocation failed");
    }

    return result;
}

bool IsAlive(ObjectRegistry& objects, ObjectHandle handle) noexcept {
    auto lifecycle = objects.InspectLifecycle(objects.OwnerAccess(), handle);
    return lifecycle && lifecycle.Value() == ObjectLifecycle::Alive;
}

Node* Get(Borrow<Node>& borrow) noexcept {
    auto pointer = borrow.Get();
    if (!pointer) {
        platform::Fatal("fixture borrow failed");
    }

    return pointer.Value();
}

// 한 시험의 Registry 묶음이다(종료 때 객체를 모두 회수하고 lease를 놓는다).
struct World {
    explicit World(Allocator& allocator) noexcept
        : types(allocator), objects(allocator, types, {0x5054, 1}) {
    }

    TypeRegistry types;
    ObjectRegistry objects;
};

} // namespace

TEST(PropertyPath_ReadWriteEveryKindWithPrevious) {
    SystemAllocator allocator;
    {
        World world(allocator);
        REQUIRE(RegisterAll(world.types));
        ObjectRegistry& objects = world.objects;
        const auto access = objects.OwnerAccess();
        auto node = objects.NewObject<Node>(access, NodeId);
        auto other = objects.NewObject<Node>(access, NodeId);
        REQUIRE(node && other);
        const ObjectHandle handle = node.Value().Handle();
        const TypeId groupType = TypeIdOf<TextureGroup>();
        const TypeId channelType = TypeIdOf<Channels>();

        // struct 멤버의 단일 값 종류마다 쓰고(옛 값) 읽는다.
        struct Case {
            Guid member;
            FieldValue value;
        };

        FieldValue digest(allocator);
        digest.kind = ValueKind::Digest256;
        digest.digest.bytes[0] = 0xab;
        digest.digest.bytes[31] = 0xcd;
        FieldValue boolean(allocator);
        boolean.kind = ValueKind::Bool;
        boolean.boolean = true;
        FieldValue f32(allocator);
        f32.kind = ValueKind::Float32;
        f32.float32 = 1.5f;
        FieldValue f64(allocator);
        f64.kind = ValueKind::Float64;
        f64.float64 = -2.25;
        FieldValue guid(allocator);
        guid.kind = ValueKind::Guid;
        guid.guid = Guid{7, 8};
        FieldValue asset(allocator);
        asset.kind = ValueKind::AssetId;
        asset.guid = Guid{9, 10};
        Case cases[] = {{FlagId, Move(boolean)},
                        {U8Id, Unsigned(allocator, ValueKind::UInt8, 200)},
                        {U16Id, Unsigned(allocator, ValueKind::UInt16, 60000)},
                        {U32Id, Unsigned(allocator, ValueKind::UInt32, 4000000000u)},
                        {U64Id, Unsigned(allocator, ValueKind::UInt64, 1ull << 40)},
                        {I32Id, Signed(allocator, ValueKind::Int32, -7)},
                        {I64Id, Signed(allocator, ValueKind::Int64, -(1ll << 40))},
                        {F32Id, Move(f32)},
                        {F64Id, Move(f64)},
                        {GuidId, Move(guid)},
                        {TextId, TextValue(allocator, "a fairly long text value for the leaf")},
                        {AssetFieldId, Move(asset)},
                        {SoftId, Soft(allocator, Guid{1, 2}, "Body.Mesh")},
                        {DigestId, Move(digest)},
                        {GroupId, EnumValue(allocator, groupType, 7)},
                        {ChannelsId, EnumValue(allocator, channelType, 5)}};
        for (Case& entry : cases) {
            const PathStep steps[] = {{LeafFieldId}, {entry.member}};
            FieldValue previous(allocator);
            PropertyPathFailure failure;
            REQUIRE(PathWrite(objects, access, handle, PathOf(steps), entry.value, &previous,
                              &failure));
            CHECK(failure.reason == PropertyPathError::None);
            CHECK(previous.kind == entry.value.kind);
            FieldValue read(allocator);
            REQUIRE(PathRead(objects, access, handle, PathOf(steps), read));
            CHECK(read.kind == entry.value.kind);
            CHECK(read.boolean == entry.value.boolean);
            CHECK(read.unsignedInteger == entry.value.unsignedInteger);
            CHECK(read.signedInteger == entry.value.signedInteger);
            CHECK(read.float32 == entry.value.float32 && read.float64 == entry.value.float64);
            CHECK(read.guid == entry.value.guid && read.digest == entry.value.digest);
            CHECK(read.type == entry.value.type);
            CHECK(read.string.View() == entry.value.string.View());
        }

        {
            // 옛 값: 두 번째 쓰기의 previous는 첫 값이다(String — 같은 allocator면 옮기고 다르면
            // previous의 allocator로 복사한다).
            SystemAllocator separate;
            const PathStep text[] = {{LeafFieldId}, {TextId}};
            FieldValue second = TextValue(allocator, "second");
            FieldValue sameAllocator(allocator);
            REQUIRE(PathWrite(objects, access, handle, PathOf(text), second, &sameAllocator));
            CHECK(sameAllocator.string.View() ==
                  StringView("a fairly long text value for the leaf"));
            FieldValue third = TextValue(allocator, "third");
            FieldValue otherAllocator(separate);
            REQUIRE(PathWrite(objects, access, handle, PathOf(text), third, &otherAllocator));
            CHECK(otherAllocator.string.View() == StringView("second"));
            CHECK(&otherAllocator.string.GetAllocator() == &separate);
            const PathStep soft[] = {{LeafFieldId}, {SoftId}};
            FieldValue nextSoft = Soft(allocator, Guid{3, 4}, "Other");
            FieldValue previousSoft(separate);
            REQUIRE(PathWrite(objects, access, handle, PathOf(soft), nextSoft, &previousSoft));
            CHECK(previousSoft.kind == ValueKind::SoftObjectReference);
            CHECK(previousSoft.guid == (Guid{1, 2}) &&
                  previousSoft.string.View() == StringView("Body.Mesh"));
            const PathStep numberSteps[] = {{LeafFieldId}, {U32Id}};
            FieldValue number = Unsigned(allocator, ValueKind::UInt32, 5);
            FieldValue previousNumber(separate);
            REQUIRE(
                PathWrite(objects, access, handle, PathOf(numberSteps), number, &previousNumber));
            CHECK(previousNumber.kind == ValueKind::UInt32 &&
                  previousNumber.unsignedInteger == 4000000000u);
        }

        {
            // 중첩 struct 멤버와 최상위 단일 값(등록된 read·write — previous는 write 직전 read).
            const PathStep innerName[] = {{LeafFieldId}, {LeafInnerId}, {NameId}};
            FieldValue name = TextValue(allocator, "inner");
            REQUIRE(PathWrite(objects, access, handle, PathOf(innerName), name));
            FieldValue read(allocator);
            REQUIRE(PathRead(objects, access, handle, PathOf(innerName), read));
            CHECK(read.string.View() == StringView("inner"));
            const PathStep count[] = {{CountId}};
            FieldValue value = Unsigned(allocator, ValueKind::UInt32, 11);
            FieldValue previous(allocator);
            REQUIRE(PathWrite(objects, access, handle, PathOf(count), value, &previous));
            CHECK(previous.kind == ValueKind::UInt32 && previous.unsignedInteger == 0);
            const PathStep title[] = {{TitleId}};
            FieldValue titleValue = TextValue(allocator, "title");
            REQUIRE(PathWrite(objects, access, handle, PathOf(title), titleValue, &previous));
            CHECK(previous.kind == ValueKind::Utf8String && previous.string.IsEmpty());
            REQUIRE(PathRead(objects, access, handle, PathOf(title), read));
            CHECK(read.string.View() == StringView("title"));
            const PathStep next[] = {{NextId}};
            FieldValue link = Reference(allocator, other.Value().Handle());
            REQUIRE(PathWrite(objects, access, handle, PathOf(next), link, &previous));
            CHECK(previous.kind == ValueKind::ObjectReference &&
                  previous.reference == ObjectHandle{});
            REQUIRE(PathRead(objects, access, handle, PathOf(next), read));
            CHECK(read.reference == other.Value().Handle());
        }

        {
            // 배열 원소의 단일 값: 삽입·교체(옛 값)·읽기·삭제(removed).
            const PathStep label0[] = {At(LabelsId, 0)};
            const PathStep label1[] = {At(LabelsId, 1)};
            FieldValue first = TextValue(allocator, "first label");
            FieldValue second = TextValue(allocator, "second label");
            REQUIRE(PathInsertElement(objects, access, handle, PathOf(label0), &first));
            REQUIRE(PathInsertElement(objects, access, handle, PathOf(label1), &second));
            FieldValue replacement = TextValue(allocator, "replaced");
            FieldValue previous(allocator);
            REQUIRE(PathWrite(objects, access, handle, PathOf(label0), replacement, &previous));
            CHECK(previous.string.View() == StringView("first label"));
            FieldValue removed(allocator);
            REQUIRE(PathRemoveElement(objects, access, handle, PathOf(label0), &removed));
            CHECK(removed.kind == ValueKind::Utf8String &&
                  removed.string.View() == StringView("replaced"));
            const PathStep labels[] = {{LabelsId}};
            auto length = PathArrayLength(objects, access, handle, PathOf(labels));
            REQUIRE(length);
            CHECK(length.Value() == 1);
            FieldValue read(allocator);
            REQUIRE(PathRead(objects, access, handle, PathOf(label0), read));
            CHECK(read.string.View() == StringView("second label"));
            SystemAllocator separate;
            FieldValue copied(separate);
            REQUIRE(PathRemoveElement(objects, access, handle, PathOf(label0), &copied));
            CHECK(copied.string.View() == StringView("second label") &&
                  &copied.string.GetAllocator() == &separate);

            const PathStep group0[] = {At(GroupsId, 0)};
            FieldValue group = EnumValue(allocator, groupType, 13);
            REQUIRE(PathInsertElement(objects, access, handle, PathOf(group0), &group));
            FieldValue removedGroup(allocator);
            REQUIRE(PathRemoveElement(objects, access, handle, PathOf(group0), &removedGroup));
            CHECK(removedGroup.kind == ValueKind::Enum && removedGroup.signedInteger == 13 &&
                  removedGroup.type == groupType);

            const PathStep soft0[] = {At(SoftsId, 0)};
            FieldValue soft = Soft(allocator, Guid{5, 6}, "Path.Value");
            REQUIRE(PathInsertElement(objects, access, handle, PathOf(soft0), &soft));
            REQUIRE(PathRead(objects, access, handle, PathOf(soft0), read));
            CHECK(read.kind == ValueKind::SoftObjectReference && read.guid == (Guid{5, 6}) &&
                  read.string.View() == StringView("Path.Value"));
            FieldValue softPrevious(separate);
            FieldValue nullSoft(allocator);
            nullSoft.kind = ValueKind::SoftObjectReference;
            REQUIRE(PathWrite(objects, access, handle, PathOf(soft0), nullSoft, &softPrevious));
            CHECK(softPrevious.guid == (Guid{5, 6}) &&
                  softPrevious.string.View() == StringView("Path.Value"));

            const PathStep number0[] = {At(NumbersId, 0)};
            FieldValue number = Unsigned(allocator, ValueKind::UInt32, 9);
            REQUIRE(PathInsertElement(objects, access, handle, PathOf(number0), &number));
            FieldValue removedNumber(allocator);
            REQUIRE(PathRemoveElement(objects, access, handle, PathOf(number0), &removedNumber));
            CHECK(removedNumber.unsignedInteger == 9);
        }

        node.Value().Reset();
        other.Value().Reset();
        REQUIRE(objects.Collect(access));
        CHECK(objects.Stats().liveObjects == 0);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(PropertyPath_AggregateAccessAndThunkErrors) {
    SystemAllocator allocator;
    {
        World world(allocator);
        REQUIRE(RegisterAll(world.types));
        ObjectRegistry& objects = world.objects;
        const auto access = objects.OwnerAccess();
        auto node = objects.NewObject<Node>(access, NodeId);
        REQUIRE(node);
        const ObjectHandle handle = node.Value().Handle();
        const PathStep point0[] = {At(PointsId, 0)};
        REQUIRE(PathInsertElement(objects, access, handle, PathOf(point0), nullptr));
        const PathStep slot0[] = {At(SlotsId, 0)};
        REQUIRE(PathInsertElement(objects, access, handle, PathOf(slot0), nullptr));

        // struct·배열을 통째로 읽거나 쓰면 Unsupported(AggregateValue)다.
        const PathStep leaf[] = {{LeafFieldId}};
        const PathStep numbers[] = {{NumbersId}};
        const PathStep inner[] = {{LeafFieldId}, {LeafInnerId}};
        const PathStep names[] = {{LeafFieldId}, {NamesId}};
        const PropertyPath aggregates[] = {PathOf(leaf),  PathOf(numbers), PathOf(point0),
                                           PathOf(slot0), PathOf(inner),   PathOf(names)};
        FieldValue value = Unsigned(allocator, ValueKind::UInt32, 1);
        for (const PropertyPath& path : aggregates) {
            PropertyPathFailure failure;
            FieldValue read(allocator);
            CHECK(ErrorOf(PathRead(objects, access, handle, path, read, &failure)) ==
                  ErrorCode::Unsupported);
            CHECK(failure.reason == PropertyPathError::AggregateValue &&
                  failure.step == path.steps.Size() - 1);
            CHECK(ErrorOf(PathWrite(objects, access, handle, path, value, nullptr, &failure)) ==
                  ErrorCode::Unsupported);
            CHECK(failure.reason == PropertyPathError::AggregateValue);
        }

        // struct 원소 삽입에 값을 주면 Unsupported, 단일 값 원소 삽입에 값이 없으면
        // InvalidArgument, struct 원소 제거에 removed를 주면 InvalidArgument다.
        PropertyPathFailure failure;
        CHECK(ErrorOf(PathInsertElement(objects, access, handle, PathOf(point0), &value,
                                        &failure)) == ErrorCode::Unsupported);
        CHECK(failure.reason == PropertyPathError::AggregateValue);
        const PathStep number0[] = {At(NumbersId, 0)};
        CHECK(ErrorOf(PathInsertElement(objects, access, handle, PathOf(number0), nullptr,
                                        &failure)) == ErrorCode::InvalidArgument);
        CHECK(failure.reason == PropertyPathError::InvalidValue);
        FieldValue removed(allocator);
        CHECK(ErrorOf(PathRemoveElement(objects, access, handle, PathOf(slot0), &removed,
                                        &failure)) == ErrorCode::InvalidArgument);
        CHECK(failure.reason == PropertyPathError::AggregateValue);

        // 경로 thunk가 없는 클래스 필드·struct 멤버는 Unsupported(NoPathAccess)다.
        const PathStep raw[] = {{RawId}, {U32Id}};
        CHECK(ErrorOf(PathWrite(objects, access, handle, PathOf(raw), value, nullptr, &failure)) ==
              ErrorCode::Unsupported);
        CHECK(failure.reason == PropertyPathError::NoPathAccess && failure.step == 0);
        const PathStep pointX[] = {At(PointsId, 0), {XId}};
        CHECK(ErrorOf(PathWrite(objects, access, handle, PathOf(pointX), value, nullptr,
                                &failure)) == ErrorCode::Unsupported);
        CHECK(failure.reason == PropertyPathError::NoPathAccess && failure.step == 1);
        FieldValue read(allocator);
        CHECK(ErrorOf(PathRead(objects, access, handle, PathOf(pointX), read, &failure)) ==
              ErrorCode::Unsupported);
        {
            auto borrowed = objects.Resolve<Node>(access, handle);
            REQUIRE(borrowed);
            CHECK(Get(borrowed.Value())->points[0].x == 0);
        }

        node.Value().Reset();
        REQUIRE(objects.Collect(access));
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(PropertyPath_ReadOnlyPropagatesToMembersAndElements) {
    SystemAllocator allocator;
    {
        World world(allocator);
        REQUIRE(RegisterAll(world.types));
        ObjectRegistry& objects = world.objects;
        const auto access = objects.OwnerAccess();
        auto node = objects.NewObject<Node>(access, NodeId);
        REQUIRE(node);
        const ObjectHandle handle = node.Value().Handle();
        {
            auto borrowed = objects.Resolve<Node>(access, handle);
            REQUIRE(borrowed);
            Node* pointer = Get(borrowed.Value());
            REQUIRE(pointer->locked.TryPushBack(4));
            REQUIRE(pointer->frozen.names.TryPushBack(String(allocator)));
            pointer->frozen.u32 = 3;
        }

        FieldValue value = Unsigned(allocator, ValueKind::UInt32, 1);
        FieldValue text = TextValue(allocator, "x");
        PropertyPathFailure failure;
        // ReadOnly struct 필드 → 멤버 쓰기, 멤버 배열의 삽입·삭제·비우기.
        const PathStep frozenU32[] = {{FrozenId}, {U32Id}};
        const PathStep frozenName0[] = {{FrozenId}, At(NamesId, 0)};
        const PathStep frozenNames[] = {{FrozenId}, {NamesId}};
        CHECK(ErrorOf(PathWrite(objects, access, handle, PathOf(frozenU32), value, nullptr,
                                &failure)) == ErrorCode::Unsupported);
        CHECK(failure.reason == PropertyPathError::ReadOnly && failure.step == 1);
        CHECK(ErrorOf(PathWrite(objects, access, handle, PathOf(frozenName0), text, nullptr,
                                &failure)) == ErrorCode::Unsupported);
        CHECK(failure.reason == PropertyPathError::ReadOnly);
        CHECK(ErrorOf(PathInsertElement(objects, access, handle, PathOf(frozenName0), &text,
                                        &failure)) == ErrorCode::Unsupported);
        CHECK(failure.reason == PropertyPathError::ReadOnly);
        CHECK(ErrorOf(PathRemoveElement(objects, access, handle, PathOf(frozenName0), nullptr,
                                        &failure)) == ErrorCode::Unsupported);
        CHECK(failure.reason == PropertyPathError::ReadOnly);
        CHECK(ErrorOf(PathClearArray(objects, access, handle, PathOf(frozenNames), &failure)) ==
              ErrorCode::Unsupported);
        CHECK(failure.reason == PropertyPathError::ReadOnly);
        // ReadOnly 배열 필드 → 원소 쓰기·삽입·삭제·비우기.
        const PathStep locked0[] = {At(LockedId, 0)};
        const PathStep locked[] = {{LockedId}};
        CHECK(ErrorOf(PathWrite(objects, access, handle, PathOf(locked0), value, nullptr,
                                &failure)) == ErrorCode::Unsupported);
        CHECK(failure.reason == PropertyPathError::ReadOnly);
        CHECK(ErrorOf(PathInsertElement(objects, access, handle, PathOf(locked0), &value,
                                        &failure)) == ErrorCode::Unsupported);
        CHECK(ErrorOf(PathRemoveElement(objects, access, handle, PathOf(locked0), nullptr,
                                        &failure)) == ErrorCode::Unsupported);
        CHECK(ErrorOf(PathClearArray(objects, access, handle, PathOf(locked), &failure)) ==
              ErrorCode::Unsupported);
        CHECK(failure.reason == PropertyPathError::ReadOnly);
        // 읽기는 막지 않는다. 값은 바뀌지 않았다.
        FieldValue read(allocator);
        REQUIRE(PathRead(objects, access, handle, PathOf(frozenU32), read));
        CHECK(read.unsignedInteger == 3);
        REQUIRE(PathRead(objects, access, handle, PathOf(locked0), read));
        CHECK(read.unsignedInteger == 4);
        auto length = PathArrayLength(objects, access, handle, PathOf(frozenNames));
        REQUIRE(length);
        CHECK(length.Value() == 1);
        node.Value().Reset();
        REQUIRE(objects.Collect(access));
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(PropertyPath_EnumSoftOverflowAndStaticTypeChecks) {
    SystemAllocator allocator;
    {
        World world(allocator);
        REQUIRE(RegisterAll(world.types));
        ObjectRegistry& objects = world.objects;
        const auto access = objects.OwnerAccess();
        auto node = objects.NewObject<Node>(access, NodeId);
        REQUIRE(node);
        const ObjectHandle handle = node.Value().Handle();
        PropertyPathFailure failure;
        // enum: 선언되지 않은 값은 InvalidArgument, enum ID가 다르면 TypeMismatch(값 오류).
        const PathStep group[] = {{LeafFieldId}, {GroupId}};
        FieldValue undeclared = EnumValue(allocator, TypeIdOf<TextureGroup>(), 5);
        CHECK(ErrorOf(PathWrite(objects, access, handle, PathOf(group), undeclared, nullptr,
                                &failure)) == ErrorCode::InvalidArgument);
        CHECK(failure.reason == PropertyPathError::InvalidValue && failure.step == 1);
        FieldValue wrongEnum = EnumValue(allocator, TypeIdOf<Bias>(), 0);
        CHECK(ErrorOf(PathWrite(objects, access, handle, PathOf(group), wrongEnum, nullptr,
                                &failure)) == ErrorCode::TypeMismatch);
        CHECK(failure.reason == PropertyPathError::InvalidValue);
        const PathStep channels[] = {{LeafFieldId}, {ChannelsId}};
        FieldValue bits = EnumValue(allocator, TypeIdOf<Channels>(), 16);
        CHECK(ErrorOf(PathWrite(objects, access, handle, PathOf(channels), bits, nullptr,
                                &failure)) == ErrorCode::InvalidArgument);
        // 값 종류가 다르면 TypeMismatch(InvalidValue).
        const PathStep u8[] = {{LeafFieldId}, {U8Id}};
        FieldValue wrongKind = Unsigned(allocator, ValueKind::UInt16, 1);
        CHECK(ErrorOf(PathWrite(objects, access, handle, PathOf(u8), wrongKind, nullptr,
                                &failure)) == ErrorCode::TypeMismatch);
        CHECK(failure.reason == PropertyPathError::InvalidValue);
        // 정수 폭은 Overflow(사유 없음).
        FieldValue wide = Unsigned(allocator, ValueKind::UInt8, 300);
        CHECK(ErrorOf(PathWrite(objects, access, handle, PathOf(u8), wide, nullptr, &failure)) ==
              ErrorCode::Overflow);
        CHECK(failure.reason == PropertyPathError::None && failure.step == 1);
        const PathStep i32[] = {{LeafFieldId}, {I32Id}};
        FieldValue big = Signed(allocator, ValueKind::Int32, 1ll << 40);
        CHECK(ErrorOf(PathWrite(objects, access, handle, PathOf(i32), big, nullptr, &failure)) ==
              ErrorCode::Overflow);
        // Soft: asset 없이 경로만 있으면·경로가 상한을 넘으면 InvalidArgument.
        const PathStep soft[] = {{LeafFieldId}, {SoftId}};
        FieldValue pathOnly = Soft(allocator, Guid{}, "Orphan");
        CHECK(ErrorOf(PathWrite(objects, access, handle, PathOf(soft), pathOnly, nullptr,
                                &failure)) == ErrorCode::InvalidArgument);
        CHECK(failure.reason == PropertyPathError::InvalidValue);
        FieldValue longPath = Soft(allocator, Guid{1, 1}, "");
        for (usize index = 0; index <= SoftObjectRef<>::MaxObjectPathBytes; ++index) {
            REQUIRE(longPath.string.TryAppend("a"));
        }

        CHECK(ErrorOf(PathWrite(objects, access, handle, PathOf(soft), longPath, nullptr,
                                &failure)) == ErrorCode::InvalidArgument);
        const PathStep soft0[] = {At(SoftsId, 0)};
        CHECK(ErrorOf(PathInsertElement(objects, access, handle, PathOf(soft0), &pathOnly,
                                        &failure)) == ErrorCode::InvalidArgument);
        // 정적 대조: 생성 thunk의 정적 타입이 등록 스키마와 다르면
        // TypeMismatch(StaticTypeMismatch).
        const PathStep wideElement[] = {{OddFieldId}, At(WideId, 0)};
        FieldValue shortValue = Unsigned(allocator, ValueKind::UInt16, 1);
        CHECK(ErrorOf(PathInsertElement(objects, access, handle, PathOf(wideElement), &shortValue,
                                        &failure)) == ErrorCode::TypeMismatch);
        CHECK(failure.reason == PropertyPathError::StaticTypeMismatch && failure.step == 1);
        const PathStep oddGroup[] = {{OddFieldId}, {OddGroupId}};
        FieldValue bias = EnumValue(allocator, TypeIdOf<Bias>(), 0);
        CHECK(ErrorOf(PathWrite(objects, access, handle, PathOf(oddGroup), bias, nullptr,
                                &failure)) == ErrorCode::TypeMismatch);
        CHECK(failure.reason == PropertyPathError::StaticTypeMismatch);
        FieldValue read(allocator);
        CHECK(ErrorOf(PathRead(objects, access, handle, PathOf(oddGroup), read, &failure)) ==
              ErrorCode::TypeMismatch);
        const PathStep wrongName[] = {{WrongId}, {NameId}};
        FieldValue name = TextValue(allocator, "n");
        CHECK(ErrorOf(PathWrite(objects, access, handle, PathOf(wrongName), name, nullptr,
                                &failure)) == ErrorCode::TypeMismatch);
        CHECK(failure.reason == PropertyPathError::StaticTypeMismatch && failure.step == 0);
        {
            // 어느 실패도 값을 바꾸지 않았다.
            auto borrowed = objects.Resolve<Node>(access, handle);
            REQUIRE(borrowed);
            const Node* pointer = Get(borrowed.Value());
            CHECK(pointer->leaf.group == TextureGroup::World && pointer->leaf.u8 == 0 &&
                  pointer->leaf.i32 == 0 && pointer->leaf.soft.IsNull());
            CHECK(pointer->softs.IsEmpty() && pointer->odd.wide.IsEmpty());
            CHECK(pointer->wrong.inner.name.IsEmpty());
        }

        node.Value().Reset();
        REQUIRE(objects.Collect(access));
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(PropertyPath_IndexShapeAndInputErrors) {
    SystemAllocator allocator;
    {
        World world(allocator);
        REQUIRE(RegisterAll(world.types));
        ObjectRegistry& objects = world.objects;
        const auto access = objects.OwnerAccess();
        auto node = objects.NewObject<Node>(access, NodeId);
        REQUIRE(node);
        const ObjectHandle handle = node.Value().Handle();
        FieldValue value = Unsigned(allocator, ValueKind::UInt32, 1);
        PropertyPathFailure failure;
        // MaxElements 경계: 상한(3)까지 성공, 넘으면 CapacityExceeded(TooManyElements).
        for (uint32 index = 0; index < 3; ++index) {
            const PathStep at[] = {At(NumbersId, index)};
            value.unsignedInteger = index + 10;
            REQUIRE(PathInsertElement(objects, access, handle, PathOf(at), &value));
        }

        const PathStep end[] = {At(NumbersId, 3)};
        CHECK(ErrorOf(PathInsertElement(objects, access, handle, PathOf(end), &value, &failure)) ==
              ErrorCode::CapacityExceeded);
        CHECK(failure.reason == PropertyPathError::TooManyElements && failure.step == 0);
        // index: 읽기·쓰기·삭제는 ≥ 길이, 삽입은 > 길이가 IndexOutOfRange.
        const PathStep past[] = {At(NumbersId, 3)};
        FieldValue read(allocator);
        CHECK(ErrorOf(PathRead(objects, access, handle, PathOf(past), read, &failure)) ==
              ErrorCode::InvalidArgument);
        CHECK(failure.reason == PropertyPathError::IndexOutOfRange);
        CHECK(ErrorOf(PathWrite(objects, access, handle, PathOf(past), value, nullptr, &failure)) ==
              ErrorCode::InvalidArgument);
        CHECK(ErrorOf(PathRemoveElement(objects, access, handle, PathOf(past), nullptr,
                                        &failure)) == ErrorCode::InvalidArgument);
        CHECK(failure.reason == PropertyPathError::IndexOutOfRange);
        const PathStep label5[] = {At(LabelsId, 1)};
        FieldValue text = TextValue(allocator, "x");
        CHECK(ErrorOf(PathInsertElement(objects, access, handle, PathOf(label5), &text,
                                        &failure)) == ErrorCode::InvalidArgument);
        CHECK(failure.reason == PropertyPathError::IndexOutOfRange);
        const PathStep pointMember[] = {At(PointsId, 0), {PointInnerId}, {NameId}};
        CHECK(ErrorOf(PathWrite(objects, access, handle, PathOf(pointMember), text, nullptr,
                                &failure)) == ErrorCode::InvalidArgument);
        CHECK(failure.reason == PropertyPathError::IndexOutOfRange && failure.step == 0);
        // 모양: 배열이 아닌 필드의 index(NotArray), struct가 아닌 값 뒤의 단계(NotStruct — index
        // 없는 배열 포함), 길이·비우기의 배열 아닌 필드(NotArray).
        const PathStep indexedCount[] = {At(CountId, 0)};
        CHECK(ErrorOf(PathRead(objects, access, handle, PathOf(indexedCount), read, &failure)) ==
              ErrorCode::TypeMismatch);
        CHECK(failure.reason == PropertyPathError::NotArray && failure.step == 0);
        const PathStep afterScalar[] = {{CountId}, {U32Id}};
        CHECK(ErrorOf(PathRead(objects, access, handle, PathOf(afterScalar), read, &failure)) ==
              ErrorCode::TypeMismatch);
        CHECK(failure.reason == PropertyPathError::NotStruct && failure.step == 0);
        const PathStep afterArray[] = {{PointsId}, {XId}};
        CHECK(ErrorOf(PathRead(objects, access, handle, PathOf(afterArray), read, &failure)) ==
              ErrorCode::TypeMismatch);
        CHECK(failure.reason == PropertyPathError::NotStruct);
        const PathStep afterElement[] = {At(NumbersId, 0), {U32Id}};
        CHECK(ErrorOf(PathRead(objects, access, handle, PathOf(afterElement), read, &failure)) ==
              ErrorCode::TypeMismatch);
        CHECK(failure.reason == PropertyPathError::NotStruct);
        const PathStep count[] = {{CountId}};
        CHECK(PathArrayLength(objects, access, handle, PathOf(count), &failure).Error() ==
              ErrorCode::TypeMismatch);
        CHECK(failure.reason == PropertyPathError::NotArray);
        CHECK(ErrorOf(PathClearArray(objects, access, handle, PathOf(count), &failure)) ==
              ErrorCode::TypeMismatch);
        // 입력 형식: index 유무(IndexRequired·IndexNotAllowed), 정규형, 단계 수, Opaque 값.
        const PathStep numbers[] = {{NumbersId}};
        CHECK(ErrorOf(PathInsertElement(objects, access, handle, PathOf(numbers), &value,
                                        &failure)) == ErrorCode::InvalidArgument);
        CHECK(failure.reason == PropertyPathError::IndexRequired);
        CHECK(ErrorOf(PathRemoveElement(objects, access, handle, PathOf(numbers), nullptr,
                                        &failure)) == ErrorCode::InvalidArgument);
        CHECK(failure.reason == PropertyPathError::IndexRequired);
        const PathStep number0[] = {At(NumbersId, 0)};
        CHECK(PathArrayLength(objects, access, handle, PathOf(number0), &failure).Error() ==
              ErrorCode::InvalidArgument);
        CHECK(failure.reason == PropertyPathError::IndexNotAllowed);
        CHECK(ErrorOf(PathClearArray(objects, access, handle, PathOf(number0), &failure)) ==
              ErrorCode::InvalidArgument);
        CHECK(failure.reason == PropertyPathError::IndexNotAllowed);
        const PathStep nonCanonical[] = {{LeafFieldId}, {U32Id, 3, false}};
        CHECK(ErrorOf(PathRead(objects, access, handle, PathOf(nonCanonical), read, &failure)) ==
              ErrorCode::InvalidArgument);
        CHECK(failure.reason == PropertyPathError::None && failure.step == 1);
        CHECK(ErrorOf(PathRead(objects, access, handle, PropertyPath{}, read, &failure)) ==
              ErrorCode::InvalidArgument);
        PathStep deep[MaxPropertyPathSteps + 1];
        for (PathStep& step : deep) {
            step = PathStep{LeafFieldId};
        }

        CHECK(ErrorOf(PathRead(objects, access, handle, PathOf(deep), read, &failure)) ==
              ErrorCode::InvalidArgument);
        CHECK(failure.reason == PropertyPathError::TooDeep);
        FieldValue opaque(allocator);
        const PathStep u32[] = {{LeafFieldId}, {U32Id}};
        CHECK(ErrorOf(PathWrite(objects, access, handle, PathOf(u32), opaque, nullptr, &failure)) ==
              ErrorCode::InvalidArgument);
        CHECK(failure.reason == PropertyPathError::InvalidValue);
        // 없는 필드 ID(스키마가 바뀐 옛 경로)는 NotFound(UnknownField)와 그 단계다.
        const PathStep unknown[] = {{LeafFieldId}, {Guid{0x999, 1}}};
        CHECK(ErrorOf(PathRead(objects, access, handle, PathOf(unknown), read, &failure)) ==
              ErrorCode::NotFound);
        CHECK(failure.reason == PropertyPathError::UnknownField && failure.step == 1);
        // 성공하면 failure를 비운다.
        REQUIRE(PathRead(objects, access, handle, PathOf(number0), read, &failure));
        CHECK(failure.reason == PropertyPathError::None && failure.step == 0 &&
              read.unsignedInteger == 10);
        // DestroyRequested owner는 Destroyed다(정리 중 변경은 Registry API를 직접 쓴다).
        REQUIRE(objects.RequestDestroy(access, handle));
        CHECK(ErrorOf(PathClearArray(objects, access, handle, PathOf(numbers), &failure)) ==
              ErrorCode::Destroyed);
        CHECK(failure.reason == PropertyPathError::None);
        node.Value().Reset();
        REQUIRE(objects.Collect(access));
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(PropertyPath_ObjectRefArrayElements) {
    SystemAllocator allocator;
    {
        World world(allocator);
        REQUIRE(RegisterAll(world.types));
        ObjectRegistry& objects = world.objects;
        const auto access = objects.OwnerAccess();
        auto node = objects.NewObject<Node>(access, NodeId);
        auto first = objects.NewObject<Node>(access, NodeId);
        auto second = objects.NewObject<Node>(access, NodeId);
        auto base = objects.NewObject<reflection_fixture::Base>(
            access, GeneratedType<reflection_fixture::Base>::Id());
        REQUIRE(node && first && second && base);
        const ObjectHandle handle = node.Value().Handle();
        const PathStep ref0[] = {At(RefsId, 0)};
        const PathStep ref1[] = {At(RefsId, 1)};
        const PathStep refs[] = {{RefsId}};
        PropertyPathFailure failure;
        // null·비정규 null(generation 0)은 ObjectHandle{}로 정규화한다.
        FieldValue null = Reference(allocator, {});
        FieldValue nonCanonical = Reference(allocator, {0, 7, 1});
        REQUIRE(PathInsertElement(objects, access, handle, PathOf(ref0), &null));
        REQUIRE(PathInsertElement(objects, access, handle, PathOf(ref1), &nonCanonical));
        FieldValue read(allocator);
        REQUIRE(PathRead(objects, access, handle, PathOf(ref1), read));
        CHECK(read.reference == ObjectHandle{});
        FieldValue target = Reference(allocator, first.Value().Handle());
        FieldValue previous(allocator);
        REQUIRE(PathWrite(objects, access, handle, PathOf(ref0), target, &previous));
        CHECK(previous.kind == ValueKind::ObjectReference && previous.reference == ObjectHandle{});
        REQUIRE(PathRead(objects, access, handle, PathOf(ref0), read));
        CHECK(read.reference == first.Value().Handle());
        // 타입이 다르면 TypeMismatch, 무효 handle은 StaleHandle(참조 lease 단계).
        FieldValue wrongType = Reference(allocator, base.Value().Handle());
        CHECK(ErrorOf(PathWrite(objects, access, handle, PathOf(ref0), wrongType, nullptr,
                                &failure)) == ErrorCode::TypeMismatch);
        CHECK(failure.reason == PropertyPathError::InvalidValue);
        ObjectHandle stale = second.Value().Handle();
        stale.generation += 1;
        FieldValue staleValue = Reference(allocator, stale);
        CHECK(ErrorOf(PathInsertElement(objects, access, handle, PathOf(ref0), &staleValue,
                                        &failure)) == ErrorCode::StaleHandle);
        CHECK(failure.reason == PropertyPathError::None);
        // non-canonical null로 교체하면 null이 된다(옛 값은 first).
        REQUIRE(PathWrite(objects, access, handle, PathOf(ref0), nonCanonical, &previous));
        CHECK(previous.reference == first.Value().Handle());
        REQUIRE(PathWrite(objects, access, handle, PathOf(ref0), target));
        FieldValue removed(allocator);
        REQUIRE(PathRemoveElement(objects, access, handle, PathOf(ref0), &removed));
        CHECK(removed.reference == first.Value().Handle());
        auto length = PathArrayLength(objects, access, handle, PathOf(refs));
        REQUIRE(length);
        CHECK(length.Value() == 1);
        REQUIRE(PathClearArray(objects, access, handle, PathOf(refs)));
        length = PathArrayLength(objects, access, handle, PathOf(refs));
        REQUIRE(length);
        CHECK(length.Value() == 0);

        // 인라인 참조 struct 안의 ObjectRefArray·참조 멤버(SetReference)도 같다.
        const PathStep holderRef0[] = {{HolderFieldId}, At(HolderRefsId, 0)};
        FieldValue secondValue = Reference(allocator, second.Value().Handle());
        REQUIRE(PathInsertElement(objects, access, handle, PathOf(holderRef0), &secondValue));
        const PathStep holderTarget[] = {{HolderFieldId}, {TargetId}};
        REQUIRE(PathWrite(objects, access, handle, PathOf(holderTarget), target, &previous));
        CHECK(previous.reference == ObjectHandle{});
        REQUIRE(PathWrite(objects, access, handle, PathOf(holderTarget), nonCanonical, &previous));
        CHECK(previous.reference == first.Value().Handle());
        REQUIRE(PathRead(objects, access, handle, PathOf(holderTarget), read));
        CHECK(read.reference == ObjectHandle{});
        CHECK(ErrorOf(PathWrite(objects, access, handle, PathOf(holderTarget), wrongType, nullptr,
                                &failure)) == ErrorCode::TypeMismatch);
        {
            auto borrowed = objects.Resolve<Node>(access, handle);
            REQUIRE(borrowed);
            const Node* pointer = Get(borrowed.Value());
            CHECK(pointer->holder.refs.Size() == 1 &&
                  pointer->holder.refs[0] == second.Value().Handle());
            CHECK(pointer->holder.target.Handle() == ObjectHandle{});
        }

        // 원소로만 닿는 객체는 GC에서 살아남는다.
        second.Value().Reset();
        REQUIRE(objects.Collect(access));
        CHECK(IsAlive(objects, secondValue.reference));
        node.Value().Reset();
        first.Value().Reset();
        base.Value().Reset();
        REQUIRE(objects.Collect(access));
        CHECK(objects.Stats().liveObjects == 0);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(PropertyPath_ReferenceInsertShadesDuringMarking) {
    SystemAllocator allocator;
    {
        World world(allocator);
        REQUIRE(RegisterAll(world.types));
        ObjectRegistry& objects = world.objects;
        const auto access = objects.OwnerAccess();
        // owner가 slot 0이라 첫 slice에서 검게 스캔된다. 패딩 루트가 Marking을 이어 가게 한다.
        auto node = objects.NewObject<Node>(access, NodeId);
        auto padding = objects.NewObject<Node>(access, NodeId);
        auto paddingTwo = objects.NewObject<Node>(access, NodeId);
        REQUIRE(node && padding && paddingTwo);
        const ObjectHandle handle = node.Value().Handle();
        ObjectHandle inserted;
        ObjectHandle member;
        ObjectHandle garbage;
        {
            auto created = objects.NewObject<Node>(access, NodeId);
            auto createdMember = objects.NewObject<Node>(access, NodeId);
            auto createdGarbage = objects.NewObject<Node>(access, NodeId);
            REQUIRE(created && createdMember && createdGarbage);
            inserted = created.Value().Handle();
            member = createdMember.Value().Handle();
            garbage = createdGarbage.Value().Handle();
        }

        const PathStep slot0[] = {At(SlotsId, 0)};
        REQUIRE(PathInsertElement(objects, access, handle, PathOf(slot0), nullptr));
        REQUIRE(objects.BeginCollection(access));
        GcBudget budget;
        budget.maxObjects = 1;
        budget.maxNanoseconds = 0;
        REQUIRE(objects.StepCollection(access, budget));
        REQUIRE(objects.CollectionPhase() == GcPhase::Marking);
        // 이미 검게 스캔된 owner에 흰 대상을 넣는다(ObjectRefArray 삽입·StructRefArray 원소 참조).
        const PathStep ref0[] = {At(RefsId, 0)};
        FieldValue value = Reference(allocator, inserted);
        REQUIRE(PathInsertElement(objects, access, handle, PathOf(ref0), &value));
        const PathStep material[] = {At(SlotsId, 0), {MaterialId}};
        FieldValue memberValue = Reference(allocator, member);
        REQUIRE(PathWrite(objects, access, handle, PathOf(material), memberValue));
        for (uint32 guard = 0; guard < 4096 && objects.CollectionPhase() != GcPhase::Idle;
             ++guard) {
            REQUIRE(objects.StepCollection(access, budget));
        }

        CHECK(IsAlive(objects, inserted) && IsAlive(objects, member));
        CHECK(!IsAlive(objects, garbage));
        node.Value().Reset();
        padding.Value().Reset();
        paddingTwo.Value().Reset();
        REQUIRE(objects.Collect(access));
        CHECK(objects.Stats().liveObjects == 0);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(PropertyPath_StructRefArrayElements) {
    SystemAllocator allocator;
    {
        World world(allocator);
        REQUIRE(RegisterAll(world.types));
        ObjectRegistry& objects = world.objects;
        const auto access = objects.OwnerAccess();
        auto node = objects.NewObject<Node>(access, NodeId);
        auto material = objects.NewObject<Node>(access, NodeId);
        REQUIRE(node && material);
        const ObjectHandle handle = node.Value().Handle();
        const PathStep slot0[] = {At(SlotsId, 0)};
        const PathStep slot1[] = {At(SlotsId, 1)};
        const PathStep slots[] = {{SlotsId}};
        REQUIRE(PathInsertElement(objects, access, handle, PathOf(slot0), nullptr));
        REQUIRE(PathInsertElement(objects, access, handle, PathOf(slot1), nullptr));
        // 원소 멤버 단위 쓰기: String·정수·참조(원소 위치의 SetElementReference).
        const PathStep label[] = {At(SlotsId, 1), {LabelId}};
        const PathStep index[] = {At(SlotsId, 1), {IndexId}};
        const PathStep reference[] = {At(SlotsId, 1), {MaterialId}};
        FieldValue text = TextValue(allocator, "slot label");
        FieldValue number = Unsigned(allocator, ValueKind::UInt32, 42);
        FieldValue target = Reference(allocator, material.Value().Handle());
        REQUIRE(PathWrite(objects, access, handle, PathOf(label), text));
        REQUIRE(PathWrite(objects, access, handle, PathOf(index), number));
        FieldValue previous(allocator);
        REQUIRE(PathWrite(objects, access, handle, PathOf(reference), target, &previous));
        CHECK(previous.reference == ObjectHandle{});
        FieldValue read(allocator);
        REQUIRE(PathRead(objects, access, handle, PathOf(reference), read));
        CHECK(read.reference == material.Value().Handle());
        // 원소 앞에 기본 원소를 넣으면 기존 원소는 index 2가 된다(주소 고정).
        REQUIRE(PathInsertElement(objects, access, handle, PathOf(slot0), nullptr));
        const PathStep moved[] = {At(SlotsId, 2), {LabelId}};
        REQUIRE(PathRead(objects, access, handle, PathOf(moved), read));
        CHECK(read.string.View() == StringView("slot label"));
        REQUIRE(PathRemoveElement(objects, access, handle, PathOf(slot0)));
        auto length = PathArrayLength(objects, access, handle, PathOf(slots));
        REQUIRE(length);
        CHECK(length.Value() == 2);
        {
            // 위치 오버로드는 private(경로 구현만 friend)이다 — 같은 SetEdge 원소 검사를 공개
            // (배열, index) 오버로드로 확인한다: 원소 블록 밖 field·범위 밖 index는
            // InvalidArgument.
            auto borrowed = objects.Resolve<Node>(access, handle);
            REQUIRE(borrowed);
            Node* pointer = Get(borrowed.Value());
            CHECK(objects.SetElementReference(access, handle, pointer->slots, 1,
                                              pointer->slots[1].material, {}));
            CHECK(ErrorOf(objects.SetElementReference(access, handle, pointer->slots, 1,
                                                      pointer->slots[0].material, {})) ==
                  ErrorCode::InvalidArgument);
            CHECK(ErrorOf(objects.SetElementReference(access, handle, pointer->slots, 1,
                                                      pointer->next, {})) ==
                  ErrorCode::InvalidArgument);
            CHECK(ErrorOf(objects.SetElementReference(access, handle, pointer->slots, 5,
                                                      pointer->slots[1].material, {})) ==
                  ErrorCode::InvalidArgument);
            CHECK(pointer->slots[1].material.Handle() == ObjectHandle{});
        }

        // 인라인 struct 안 StructRefArray의 원소 멤버.
        const PathStep holderSlot0[] = {{HolderFieldId}, At(HolderSlotsId, 0)};
        const PathStep holderMaterial[] = {{HolderFieldId}, At(HolderSlotsId, 0), {MaterialId}};
        REQUIRE(PathInsertElement(objects, access, handle, PathOf(holderSlot0), nullptr));
        REQUIRE(PathWrite(objects, access, handle, PathOf(holderMaterial), target));
        REQUIRE(PathRead(objects, access, handle, PathOf(holderMaterial), read));
        CHECK(read.reference == material.Value().Handle());
        // 원소 참조로만 닿는 객체가 살아남는다.
        const ObjectHandle materialHandle = material.Value().Handle();
        material.Value().Reset();
        REQUIRE(objects.Collect(access));
        CHECK(IsAlive(objects, materialHandle));
        REQUIRE(PathClearArray(objects, access, handle, PathOf(slots)));
        const PathStep holderSlots[] = {{HolderFieldId}, {HolderSlotsId}};
        REQUIRE(PathClearArray(objects, access, handle, PathOf(holderSlots)));
        REQUIRE(objects.Collect(access));
        CHECK(!IsAlive(objects, materialHandle));
        node.Value().Reset();
        REQUIRE(objects.Collect(access));
        CHECK(objects.Stats().liveObjects == 0);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(PropertyPath_ArrayOfStructDefaultElementAndNestedMember) {
    SystemAllocator allocator;
    {
        World world(allocator);
        REQUIRE(RegisterAll(world.types));
        ObjectRegistry& objects = world.objects;
        const auto access = objects.OwnerAccess();
        auto node = objects.NewObject<Node>(access, NodeId);
        REQUIRE(node);
        const ObjectHandle handle = node.Value().Handle();
        // Array<S> 기본 원소(S(Allocator&) — 원소 String이 배열의 allocator를 쓴다)와
        // arr[i].inner.name.
        for (uint32 index = 0; index < 6; ++index) {
            const PathStep at[] = {At(PointsId, index)};
            REQUIRE(PathInsertElement(objects, access, handle, PathOf(at), nullptr));
            const PathStep name[] = {At(PointsId, index), {PointInnerId}, {NameId}};
            FieldValue text = TextValue(allocator, "point name");
            REQUIRE(text.string.TryAppend(index == 0 ? "0" : "n"));
            REQUIRE(PathWrite(objects, access, handle, PathOf(name), text));
        }

        const PathStep front[] = {At(PointsId, 0)};
        REQUIRE(PathInsertElement(objects, access, handle, PathOf(front), nullptr));
        const PathStep name1[] = {At(PointsId, 1), {PointInnerId}, {NameId}};
        FieldValue read(allocator);
        REQUIRE(PathRead(objects, access, handle, PathOf(name1), read));
        CHECK(read.string.View() == StringView("point name0"));
        {
            auto borrowed = objects.Resolve<Node>(access, handle);
            REQUIRE(borrowed);
            Node* pointer = Get(borrowed.Value());
            REQUIRE(pointer->points.Size() == 7);
            CHECK(&pointer->points[0].inner.name.GetAllocator() == &pointer->points.GetAllocator());
            CHECK(pointer->points[0].inner.name.IsEmpty());
        }

        // struct 멤버 배열의 struct 원소(leaf.points[0].inner.value).
        const PathStep leafPoint[] = {{LeafFieldId}, At(LeafPointsId, 0)};
        REQUIRE(PathInsertElement(objects, access, handle, PathOf(leafPoint), nullptr));
        const PathStep leafValue[] = {
            {LeafFieldId}, At(LeafPointsId, 0), {PointInnerId}, {ValueId}};
        FieldValue number = Unsigned(allocator, ValueKind::UInt32, 77);
        REQUIRE(PathWrite(objects, access, handle, PathOf(leafValue), number));
        REQUIRE(PathRead(objects, access, handle, PathOf(leafValue), read));
        CHECK(read.unsignedInteger == 77);
        // 제거·비우기(용량까지 반환).
        REQUIRE(PathRemoveElement(objects, access, handle, PathOf(front)));
        REQUIRE(PathRead(objects, access, handle, PathOf(front), read).Error() ==
                ErrorCode::Unsupported);
        const PathStep name0[] = {At(PointsId, 0), {PointInnerId}, {NameId}};
        REQUIRE(PathRead(objects, access, handle, PathOf(name0), read));
        CHECK(read.string.View() == StringView("point name0"));
        const PathStep points[] = {{PointsId}};
        REQUIRE(PathClearArray(objects, access, handle, PathOf(points)));
        {
            auto borrowed = objects.Resolve<Node>(access, handle);
            REQUIRE(borrowed);
            const Node* pointer = Get(borrowed.Value());
            CHECK(pointer->points.IsEmpty() && pointer->points.Capacity() == 0);
            CHECK(pointer->leaf.points.Size() == 1 && pointer->leaf.points[0].inner.value == 77);
        }

        node.Value().Reset();
        REQUIRE(objects.Collect(access));
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(PropertyPath_EncodePinMakesEveryWriteBusy) {
    SystemAllocator allocator;
    {
        World world(allocator);
        REQUIRE(RegisterAll(world.types));
        ObjectRegistry& objects = world.objects;
        const auto access = objects.OwnerAccess();
        auto node = objects.NewObject<Node>(access, NodeId);
        auto other = objects.NewObject<Node>(access, NodeId);
        REQUIRE(node && other);
        const ObjectHandle handle = node.Value().Handle();
        const PathStep number0[] = {At(NumbersId, 0)};
        const PathStep label0[] = {At(LabelsId, 0)};
        const PathStep ref0[] = {At(RefsId, 0)};
        const PathStep slot0[] = {At(SlotsId, 0)};
        const PathStep point0[] = {At(PointsId, 0)};
        FieldValue number = Unsigned(allocator, ValueKind::UInt32, 1);
        FieldValue text = TextValue(allocator, "label");
        FieldValue reference = Reference(allocator, other.Value().Handle());
        REQUIRE(PathInsertElement(objects, access, handle, PathOf(number0), &number));
        REQUIRE(PathInsertElement(objects, access, handle, PathOf(label0), &text));
        REQUIRE(PathInsertElement(objects, access, handle, PathOf(ref0), &reference));
        REQUIRE(PathInsertElement(objects, access, handle, PathOf(slot0), nullptr));
        REQUIRE(PathInsertElement(objects, access, handle, PathOf(point0), nullptr));
        auto pin = objects.PinForEncode(access, handle);
        REQUIRE(pin);
        const PathStep count[] = {{CountId}};
        const PathStep leafU32[] = {{LeafFieldId}, {U32Id}};
        const PathStep leafText[] = {{LeafFieldId}, {TextId}};
        const PathStep material[] = {At(SlotsId, 0), {MaterialId}};
        const PathStep target[] = {{HolderFieldId}, {TargetId}};
        const PathStep numbers[] = {{NumbersId}};
        const PathStep slots[] = {{SlotsId}};
        const PathStep refs[] = {{RefsId}};
        CHECK(ErrorOf(PathWrite(objects, access, handle, PathOf(count), number)) ==
              ErrorCode::Busy);
        CHECK(ErrorOf(PathWrite(objects, access, handle, PathOf(leafU32), number)) ==
              ErrorCode::Busy);
        CHECK(ErrorOf(PathWrite(objects, access, handle, PathOf(leafText), text)) ==
              ErrorCode::Busy);
        CHECK(ErrorOf(PathWrite(objects, access, handle, PathOf(label0), text)) == ErrorCode::Busy);
        CHECK(ErrorOf(PathWrite(objects, access, handle, PathOf(material), reference)) ==
              ErrorCode::Busy);
        CHECK(ErrorOf(PathWrite(objects, access, handle, PathOf(target), reference)) ==
              ErrorCode::Busy);
        CHECK(ErrorOf(PathWrite(objects, access, handle, PathOf(ref0), reference)) ==
              ErrorCode::Busy);
        CHECK(ErrorOf(PathInsertElement(objects, access, handle, PathOf(number0), &number)) ==
              ErrorCode::Busy);
        CHECK(ErrorOf(PathInsertElement(objects, access, handle, PathOf(point0), nullptr)) ==
              ErrorCode::Busy);
        CHECK(ErrorOf(PathInsertElement(objects, access, handle, PathOf(slot0), nullptr)) ==
              ErrorCode::Busy);
        CHECK(ErrorOf(PathRemoveElement(objects, access, handle, PathOf(label0))) ==
              ErrorCode::Busy);
        CHECK(ErrorOf(PathClearArray(objects, access, handle, PathOf(numbers))) == ErrorCode::Busy);
        CHECK(ErrorOf(PathClearArray(objects, access, handle, PathOf(slots))) == ErrorCode::Busy);
        CHECK(ErrorOf(PathClearArray(objects, access, handle, PathOf(refs))) == ErrorCode::Busy);
        // 읽기·길이는 고정과 무관하다.
        FieldValue read(allocator);
        CHECK(PathRead(objects, access, handle, PathOf(label0), read));
        CHECK(read.string.View() == StringView("label"));
        auto length = PathArrayLength(objects, access, handle, PathOf(numbers));
        CHECK(length && length.Value() == 1);
        pin.Value().Reset();
        CHECK(PathClearArray(objects, access, handle, PathOf(numbers)));
        node.Value().Reset();
        other.Value().Reset();
        REQUIRE(objects.Collect(access));
        CHECK(objects.Stats().liveObjects == 0);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(PropertyPath_RegistryIndexesAndPathRegistrationRule) {
    SystemAllocator allocator;
    {
        World world(allocator);
        REQUIRE(RegisterAll(world.types));
        auto lease = world.types.Acquire(NodeId);
        REQUIRE(lease);
        // 클래스 레코드의 필드 ID 정렬 색인과 스키마 위치 → 함수표 색인.
        const usize holder = lease.Value().FieldIndex(HolderFieldId);
        REQUIRE(holder != TypeLease::NoField);
        CHECK(lease.Value().Schema().fields[holder].id == HolderFieldId);
        const RuntimeFieldOps* ops = lease.Value().FieldOps(holder);
        REQUIRE(ops);
        CHECK(ops->fieldId == HolderFieldId && ops->path != nullptr);
        const RuntimeFieldOps* count = lease.Value().FieldOps(lease.Value().FieldIndex(CountId));
        REQUIRE(count);
        CHECK(count->path == nullptr && count->read != nullptr);
        CHECK(lease.Value().FieldIndex(Guid{0x777, 7}) == TypeLease::NoField);
        auto leaf = world.types.AcquireValueType(LeafId, TypeKind::Struct);
        REQUIRE(leaf);
        CHECK(leaf.Value().FieldIndex(TextId) == 10);
        lease.Value().Reset();
        leaf.Value().Reset();

        // 클래스 단일 값 필드의 path는 null이어야 한다(InvalidArgument).
        TypeSchema schema = GeneratedType<Node>::Schema();
        schema.id = TypeId{0x50544e, 9};
        RuntimeTypeOps bad = GeneratedType<Node>::Ops();
        RuntimeFieldOps fields[17];
        for (usize index = 0; index < 17; ++index) {
            fields[index] = bad.fields[index];
        }

        fields[0].path = fields[3].path;
        bad.fields = Span<const RuntimeFieldOps>(fields, 17);
        CHECK(world.types.Register(schema, Move(bad)).Error() == ErrorCode::InvalidArgument);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

namespace {

Result<void> ReadManual(ObjectRegistry&, const ObjectAccessContext&, ObjectHandle,
                        FieldValue& out) noexcept {
    out.kind = ValueKind::ObjectReference;
    out.reference = Manual::Received;
    return Result<void>::Success();
}

Result<void> WriteManual(ObjectRegistry&, const ObjectAccessContext&, ObjectHandle,
                         const FieldValue& value) noexcept {
    if (Manual::Reject) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    Manual::Received = value.reference;
    return Result<void>::Success();
}

Result<void> WriteManualStaged(ConstructionContext&, ObjectHandle, const FieldValue&) noexcept {
    return Result<void>::Success();
}

} // namespace

TEST(PropertyPath_NonCanonicalNullIsNormalizedForRegisteredWrites) {
    SystemAllocator allocator;
    {
        World world(allocator);
        REQUIRE(RegisterAll(world.types));
        const FieldSchema fields[] = {Scalar(LinkId, "link", ValueKind::ObjectReference, NodeId)};
        const TypeSchema schema{ManualId, 1, fields, {"Manual", 6}};
        RuntimeTypeOps ops = MakeRuntimeTypeOps<Manual>();
        const RuntimeFieldOps fieldOps[] = {
            {LinkId, &ReadManual, &WriteManual, &WriteManualStaged}};
        ops.fields = fieldOps;
        REQUIRE(world.types.Register(schema, Move(ops)));
        ObjectRegistry& objects = world.objects;
        const auto access = objects.OwnerAccess();
        auto manual = objects.NewObject<Manual>(access, ManualId);
        REQUIRE(manual);
        // 수동 write callback은 비정규 null 대신 정규 null 사본을 받는다(검증은 null로).
        FieldValue nonCanonical = Reference(allocator, {0, 7, 1});
        const PathStep link[] = {{LinkId}};
        REQUIRE(PathWrite(objects, access, manual.Value().Handle(), PathOf(link), nonCanonical));
        CHECK(Manual::Received == ObjectHandle{});
        // 수동 callback이 낸 Unsupported는 필드가 ReadOnly가 아니므로 사유 없음(None)이다.
        Manual::Reject = true;
        PropertyPathFailure failure;
        CHECK(ErrorOf(PathWrite(objects, access, manual.Value().Handle(), PathOf(link),
                                nonCanonical, nullptr, &failure)) == ErrorCode::Unsupported);
        CHECK(failure.reason == PropertyPathError::None);
        Manual::Reject = false;
        Manual::Received = ObjectHandle{1, 1, 1};
        auto lease = world.types.Acquire(ManualId);
        REQUIRE(lease);
        REQUIRE(lease.Value().Ops().fields[0].write(objects, access, manual.Value().Handle(),
                                                    nonCanonical));
        CHECK(Manual::Received == ObjectHandle{});
        // ValidateFieldWrite의 null 판정도 IsNull이다(대상 lease 없이 통과).
        detail::FieldWriteBinding binding;
        binding.value = ValueType{ValueKind::ObjectReference, NodeId};
        detail::FieldWriteTarget target;
        target.actual = &fields[0];
        CHECK(detail::ValidateFieldWrite(binding, target, nonCanonical));
        lease.Value().Reset();
        manual.Value().Reset();
        REQUIRE(objects.Collect(access));
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(PropertyPath_EditorOnlyAndTransientDoNotBlockPathAccess) {
    SystemAllocator allocator;
    {
        World world(allocator);
        REQUIRE(RegisterAll(world.types));
        ObjectRegistry& objects = world.objects;
        const auto access = objects.OwnerAccess();
        auto node = objects.NewObject<Node>(access, NodeId);
        REQUIRE(node);
        const ObjectHandle handle = node.Value().Handle();
        PropertyPathFailure failure;
        // EditorOnly·Transient struct 멤버와 EditorOnly 배열 필드는 읽기·쓰기·삽입·삭제 모두 된다.
        const PathStep f64[] = {{LeafFieldId}, {F64Id}};
        FieldValue value(allocator);
        value.kind = ValueKind::Float64;
        value.float64 = 6.5;
        REQUIRE(PathWrite(objects, access, handle, PathOf(f64), value, nullptr, &failure));
        FieldValue read(allocator);
        REQUIRE(PathRead(objects, access, handle, PathOf(f64), read));
        CHECK(read.float64 == 6.5 && failure.reason == PropertyPathError::None);
        const PathStep label0[] = {At(LabelsId, 0)};
        FieldValue text = TextValue(allocator, "editor only label");
        REQUIRE(PathInsertElement(objects, access, handle, PathOf(label0), &text, &failure));
        REQUIRE(PathWrite(objects, access, handle, PathOf(label0), text, nullptr, &failure));
        REQUIRE(PathRemoveElement(objects, access, handle, PathOf(label0), nullptr, &failure));
        CHECK(failure.reason == PropertyPathError::None);
        node.Value().Reset();
        REQUIRE(objects.Collect(access));
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(PropertyPath_ValidateArrayEditDirect) {
    const FieldSchema array = ArrayOf(NumbersId, "numbers", ValueKind::UInt32);
    const FieldSchema scalar = Scalar(CountId, "count", ValueKind::UInt32);
    detail::FieldWriteTarget target;
    // actual 없음·배열이 아닌 필드는 TypeMismatch, 알 수 없는 edit 값은 InvalidArgument.
    CHECK(detail::ValidateArrayEdit(target, detail::ArrayEdit::Insert).Error() ==
          ErrorCode::TypeMismatch);
    target.actual = &scalar;
    CHECK(detail::ValidateArrayEdit(target, detail::ArrayEdit::Clear).Error() ==
          ErrorCode::TypeMismatch);
    target.actual = &array;
    CHECK(detail::ValidateArrayEdit(target, static_cast<detail::ArrayEdit>(7)).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(detail::ValidateArrayEdit(target, detail::ArrayEdit::Insert));
    CHECK(detail::ValidateArrayEdit(target, detail::ArrayEdit::Remove));
    CHECK(detail::ValidateArrayEdit(target, detail::ArrayEdit::Clear));
    // 전파된 유효 플래그의 ReadOnly는 Unsupported, EditorOnly·Transient는 막지 않는다.
    target.effectiveFlags = ReadOnly;
    CHECK(detail::ValidateArrayEdit(target, detail::ArrayEdit::Remove).Error() ==
          ErrorCode::Unsupported);
    target.effectiveFlags = EditorOnly | Transient;
    CHECK(detail::ValidateArrayEdit(target, detail::ArrayEdit::Remove));
}

namespace {

// 다른 스레드에서 경로 연산(WrongThread)과 문자열 해석(스레드 무관)을 시도한다.
struct ThreadProbe {
    ObjectRegistry* objects = nullptr;
    const TypeLease* lease = nullptr;
    ObjectHandle node;
    Allocator* allocator = nullptr;
    ErrorCode read = ErrorCode::DependencyFailed;
    ErrorCode written = ErrorCode::DependencyFailed;
    ErrorCode parsed = ErrorCode::DependencyFailed;
    PropertyPathError reason = PropertyPathError::StaticTypeMismatch;
};

void RunPathFromOtherThread(void* raw) noexcept {
    auto& probe = *static_cast<ThreadProbe*>(raw);
    const auto access = probe.objects->OwnerAccess();
    const PathStep u32[] = {{LeafFieldId}, {U32Id}};
    FieldValue out(*probe.allocator);
    PropertyPathFailure failure;
    probe.read = ErrorOf(PathRead(*probe.objects, access, probe.node, PathOf(u32), out, &failure));
    probe.reason = failure.reason;
    FieldValue value = Unsigned(*probe.allocator, ValueKind::UInt32, 3);
    probe.written = ErrorOf(PathWrite(*probe.objects, access, probe.node, PathOf(u32), value));
    PropertyPathBuffer path;
    probe.parsed = ErrorOf(ParsePropertyPath(*probe.lease, "leaf.u32", path));
}

} // namespace

TEST(PropertyPath_OperationsAreOwnerThreadOnly) {
    SystemAllocator allocator;
    {
        World world(allocator);
        REQUIRE(RegisterAll(world.types));
        ObjectRegistry& objects = world.objects;
        const auto access = objects.OwnerAccess();
        auto node = objects.NewObject<Node>(access, NodeId);
        auto lease = world.types.Acquire(NodeId);
        REQUIRE(node && lease);
        ThreadProbe probe;
        probe.objects = &objects;
        probe.lease = &lease.Value();
        probe.node = node.Value().Handle();
        probe.allocator = &allocator;
        platform::Thread thread;
        REQUIRE(thread.Start(RunPathFromOtherThread, &probe) == platform::ThreadStatus::Success);
        REQUIRE(thread.WaitForCompletion(30000) == platform::WaitResult::Signaled);
        REQUIRE(thread.Join() == platform::ThreadStatus::Success);
        // 경로 연산은 소유 스레드 전용(WrongThread, 사유 없음), 문자열 해석은 어느 스레드나 된다.
        CHECK(probe.read == ErrorCode::WrongThread && probe.written == ErrorCode::WrongThread);
        CHECK(probe.reason == PropertyPathError::None && probe.parsed == ErrorCode::None);
        {
            auto borrowed = objects.Resolve<Node>(access, node.Value().Handle());
            REQUIRE(borrowed);
            CHECK(Get(borrowed.Value())->leaf.u32 == 0);
        }

        lease.Value().Reset();
        node.Value().Reset();
        REQUIRE(objects.Collect(access));
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}
