#include <Core/Memory/SystemAllocator.h>
#include <Object/GeneratedFieldCodec.h>
#include <Object/GeneratedPathAccess.h>
#include <Object/PropertyPath.h>
#include <ObjectSerialization/ObjectGraph.h>
#include <TestSupport/Test.h>

// struct 원소 제거의 Undo 재생 시험이다(Docs/Design/ReflectionFoundations.ko.md 7.1절 "옛 값
// 출력"): struct 원소 제거는 removed를 주지 않으므로 CU4가 연산 전 PathRead로 잎을 기록하고 "기본
// 원소 삽입 + 잎 쓰기"로 되살린다 — 되살린 객체를 저장한 byte가 제거 전과 같아야 한다(Array<S>와
// StructRefArray<S> 원소, 원소 안 배열·참조 멤버 포함). GeneratedType 특수화는 B6b 생성 코드와 같은
// 모양으로 손으로 썼다.
using namespace rocks;

namespace path_replay {

struct Shelf;

// 참조 없는 struct 원소다(원소 안 배열 멤버).
struct Tag {
    explicit Tag(Allocator& allocator) noexcept : name(allocator), values(allocator) {
    }

    uint32 code = 0;
    String name;
    Array<uint32> values;
};

// 참조 struct 원소다(StructRefArray).
struct Socket {
    explicit Socket(Allocator& allocator) noexcept : name(allocator) {
    }

    ObjectRef<Shelf> target;
    String name;
    uint32 slot = 0;
};

struct Shelf final : Object {
    explicit Shelf(Allocator& allocator) noexcept : tags(allocator), sockets(allocator) {
    }

    void Trace(TraceVisitor& visitor) const noexcept;

    Array<Tag> tags;
    StructRefArray<Socket> sockets;
};

constexpr TypeId ShelfId{0x505253, 0x10};
constexpr TypeId TagId{0x505253, 0x11};
constexpr TypeId SocketId{0x505253, 0x12};
constexpr Guid CodeId{0x50526d, 1};
constexpr Guid NameId{0x50526d, 2};
constexpr Guid ValuesId{0x50526d, 3};
constexpr Guid TargetId{0x50526d, 4};
constexpr Guid SocketNameId{0x50526d, 5};
constexpr Guid SlotId{0x50526d, 6};
constexpr Guid TagsId{0x505266, 1};
constexpr Guid SocketsId{0x505266, 2};

Span<const char> Text(const char* text) noexcept {
    usize bytes = 0;
    while (text[bytes] != '\0') {
        ++bytes;
    }

    return Span<const char>(text, bytes);
}

FieldSchema Scalar(Guid id, const char* name, ValueKind kind, TypeId type = {}) noexcept {
    FieldSchema field;
    field.id = id;
    field.kind = kind == ValueKind::ObjectReference ? FieldKind::ObjectReference : FieldKind::Value;
    field.name = Text(name);
    field.value = ValueType{kind, type};
    return field;
}

FieldSchema ArrayOf(Guid id, const char* name, ValueKind element, TypeId type = {}) noexcept {
    FieldSchema field;
    field.id = id;
    field.name = Text(name);
    field.value = ValueType{ValueKind::Array, {}};
    field.element = ValueType{element, type};
    field.maxElements = DefaultMaxElements;
    return field;
}

template <auto Member, ValueKind Target>
RuntimeStructFieldOps MemberOps(Guid id) noexcept {
    return detail::StructFieldOps<Member>(id, &detail::PathStructMember<Member, Target>);
}

template <auto Member, ValueKind Target>
RuntimeFieldOps PathField(Guid id, const void* valueNative) noexcept {
    using O = typename detail::StructMemberPointer<decltype(Member)>::Owner;
    RuntimeFieldOps ops;
    ops.fieldId = id;
    ops.encode =
        +[](const RuntimeFieldOps&, ObjectRegistry& registry, const ObjectAccessContext& access,
            ObjectHandle object, ArchiveWriter& out, FieldEncodeContext& context) noexcept {
            return detail::EncodeGeneratedField<O>(registry, access, object, Member, out, context);
        };
    ops.decode = +[](const RuntimeFieldOps&, ConstructionContext* construction, ObjectHandle object,
                     const FieldView& field, FieldDecodeContext& context) noexcept {
        return detail::DecodeGeneratedField<O>(construction, object, Member, field, context);
    };
    ops.valueNativeType = valueNative;
    ops.path = +[](const RuntimeFieldOps&, detail::PathCursor& cursor) noexcept -> Result<void> {
        return detail::PathClassField<O, Target>(cursor, Member);
    };
    return ops;
}

} // namespace path_replay

namespace rocks {

template <>
struct GeneratedType<path_replay::Tag> {
    static constexpr bool ContainsStrongReference = false;
    static constexpr bool ContainsReferenceArray = false;

    static constexpr TypeId Id() noexcept {
        return path_replay::TagId;
    }

    static const TypeSchema& Schema() noexcept {
        using namespace path_replay;
        static const FieldSchema fields[] = {Scalar(CodeId, "code", ValueKind::UInt32),
                                             Scalar(NameId, "name", ValueKind::Utf8String),
                                             ArrayOf(ValuesId, "values", ValueKind::UInt32)};
        static const TypeSchema schema{Id(), 1, fields, {"Tag", 3}};
        return schema;
    }

    static void Trace(const path_replay::Tag&, TraceVisitor&) noexcept {
    }

    static RuntimeStructOps Ops() noexcept {
        using namespace path_replay;
        static const RuntimeStructFieldOps fields[] = {
            MemberOps<&Tag::code, ValueKind::UInt32>(CodeId),
            MemberOps<&Tag::name, ValueKind::Utf8String>(NameId),
            MemberOps<&Tag::values, ValueKind::UInt32>(ValuesId)};
        RuntimeStructOps ops = MakeRuntimeStructOps<Tag>();
        ops.fields = fields;
        return ops;
    }
};

template <>
struct GeneratedType<path_replay::Socket> {
    static constexpr bool ContainsStrongReference = true;
    static constexpr bool ContainsReferenceArray = false;

    static constexpr TypeId Id() noexcept {
        return path_replay::SocketId;
    }

    static const TypeSchema& Schema() noexcept {
        using namespace path_replay;
        static const FieldSchema fields[] = {
            Scalar(TargetId, "target", ValueKind::ObjectReference, ShelfId),
            Scalar(SocketNameId, "name", ValueKind::Utf8String),
            Scalar(SlotId, "slot", ValueKind::UInt32)};
        static const TypeSchema schema{Id(), 1, fields, {"Socket", 6}};
        return schema;
    }

    static void Trace(const path_replay::Socket& value, TraceVisitor& visitor) noexcept {
        visitor.Visit(value.target);
    }

    static RuntimeStructOps Ops() noexcept {
        using namespace path_replay;
        static const RuntimeStructFieldOps fields[] = {
            MemberOps<&Socket::target, ValueKind::ObjectReference>(TargetId),
            MemberOps<&Socket::name, ValueKind::Utf8String>(SocketNameId),
            MemberOps<&Socket::slot, ValueKind::UInt32>(SlotId)};
        RuntimeStructOps ops = MakeRuntimeStructOps<Socket>();
        ops.trace = [](const void* value, TraceVisitor& visitor) noexcept {
            GeneratedType<path_replay::Socket>::Trace(
                *static_cast<const path_replay::Socket*>(value), visitor);
        };
        ops.fields = fields;
        return ops;
    }
};

template <>
struct GeneratedType<path_replay::Shelf> {
    static constexpr TypeId Id() noexcept {
        return path_replay::ShelfId;
    }

    static const TypeSchema& Schema() noexcept {
        using namespace path_replay;
        static const FieldSchema fields[] = {
            ArrayOf(TagsId, "tags", ValueKind::Struct, TagId),
            ArrayOf(SocketsId, "sockets", ValueKind::Struct, SocketId)};
        static const TypeSchema schema{Id(), 1, fields, {"Shelf", 5}};
        return schema;
    }

    static RuntimeTypeOps Ops() noexcept {
        using namespace path_replay;
        static const RuntimeFieldOps fields[] = {
            PathField<&Shelf::tags, ValueKind::Struct>(TagsId, NativeTypeKey<Tag>()),
            PathField<&Shelf::sockets, ValueKind::Struct>(SocketsId, NativeTypeKey<Socket>())};
        auto ops = MakeRuntimeTypeOps<Shelf>();
        ops.fields = fields;
        ops.completeFieldLayout = true;
        return ops;
    }
};

} // namespace rocks

void path_replay::Shelf::Trace(TraceVisitor& visitor) const noexcept {
    detail::TraceArrayField(tags, visitor);
    detail::TraceArrayField(sockets, visitor);
}

namespace {

using namespace path_replay;

PathStep At(Guid id, uint32 index) noexcept {
    return PathStep{id, index, true};
}

template <usize N>
PropertyPath PathOf(const PathStep (&steps)[N]) noexcept {
    return PropertyPath{Span<const PathStep>(steps, N)};
}

FieldValue Unsigned(Allocator& allocator, uint64 value) noexcept {
    FieldValue result(allocator);
    result.kind = ValueKind::UInt32;
    result.unsignedInteger = value;
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

bool SameBytes(const Array<uint8>& left, const Array<uint8>& right) noexcept {
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

TEST(PropertyPathReplay_StructElementRemovalReplaysToSameBytes) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(types.RegisterStruct(GeneratedType<Tag>::Schema(), GeneratedType<Tag>::Ops()));
        REQUIRE(
            types.RegisterStruct(GeneratedType<Socket>::Schema(), GeneratedType<Socket>::Ops()));
        REQUIRE(types.Register(GeneratedType<Shelf>::Schema(), GeneratedType<Shelf>::Ops()));
        ObjectRegistry objects(allocator, types, {0x5052, 9});
        const auto access = objects.OwnerAccess();
        auto shelf = objects.NewObject<Shelf>(access, ShelfId);
        auto other = objects.NewObject<Shelf>(access, ShelfId);
        REQUIRE(shelf && other);
        const ObjectHandle owner = shelf.Value().Handle();

        // 원소 셋: Tag(code·name·values)와 Socket(target·name·slot).
        for (uint32 index = 0; index < 3; ++index) {
            const PathStep tag[] = {At(TagsId, index)};
            const PathStep socket[] = {At(SocketsId, index)};
            REQUIRE(PathInsertElement(objects, access, owner, PathOf(tag), nullptr));
            REQUIRE(PathInsertElement(objects, access, owner, PathOf(socket), nullptr));
            const PathStep code[] = {At(TagsId, index), {CodeId}};
            const PathStep name[] = {At(TagsId, index), {NameId}};
            FieldValue codeValue = Unsigned(allocator, 100 + index);
            FieldValue nameValue = TextValue(allocator, index == 1 ? "middle tag name" : "tag");
            REQUIRE(PathWrite(objects, access, owner, PathOf(code), codeValue));
            REQUIRE(PathWrite(objects, access, owner, PathOf(name), nameValue));
            for (uint32 value = 0; value <= index; ++value) {
                const PathStep element[] = {At(TagsId, index), At(ValuesId, value)};
                FieldValue number = Unsigned(allocator, index * 10 + value);
                REQUIRE(PathInsertElement(objects, access, owner, PathOf(element), &number));
            }

            const PathStep target[] = {At(SocketsId, index), {TargetId}};
            const PathStep socketName[] = {At(SocketsId, index), {SocketNameId}};
            const PathStep slot[] = {At(SocketsId, index), {SlotId}};
            FieldValue reference(allocator);
            reference.kind = ValueKind::ObjectReference;
            reference.reference = index == 1 ? other.Value().Handle() : ObjectHandle{};
            FieldValue socketNameValue = TextValue(allocator, "socket name");
            FieldValue slotValue = Unsigned(allocator, 7 + index);
            REQUIRE(PathWrite(objects, access, owner, PathOf(target), reference));
            REQUIRE(PathWrite(objects, access, owner, PathOf(socketName), socketNameValue));
            REQUIRE(PathWrite(objects, access, owner, PathOf(slot), slotValue));
        }

        auto before = SaveObjectGraph(allocator, objects, access, owner);
        REQUIRE(before);

        // Array<S> 원소 tags[1]: 연산 전 잎을 PathRead로 기록 → 제거 → 기본 원소 삽입 + 잎 쓰기.
        {
            const PathStep code[] = {At(TagsId, 1), {CodeId}};
            const PathStep name[] = {At(TagsId, 1), {NameId}};
            const PathStep values[] = {At(TagsId, 1), {ValuesId}};
            FieldValue savedCode(allocator);
            FieldValue savedName(allocator);
            REQUIRE(PathRead(objects, access, owner, PathOf(code), savedCode));
            REQUIRE(PathRead(objects, access, owner, PathOf(name), savedName));
            auto count = PathArrayLength(objects, access, owner, PathOf(values));
            REQUIRE(count && count.Value() == 2);
            FieldValue savedValues[2] = {FieldValue(allocator), FieldValue(allocator)};
            for (uint32 index = 0; index < 2; ++index) {
                const PathStep element[] = {At(TagsId, 1), At(ValuesId, index)};
                REQUIRE(PathRead(objects, access, owner, PathOf(element), savedValues[index]));
            }

            const PathStep tag[] = {At(TagsId, 1)};
            REQUIRE(PathRemoveElement(objects, access, owner, PathOf(tag)));
            auto removed = SaveObjectGraph(allocator, objects, access, owner);
            REQUIRE(removed);
            CHECK(!SameBytes(removed.Value(), before.Value()));
            REQUIRE(PathInsertElement(objects, access, owner, PathOf(tag), nullptr));
            REQUIRE(PathWrite(objects, access, owner, PathOf(code), savedCode));
            REQUIRE(PathWrite(objects, access, owner, PathOf(name), savedName));
            for (uint32 index = 0; index < 2; ++index) {
                const PathStep element[] = {At(TagsId, 1), At(ValuesId, index)};
                REQUIRE(PathInsertElement(objects, access, owner, PathOf(element),
                                          &savedValues[index]));
            }

            auto replayed = SaveObjectGraph(allocator, objects, access, owner);
            REQUIRE(replayed);
            CHECK(SameBytes(replayed.Value(), before.Value()));
        }

        // StructRefArray 원소 sockets[1](참조 멤버 포함)도 같다.
        {
            const PathStep target[] = {At(SocketsId, 1), {TargetId}};
            const PathStep name[] = {At(SocketsId, 1), {SocketNameId}};
            const PathStep slot[] = {At(SocketsId, 1), {SlotId}};
            FieldValue savedTarget(allocator);
            FieldValue savedName(allocator);
            FieldValue savedSlot(allocator);
            REQUIRE(PathRead(objects, access, owner, PathOf(target), savedTarget));
            REQUIRE(PathRead(objects, access, owner, PathOf(name), savedName));
            REQUIRE(PathRead(objects, access, owner, PathOf(slot), savedSlot));
            // 옛 참조 handle은 약한 참조다 — CU4처럼 RootRef로 붙잡는다.
            auto keep = objects.TryPin<Object>(access, savedTarget.reference);
            REQUIRE(keep);
            const PathStep socket[] = {At(SocketsId, 1)};
            REQUIRE(PathRemoveElement(objects, access, owner, PathOf(socket)));
            REQUIRE(PathInsertElement(objects, access, owner, PathOf(socket), nullptr));
            REQUIRE(PathWrite(objects, access, owner, PathOf(target), savedTarget));
            REQUIRE(PathWrite(objects, access, owner, PathOf(name), savedName));
            REQUIRE(PathWrite(objects, access, owner, PathOf(slot), savedSlot));
            auto replayed = SaveObjectGraph(allocator, objects, access, owner);
            REQUIRE(replayed);
            CHECK(SameBytes(replayed.Value(), before.Value()));
        }

        shelf.Value().Reset();
        other.Value().Reset();
        REQUIRE(objects.Collect(access));
        CHECK(objects.Stats().liveObjects == 0);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}
