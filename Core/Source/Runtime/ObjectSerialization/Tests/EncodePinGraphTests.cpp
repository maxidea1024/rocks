#include <Core/Memory/SystemAllocator.h>
#include <Object/GeneratedFieldAccess.h>
#include <Object/GeneratedFieldCodec.h>
#include <ObjectSerialization/ObjectGraph.h>
#include <TestSupport/Test.h>

using namespace rocks;

// 그래프 저장의 encode 고정 시험이다(Docs/Design/ReflectionFoundations.ko.md 5.6절 "encode 고정"):
// SaveObjectGraph는 노드의 필드를 encode하는 동안 노드마다 PinForEncode를 쥔다. 저장 중 콜백이
// Registry를 바꾸지 않는다는 계약을 어겨도, 고정된 노드의 컨테이너·참조 변경과 등록된 필드 쓰기는
// Busy가 된다. 고정은 성공·실패 모든 경로에서 풀린다.
namespace {

constexpr Guid PinNodeType{0x50494e4e4f4445ull, 1};
constexpr Guid ChildId{0x50494e, 1};
constexpr Guid ProbeId{0x50494e, 2};
constexpr Guid TextId{0x50494e, 3};

struct PinNode final : Object {
    explicit PinNode(Allocator& allocator) noexcept : text(allocator), items(allocator) {
    }

    void Trace(TraceVisitor& visitor) const noexcept {
        visitor.Visit(child);
        visitor.Visit(link);
        items.Trace(visitor);
    }

    ObjectRef<PinNode> child;
    uint32 probe = 5;
    String text;
    // 시험용 비영속 멤버다(스키마 밖 — 저장 그래프에 참여하지 않는다).
    ObjectRef<PinNode> link;
    ObjectRefArray<PinNode> items;
};

struct EncodeAttempt {
    ObjectHandle other;
    // encode 도중 파괴 요청할 객체(없으면 null)와 probe 필드 encode를 실패시킬지다.
    ObjectHandle destroy;
    bool fail = false;
    uint32 calls = 0;
    ErrorCode ownArray = ErrorCode::DependencyFailed;
    ErrorCode ownReference = ErrorCode::DependencyFailed;
    ErrorCode ownText = ErrorCode::DependencyFailed;
    ErrorCode otherArray = ErrorCode::DependencyFailed;
};

EncodeAttempt Attempt;

ErrorCode ErrorOf(const Result<void>& result) noexcept {
    return result ? ErrorCode::None : result.Error();
}

// encode 도중의 계약 위반 콜백을 흉내 낸다: 지금 encode 중인 노드와 다른 노드를 바꿔 본 뒤 값을
// 쓴다.
Result<uint32> EncodeProbe(const RuntimeFieldOps& self, ObjectRegistry& registry,
                           const ObjectAccessContext& access, ObjectHandle object,
                           ArchiveWriter& out, FieldEncodeContext& context) noexcept {
    ++Attempt.calls;
    auto owner = registry.Resolve<PinNode>(access, object);
    auto other = registry.Resolve<PinNode>(access, Attempt.other);
    if (owner && other) {
        auto ownerPointer = owner.Value().Get();
        auto otherPointer = other.Value().Get();
        if (ownerPointer && otherPointer) {
            Attempt.ownArray = ErrorOf(
                registry.AddReference(access, object, ownerPointer.Value()->items, Attempt.other));
            Attempt.ownReference = ErrorOf(
                registry.SetReference(access, object, ownerPointer.Value()->link, Attempt.other));
            FieldValue text(ownerPointer.Value()->text.GetAllocator());
            text.kind = ValueKind::Utf8String;
            Attempt.ownText = text.string.TryAssign("changed during encode")
                                  ? ErrorOf(detail::WriteGeneratedField<PinNode>(
                                        registry, access, object, &PinNode::text, text))
                                  : ErrorCode::OutOfMemory;
            Attempt.otherArray = ErrorOf(
                registry.AddReference(access, Attempt.other, otherPointer.Value()->items, object));
        }
    }

    if (!Attempt.destroy.IsNull()) {
        static_cast<void>(registry.RequestDestroy(access, Attempt.destroy));
    }

    if (Attempt.fail) {
        return Result<uint32>::Failure(ErrorCode::DependencyFailed);
    }

    return detail::EncodeThroughFieldValue(self, registry, access, object, out, context);
}

template <auto Member>
RuntimeFieldOps FieldOps(Guid id, FieldEncodeFunction encode) noexcept {
    return {
        id,
        [](ObjectRegistry& registry, const ObjectAccessContext& access, ObjectHandle object,
           FieldValue& value) noexcept {
            return detail::ReadGeneratedField<PinNode>(registry, access, object, Member, value);
        },
        [](ObjectRegistry& registry, const ObjectAccessContext& access, ObjectHandle object,
           const FieldValue& value) noexcept {
            return detail::WriteGeneratedField<PinNode>(registry, access, object, Member, value);
        },
        [](ConstructionContext& transaction, ObjectHandle object,
           const FieldValue& value) noexcept {
            return detail::WriteGeneratedStagedField<PinNode>(transaction, object, Member, value);
        },
        encode,
        &detail::DecodeThroughFieldValue};
}

FieldSchema SchemaField(Guid id, const char* name, usize nameBytes, ValueKind kind) noexcept {
    FieldSchema field;
    field.id = id;
    field.name = {name, nameBytes};
    field.value.kind = kind;
    if (kind == ValueKind::ObjectReference) {
        field.kind = FieldKind::ObjectReference;
        field.value.type = PinNodeType;
    }

    return field;
}

// 필드 순서: child(참조 — BFS로 둘째 노드를 찾는다) → probe → text.
Result<TypeRegistrationToken> RegisterPinNode(TypeRegistry& types) noexcept {
    const FieldSchema fields[] = {SchemaField(ChildId, "child", 5, ValueKind::ObjectReference),
                                  SchemaField(ProbeId, "probe", 5, ValueKind::UInt32),
                                  SchemaField(TextId, "text", 4, ValueKind::Utf8String)};
    const RuntimeFieldOps accessors[] = {
        FieldOps<&PinNode::child>(ChildId, &detail::EncodeThroughFieldValue),
        FieldOps<&PinNode::probe>(ProbeId, &EncodeProbe),
        FieldOps<&PinNode::text>(TextId, &detail::EncodeThroughFieldValue)};
    auto ops = MakeRuntimeTypeOps<PinNode>();
    ops.fields = accessors;
    ops.completeFieldLayout = true;
    return types.Register({PinNodeType, 1, fields, {"PinNode", 7}}, Move(ops));
}

} // namespace

TEST(ObjectGraphSavePinsEachNodeWhileEncodingItsFields) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterPinNode(types));
        ObjectRegistry objects(allocator, types, {0x5049, 1});
        const auto access = objects.OwnerAccess();
        auto node = objects.NewObject<PinNode>(access, PinNodeType);
        auto other = objects.NewObject<PinNode>(access, PinNodeType);
        REQUIRE(node && other);
        Attempt = {};
        Attempt.other = other.Value().Handle();
        auto saved = SaveObjectGraph(allocator, objects, access, node.Value().Handle());
        REQUIRE(saved);
        CHECK(Attempt.calls == 1);
        // encode 중인 노드의 컨테이너·참조 변경과 등록된 필드 쓰기는 Busy, 고정되지 않은 다른
        // 노드는 바뀐다.
        CHECK(Attempt.ownArray == ErrorCode::Busy);
        CHECK(Attempt.ownReference == ErrorCode::Busy);
        CHECK(Attempt.ownText == ErrorCode::Busy);
        CHECK(Attempt.otherArray == ErrorCode::None);
        CHECK(objects.Stats().encodePins == 0);
        {
            auto borrowed = objects.Resolve<PinNode>(access, node.Value().Handle());
            REQUIRE(borrowed);
            auto pointer = borrowed.Value().Get();
            REQUIRE(pointer);
            CHECK(pointer.Value()->items.IsEmpty() &&
                  pointer.Value()->link.Handle() == ObjectHandle{} &&
                  pointer.Value()->text.IsEmpty());
            // 저장이 끝나면 고정이 풀린다.
            CHECK(objects.AddReference(access, node.Value().Handle(), pointer.Value()->items,
                                       other.Value().Handle()));
        }

        // 저장 결과는 그대로 복원된다.
        auto loaded = LoadObjectGraph(allocator, objects, types, access, saved.Value().View());
        REQUIRE(loaded);
        loaded.Value().Reset();
        node.Value().Reset();
        other.Value().Reset();
        REQUIRE(objects.Collect(access));
        CHECK(objects.Stats().liveObjects == 0);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(ObjectGraphSaveReleasesPinsOnEveryFailure) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterPinNode(types));
        ObjectRegistry objects(allocator, types, {0x5049, 2});
        const auto access = objects.OwnerAccess();
        auto node = objects.NewObject<PinNode>(access, PinNodeType);
        auto child = objects.NewObject<PinNode>(access, PinNodeType);
        auto other = objects.NewObject<PinNode>(access, PinNodeType);
        REQUIRE(node && child && other);
        {
            auto borrowed = objects.Resolve<PinNode>(access, node.Value().Handle());
            REQUIRE(borrowed);
            REQUIRE(objects.SetReference(access, node.Value().Handle(),
                                         borrowed.Value().Get().Value()->child,
                                         child.Value().Handle()));
        }

        // encode 오류: 노드의 고정이 풀려 저장 뒤 바로 바꿀 수 있다.
        Attempt = {};
        Attempt.other = other.Value().Handle();
        Attempt.fail = true;
        CHECK(SaveObjectGraph(allocator, objects, access, node.Value().Handle()).Error() ==
              ErrorCode::DependencyFailed);
        CHECK(Attempt.calls == 1 && Attempt.ownArray == ErrorCode::Busy);
        CHECK(objects.Stats().encodePins == 0);
        {
            auto borrowed = objects.Resolve<PinNode>(access, node.Value().Handle());
            REQUIRE(borrowed);
            CHECK(objects.AddReference(access, node.Value().Handle(),
                                       borrowed.Value().Get().Value()->items,
                                       other.Value().Handle()));
        }

        // 고정 획득 실패: 루트 encode 중 child를 파괴 요청하면 child 노드의 PinForEncode가
        // Destroyed로 실패하고, 쥐고 있던 고정은 모두 풀린다.
        Attempt = {};
        Attempt.other = other.Value().Handle();
        Attempt.destroy = child.Value().Handle();
        CHECK(SaveObjectGraph(allocator, objects, access, node.Value().Handle()).Error() ==
              ErrorCode::Destroyed);
        CHECK(Attempt.calls == 1);
        CHECK(objects.Stats().encodePins == 0);
        node.Value().Reset();
        child.Value().Reset();
        other.Value().Reset();
        REQUIRE(objects.Collect(access));
        CHECK(objects.Stats().liveObjects == 0);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}
