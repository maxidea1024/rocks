#include <Core/Memory/SystemAllocator.h>
#include <Object/GeneratedFieldCodec.h>
#include <ObjectSerialization/ObjectGraph.h>
#include <TestSupport/Test.h>
#include "GraphTestSupport.h"

// graph v2 한도 시험이다(Docs/Design/ObjectFoundations.ko.md 5.6절·11절 결정 4·5, 9절 Od1 "한도"):
// 기본값, 64 MiB 파일·값 경계, 바이트·필드·원소 상한을 각각 채운 입력과 원소 상한을 외부 참조로
// 채운 입력(Oe — 위치 표 청구 포함)이 기본 작업량 안에서 저장·복원되는지, 작업량이 입력에
// 선형인지(두 배 입력 → 청구 두 배 이내).
using namespace rocks;

namespace limit_fixture {

// 원소 struct다(멤버 둘 — Record 하나에 TLV 둘).
struct Pair {
    uint32 x = 0;
    uint32 y = 0;
};

constexpr TypeId PairId{0x474c4d53ULL, 1};
constexpr Guid PairXId{0x474c4d4dULL, 1};
constexpr Guid PairYId{0x474c4d4dULL, 2};

FieldSchema Scalar(Guid id, const char* name, usize bytes, ValueKind kind) noexcept {
    FieldSchema field;
    field.id = id;
    field.name = {name, bytes};
    field.value = ValueType{kind, {}};
    return field;
}

FieldSchema ArrayOf(Guid id, const char* name, usize bytes, ValueKind element,
                    TypeId type = {}) noexcept {
    FieldSchema field;
    field.id = id;
    field.name = {name, bytes};
    field.value = ValueType{ValueKind::Array, {}};
    field.element = ValueType{element, type};
    field.maxElements = MaxArrayElements;
    return field;
}

} // namespace limit_fixture

namespace rocks {

template <>
struct GeneratedType<limit_fixture::Pair> {
    static constexpr bool ContainsStrongReference = false;
    static constexpr bool ContainsReferenceArray = false;

    static constexpr TypeId Id() noexcept {
        return limit_fixture::PairId;
    }

    static const TypeSchema& Schema() noexcept {
        using namespace limit_fixture;
        static const FieldSchema fields[] = {Scalar(PairXId, "x", 1, ValueKind::UInt32),
                                             Scalar(PairYId, "y", 1, ValueKind::UInt32)};
        static const TypeSchema schema{Id(), 1, fields, {"Pair", 4}};
        return schema;
    }

    static void Trace(const limit_fixture::Pair&, TraceVisitor&) noexcept {
    }

    static RuntimeStructOps Ops() noexcept {
        using namespace limit_fixture;
        static const RuntimeStructFieldOps fields[] = {detail::StructFieldOps<&Pair::x>(PairXId),
                                                       detail::StructFieldOps<&Pair::y>(PairYId)};
        RuntimeStructOps ops = MakeRuntimeStructOps<Pair>();
        ops.fields = fields;
        return ops;
    }
};

} // namespace rocks

namespace {

using limit_fixture::ArrayOf;
using limit_fixture::Pair;
using limit_fixture::Scalar;

constexpr TypeId TextNodeId{0x474c4d43ULL, 1};
constexpr TypeId FlagsNodeId{0x474c4d43ULL, 2};
constexpr TypeId PairsNodeId{0x474c4d43ULL, 3};
constexpr TypeId CountsNodeId{0x474c4d43ULL, 4};
constexpr TypeId RefsNodeId{0x474c4d43ULL, 5};
constexpr Guid TextId{0x474c4d46ULL, 1};
constexpr Guid FlagsAId{0x474c4d46ULL, 2};
constexpr Guid FlagsBId{0x474c4d46ULL, 3};
constexpr Guid FlagsCId{0x474c4d46ULL, 4};
constexpr Guid FlagsDId{0x474c4d46ULL, 5};
constexpr Guid ExtraId{0x474c4d46ULL, 6};
constexpr Guid PairsId{0x474c4d46ULL, 7};
constexpr Guid CountsId{0x474c4d46ULL, 8};
constexpr Guid NextId{0x474c4d46ULL, 9};
constexpr Guid RefsId{0x474c4d46ULL, 10};
constexpr usize MiB = 1024 * 1024;

struct TextNode final : Object {
    explicit TextNode(Allocator& allocator) noexcept : text(allocator) {
    }

    String text;
};

struct FlagsNode final : Object {
    explicit FlagsNode(Allocator& allocator) noexcept
        : a(allocator), b(allocator), c(allocator), d(allocator) {
    }

    void Trace(TraceVisitor& visitor) const noexcept {
        detail::TraceArrayField(a, visitor);
        detail::TraceArrayField(b, visitor);
        detail::TraceArrayField(c, visitor);
        detail::TraceArrayField(d, visitor);
    }

    Array<bool> a;
    Array<bool> b;
    Array<bool> c;
    Array<bool> d;
};

struct PairsNode final : Object {
    explicit PairsNode(Allocator& allocator) noexcept : pairs(allocator) {
    }

    void Trace(TraceVisitor& visitor) const noexcept {
        detail::TraceArrayField(pairs, visitor);
    }

    uint32 extra = 0;
    Array<Pair> pairs;
};

// 작업량 선형 시험용이다: U32 배열과 다음 노드 참조.
struct CountsNode final : Object {
    explicit CountsNode(Allocator& allocator) noexcept : counts(allocator) {
    }

    void Trace(TraceVisitor& visitor) const noexcept {
        detail::TraceArrayField(counts, visitor);
        visitor.Visit(next);
    }

    Array<uint32> counts;
    ObjectRef<CountsNode> next;
};

// 외부 참조 원소 상한 fixture용이다: 강한 참조 배열 하나.
struct RefsNode final : Object {
    explicit RefsNode(Allocator& allocator) noexcept : refs(allocator) {
    }

    void Trace(TraceVisitor& visitor) const noexcept {
        refs.Trace(visitor);
    }

    ObjectRefArray<RefsNode> refs;
};

// 집합 밖 대상을 모두 외부 참조로 쓴다(Admit).
struct ImportAll final : FieldCodecHooks {
    Result<ExternalAdmission>
    AdmitExternalReference(const ExternalReferenceInfo&) noexcept override {
        return Result<ExternalAdmission>::Success(ExternalAdmission::Import);
    }
};

// 생성 코드와 같은 모양의 클래스 필드 함수표다.
template <auto Member>
RuntimeFieldOps CodecField(Guid id, const void* valueNative = nullptr) noexcept {
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
    return ops;
}

// 단일 값 필드 함수표다(read·write·writeStaged와 생성 codec).
template <auto Member>
RuntimeFieldOps ValueField(Guid id) noexcept {
    using O = typename detail::StructMemberPointer<decltype(Member)>::Owner;
    RuntimeFieldOps ops = CodecField<Member>(id);
    ops.read = +[](ObjectRegistry& registry, const ObjectAccessContext& access, ObjectHandle object,
                   FieldValue& value) noexcept {
        return detail::ReadGeneratedField<O>(registry, access, object, Member, value);
    };
    ops.write = +[](ObjectRegistry& registry, const ObjectAccessContext& access,
                    ObjectHandle object, const FieldValue& value) noexcept {
        return detail::WriteGeneratedField<O>(registry, access, object, Member, value);
    };
    ops.writeStaged = +[](ConstructionContext& transaction, ObjectHandle object,
                          const FieldValue& value) noexcept {
        return detail::WriteGeneratedStagedField<O>(transaction, object, Member, value);
    };
    return ops;
}

template <class C>
bool RegisterClass(TypeRegistry& types, TypeId id, const char* name, usize bytes,
                   Span<const FieldSchema> fields, Span<const RuntimeFieldOps> ops) noexcept {
    auto runtime = MakeRuntimeTypeOps<C>();
    runtime.fields = ops;
    runtime.completeFieldLayout = true;
    return types.Register({id, 1, fields, {name, bytes}}, Move(runtime)).IsOk();
}

bool RegisterTypes(TypeRegistry& types) noexcept {
    if (!types.RegisterStruct(GeneratedType<Pair>::Schema(), GeneratedType<Pair>::Ops())) {
        return false;
    }

    const FieldSchema textFields[] = {Scalar(TextId, "text", 4, ValueKind::Utf8String)};
    const RuntimeFieldOps textOps[] = {ValueField<&TextNode::text>(TextId)};
    const FieldSchema flagFields[] = {
        ArrayOf(FlagsAId, "a", 1, ValueKind::Bool), ArrayOf(FlagsBId, "b", 1, ValueKind::Bool),
        ArrayOf(FlagsCId, "c", 1, ValueKind::Bool), ArrayOf(FlagsDId, "d", 1, ValueKind::Bool)};
    const RuntimeFieldOps flagOps[] = {
        CodecField<&FlagsNode::a>(FlagsAId), CodecField<&FlagsNode::b>(FlagsBId),
        CodecField<&FlagsNode::c>(FlagsCId), CodecField<&FlagsNode::d>(FlagsDId)};
    const FieldSchema pairFields[] = {
        Scalar(ExtraId, "extra", 5, ValueKind::UInt32),
        ArrayOf(PairsId, "pairs", 5, ValueKind::Struct, limit_fixture::PairId)};
    const RuntimeFieldOps pairOps[] = {
        ValueField<&PairsNode::extra>(ExtraId),
        CodecField<&PairsNode::pairs>(PairsId, NativeTypeKey<Pair>())};
    FieldSchema countFields[] = {ArrayOf(CountsId, "counts", 6, ValueKind::UInt32),
                                 Scalar(NextId, "next", 4, ValueKind::ObjectReference)};
    countFields[1].kind = FieldKind::ObjectReference;
    countFields[1].value.type = CountsNodeId;
    const RuntimeFieldOps countOps[] = {CodecField<&CountsNode::counts>(CountsId),
                                        ValueField<&CountsNode::next>(NextId)};
    FieldSchema refFields[] = {ArrayOf(RefsId, "refs", 4, ValueKind::ObjectReference, RefsNodeId)};
    refFields[0].kind = FieldKind::ObjectReference;
    const RuntimeFieldOps refOps[] = {CodecField<&RefsNode::refs>(RefsId)};
    return RegisterClass<TextNode>(types, TextNodeId, "TextNode", 8, textFields, textOps) &&
           RegisterClass<FlagsNode>(types, FlagsNodeId, "FlagsNode", 9, flagFields, flagOps) &&
           RegisterClass<PairsNode>(types, PairsNodeId, "PairsNode", 9, pairFields, pairOps) &&
           RegisterClass<CountsNode>(types, CountsNodeId, "CountsNode", 10, countFields,
                                     countOps) &&
           RegisterClass<RefsNode>(types, RefsNodeId, "RefsNode", 8, refFields, refOps);
}

// TextNode 하나를 만들고 text를 bytes개의 'a'로 채운다.
Result<RootRef<TextNode>> NewText(ObjectRegistry& registry, usize bytes,
                                  Allocator& allocator) noexcept {
    auto made = registry.NewObject<TextNode>(registry.OwnerAccess(), TextNodeId);
    if (!made) {
        return made;
    }

    Array<char> buffer(allocator);
    if (auto resized = buffer.TryResize(bytes); !resized) {
        return Result<RootRef<TextNode>>::Failure(resized.Error());
    }

    for (usize index = 0; index < bytes; ++index) {
        buffer[index] = 'a';
    }

    auto borrowed = registry.Resolve<TextNode>(registry.OwnerAccess(), made.Value().Handle());
    if (!borrowed) {
        return Result<RootRef<TextNode>>::Failure(borrowed.Error());
    }

    if (auto assigned = borrowed.Value().Get().Value()->text.TryAssign(
            StringView(buffer.Data(), buffer.Size()));
        !assigned) {
        return Result<RootRef<TextNode>>::Failure(assigned.Error());
    }

    return made;
}

// 로드가 성공하는 가장 작은 maxWork다(이분 탐색 — 실패하면 0).
uint64 MinimumLoadWork(Allocator& allocator, TypeRegistry& types,
                       Span<const uint8> bytes) noexcept {
    uint64 low = 0;
    uint64 high = ObjectGraphLimits{}.maxWork;
    {
        ObjectRegistry registry(allocator, types, {0x474c3f, 1});
        if (!LoadObjectGraph(allocator, registry, types, registry.OwnerAccess(), bytes)) {
            return 0;
        }
    }

    while (low + 1 < high) {
        const uint64 middle = low + (high - low) / 2;
        ObjectRegistry registry(allocator, types, {0x474c3f, 2});
        ObjectGraphLoadOptions options;
        options.limits.maxWork = middle;
        if (LoadObjectGraph(allocator, registry, types, registry.OwnerAccess(), bytes, options)) {
            high = middle;
        } else {
            low = middle;
        }
    }

    return high;
}

// 저장이 성공하는 가장 작은 maxWork다(이분 탐색 — 실패하면 0).
uint64 MinimumSaveWork(Allocator& allocator, ObjectRegistry& registry, ObjectHandle root) noexcept {
    if (!SaveObjectGraph(allocator, registry, registry.OwnerAccess(), root)) {
        return 0;
    }

    uint64 low = 0;
    uint64 high = ObjectGraphLimits{}.maxWork;
    while (low + 1 < high) {
        const uint64 middle = low + (high - low) / 2;
        ObjectGraphSaveOptions options;
        options.limits.maxWork = middle;
        if (SaveObjectGraph(allocator, registry, registry.OwnerAccess(), root, options)) {
            high = middle;
        } else {
            low = middle;
        }
    }

    return high;
}

// count개 노드 사슬(각 노드에 U32 원소 elements개)을 만들고 첫 노드를 돌려준다.
Result<ObjectHandle> NewChain(ObjectRegistry& registry, Array<RootRef<CountsNode>>& roots,
                              usize count, usize elements) noexcept {
    for (usize index = 0; index < count; ++index) {
        auto made = registry.NewObject<CountsNode>(registry.OwnerAccess(), CountsNodeId);
        if (!made) {
            return Result<ObjectHandle>::Failure(made.Error());
        }

        auto borrowed = registry.Resolve<CountsNode>(registry.OwnerAccess(), made.Value().Handle());
        if (!borrowed) {
            return Result<ObjectHandle>::Failure(borrowed.Error());
        }

        CountsNode& node = *borrowed.Value().Get().Value();
        for (usize element = 0; element < elements; ++element) {
            if (!node.counts.TryPushBack(static_cast<uint32>(element))) {
                return Result<ObjectHandle>::Failure(ErrorCode::OutOfMemory);
            }
        }

        if (index != 0) {
            auto previous =
                registry.Resolve<CountsNode>(registry.OwnerAccess(), roots[index - 1].Handle());
            if (!previous) {
                return Result<ObjectHandle>::Failure(previous.Error());
            }

            if (auto linked = registry.SetReference(
                    registry.OwnerAccess(), roots[index - 1].Handle(),
                    previous.Value().Get().Value()->next, made.Value().Handle());
                !linked) {
                return Result<ObjectHandle>::Failure(linked.Error());
            }
        }

        if (!roots.TryPushBack(Move(made.Value()))) {
            return Result<ObjectHandle>::Failure(ErrorCode::OutOfMemory);
        }
    }

    return Result<ObjectHandle>::Success(roots[0].Handle());
}

// 외부 참조 원소 상한 fixture다: 집합 밖 대상 target 하나와 RefsNode 넷(원소 MaxArrayElements개가
// 모두 target — 넷의 합이 maxTotalElements)을 한 트랜잭션으로 만들고 roots에 쥔다(roots[0] =
// target, roots[1..4] = 노드).
Result<void> NewExternalFixture(ObjectRegistry& registry, Allocator& allocator,
                                Array<RootRef<Object>>& roots) noexcept {
    ConstructionContext transaction(registry, registry.OwnerAccess());
    auto target = transaction.Create<RefsNode>(RefsNodeId);
    if (!target) {
        return Result<void>::Failure(target.Error());
    }

    Array<ObjectHandle> targets(allocator);
    if (auto resized = targets.TryResize(MaxArrayElements); !resized) {
        return resized;
    }

    for (usize index = 0; index < targets.Size(); ++index) {
        targets[index] = target.Value();
    }

    ObjectHandle handles[5] = {target.Value()};
    for (usize index = 1; index < 5; ++index) {
        auto node = transaction.Create<RefsNode>(RefsNodeId);
        if (!node) {
            return Result<void>::Failure(node.Error());
        }

        auto value = transaction.Get<RefsNode>(node.Value());
        if (!value) {
            return Result<void>::Failure(value.Error());
        }

        if (auto assigned =
                transaction.AssignReferences(node.Value(), value.Value()->refs, targets.View());
            !assigned) {
            return assigned;
        }

        handles[index] = node.Value();
    }

    if (auto resized = roots.TryResize(5); !resized) {
        return resized;
    }

    return transaction.Commit(handles, roots.View());
}

} // namespace

// 기본 한도(5.6절): 객체 4096, 외부 참조 65536, 필드·원소 누계 그대로, 바이트·값·마이그레이션 64
// MiB, 작업량 8 × (필드 + 원소 + 바이트).
TEST(GraphLimit_DefaultsFollowDecisionFive) {
    const ObjectGraphLimits limits;
    CHECK(limits.maxObjects == 4096);
    CHECK(limits.maxExternalReferences == 65536);
    CHECK(limits.maxTotalFields == 1024 * 1024);
    CHECK(limits.maxTotalElements == 4 * 1024 * 1024);
    CHECK(limits.maxBytes == 64 * MiB);
    CHECK(limits.maxValueBytes == 64 * MiB);
    CHECK(limits.maxMigrationBytes == 64 * MiB);
    CHECK(limits.maxWork ==
          8 * (uint64{limits.maxTotalFields} + limits.maxTotalElements + uint64{limits.maxBytes}));
}

// 파일 64 MiB 경계: 정확히 maxBytes인 그래프는 기본 한도(작업량 포함)로 저장·복원되고 1 byte 더
// 크면 저장·복원 모두 CapacityExceeded다. 바이트 상한을 채운 fixture다(결정 5).
TEST(GraphLimit_FileBytesBoundaryWithinDefaultWork) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterTypes(types));
        ObjectRegistry source(allocator, types, {0x474c31, 1});
        ObjectRegistry target(allocator, types, {0x474c31, 2});
        {
            // header 24 + NodeTable 항목 28 + Record 길이 8 + Record header 44 + TLV header 28 +
            // 문자열 길이 8 = 140 byte가 문자열 밖이다.
            constexpr usize Overhead = 140;
            auto exact = NewText(source, 64 * MiB - Overhead, allocator);
            REQUIRE(exact);
            {
                auto saved = SaveObjectGraph(allocator, source, source.OwnerAccess(),
                                             exact.Value().Handle());
                REQUIRE(saved);
                CHECK(saved.Value().Size() == 64 * MiB);
                {
                    auto loaded = LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                                  saved.Value().View());
                    REQUIRE(loaded);
                    auto borrowed =
                        target.Resolve<TextNode>(target.OwnerAccess(), loaded.Value().Handle());
                    REQUIRE(borrowed);
                    CHECK(borrowed.Value().Get().Value()->text.ByteCount() == 64 * MiB - Overhead);
                }

                REQUIRE(target.Collect(target.OwnerAccess()));
                REQUIRE(saved.Value().TryPushBack(uint8{0}));
                CHECK(LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                      saved.Value().View())
                          .Error() == ErrorCode::CapacityExceeded);
            }

            exact.Value().Reset();
            REQUIRE(source.Collect(source.OwnerAccess()));
            auto over = NewText(source, 64 * MiB - Overhead + 1, allocator);
            REQUIRE(over);
            CHECK(SaveObjectGraph(allocator, source, source.OwnerAccess(), over.Value().Handle())
                      .Error() == ErrorCode::CapacityExceeded);
            CHECK(target.Stats().liveObjects == 0);
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(target.Shutdown(target.OwnerAccess()));
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 값 64 MiB 경계(결정 4): encoded 값이 정확히 maxValueBytes(문자열 길이 8 + 본문)인 필드는 저장·
// 복원되고 1 byte 더 크면 저장이 CapacityExceeded다(값 하나를 담도록 파일 한도만 넓힌다).
TEST(GraphLimit_ValueBytesBoundary) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterTypes(types));
        ObjectRegistry source(allocator, types, {0x474c32, 1});
        ObjectRegistry target(allocator, types, {0x474c32, 2});
        {
            ObjectGraphSaveOptions save;
            save.limits.maxBytes = 64 * MiB + 4096;
            ObjectGraphLoadOptions load;
            load.limits.maxBytes = save.limits.maxBytes;
            auto exact = NewText(source, 64 * MiB - 8, allocator);
            REQUIRE(exact);
            {
                auto saved = SaveObjectGraph(allocator, source, source.OwnerAccess(),
                                             exact.Value().Handle(), save);
                REQUIRE(saved);
                auto loaded = LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                              saved.Value().View(), load);
                REQUIRE(loaded);
            }

            exact.Value().Reset();
            REQUIRE(source.Collect(source.OwnerAccess()));
            REQUIRE(target.Collect(target.OwnerAccess()));
            auto over = NewText(source, 64 * MiB - 7, allocator);
            REQUIRE(over);
            CHECK(SaveObjectGraph(allocator, source, source.OwnerAccess(), over.Value().Handle(),
                                  save)
                      .Error() == ErrorCode::CapacityExceeded);
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(target.Shutdown(target.OwnerAccess()));
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 원소 상한을 채운 fixture(결정 5): bool 배열 넷 × MaxArrayElements = maxTotalElements가 기본
// 한도로 저장·복원된다.
TEST(GraphLimit_MaximumElementsWithinDefaultWork) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterTypes(types));
        ObjectRegistry source(allocator, types, {0x474c33, 1});
        ObjectRegistry target(allocator, types, {0x474c33, 2});
        {
            auto made = source.NewObject<FlagsNode>(source.OwnerAccess(), FlagsNodeId);
            REQUIRE(made);
            {
                auto borrowed =
                    source.Resolve<FlagsNode>(source.OwnerAccess(), made.Value().Handle());
                REQUIRE(borrowed);
                FlagsNode& node = *borrowed.Value().Get().Value();
                Array<bool>* arrays[] = {&node.a, &node.b, &node.c, &node.d};
                for (Array<bool>* array : arrays) {
                    REQUIRE(array->TryResize(MaxArrayElements));
                    (*array)[MaxArrayElements - 1] = true;
                }
            }

            CHECK(uint64{4} * MaxArrayElements == ObjectGraphLimits{}.maxTotalElements);
            auto saved =
                SaveObjectGraph(allocator, source, source.OwnerAccess(), made.Value().Handle());
            REQUIRE(saved);
            auto loaded = LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                          saved.Value().View());
            REQUIRE(loaded);
            auto borrowed =
                target.Resolve<FlagsNode>(target.OwnerAccess(), loaded.Value().Handle());
            REQUIRE(borrowed);
            const FlagsNode& node = *borrowed.Value().Get().Value();
            CHECK(node.d.Size() == MaxArrayElements && node.d[MaxArrayElements - 1] && !node.d[0]);
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(target.Shutdown(target.OwnerAccess()));
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 필드 상한을 채운 fixture(결정 5): 멤버 둘짜리 struct 원소 524,287개(TLV 1,048,574) + 객체 필드 둘
// = maxTotalFields가 기본 한도로 저장·복원되고, 원소 하나를 더하면 저장이 CapacityExceeded다.
TEST(GraphLimit_MaximumFieldsWithinDefaultWork) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterTypes(types));
        ObjectRegistry source(allocator, types, {0x474c34, 1});
        ObjectRegistry target(allocator, types, {0x474c34, 2});
        {
            constexpr usize Elements = (1024 * 1024 - 2) / 2;
            auto made = source.NewObject<PairsNode>(source.OwnerAccess(), PairsNodeId);
            REQUIRE(made);
            {
                auto borrowed =
                    source.Resolve<PairsNode>(source.OwnerAccess(), made.Value().Handle());
                REQUIRE(borrowed);
                PairsNode& node = *borrowed.Value().Get().Value();
                REQUIRE(node.pairs.TryReserve(Elements + 1));
                for (usize index = 0; index < Elements; ++index) {
                    REQUIRE(node.pairs.TryPushBack(Pair{static_cast<uint32>(index), 1}));
                }
            }

            {
                auto saved =
                    SaveObjectGraph(allocator, source, source.OwnerAccess(), made.Value().Handle());
                REQUIRE(saved);
                auto loaded = LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                              saved.Value().View());
                REQUIRE(loaded);
                auto borrowed =
                    target.Resolve<PairsNode>(target.OwnerAccess(), loaded.Value().Handle());
                REQUIRE(borrowed);
                const PairsNode& node = *borrowed.Value().Get().Value();
                CHECK(node.pairs.Size() == Elements && node.pairs[Elements - 1].x == Elements - 1);
            }

            {
                auto borrowed =
                    source.Resolve<PairsNode>(source.OwnerAccess(), made.Value().Handle());
                REQUIRE(borrowed);
                REQUIRE(borrowed.Value().Get().Value()->pairs.TryPushBack(Pair{}));
            }

            CHECK(SaveObjectGraph(allocator, source, source.OwnerAccess(), made.Value().Handle())
                      .Error() == ErrorCode::CapacityExceeded);
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(target.Shutdown(target.OwnerAccess()));
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 원소 상한을 외부 참조로 채운 fixture(결정 5 — Oe 위치 표 청구 포함): RefsNode 넷 ×
// MaxArrayElements 원소 = maxTotalElements가 모두 집합 밖 대상 하나를 가리키는 그래프(외부 참조
// 4,194,304개 — 위치 표 16 MiB가 출력 약 16 MiB와 함께 바이트 누계에 든다)가 기본 한도로 저장되고,
// 단계형 복원이 import 하나로 모든 원소를 잇는다.
TEST(GraphLimit_MaximumExternalReferencesWithinDefaultWork) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterTypes(types));
        Array<uint8> bytes(allocator);
        {
            ObjectRegistry source(allocator, types, {0x474c36, 1});
            {
                Array<RootRef<Object>> roots(allocator);
                REQUIRE(NewExternalFixture(source, allocator, roots));
                const ObjectHandle set[] = {roots[1].Handle(), roots[2].Handle(), roots[3].Handle(),
                                            roots[4].Handle()};
                ImportAll hooks;
                ObjectGraphSaveOptions options;
                options.externals = GraphExternalPolicy::Admit;
                options.writeNodeTable = true;
                options.hooks = &hooks;
                auto saved =
                    SaveObjectGraphSet(allocator, source, source.OwnerAccess(), set, options);
                REQUIRE(saved);
                REQUIRE(saved.Value().Externals().Size() == 1);
                CHECK(saved.Value().Externals()[0].occurrences ==
                      ObjectGraphLimits{}.maxTotalElements);
                bytes = saved.Value().TakeBytes();
            }

            REQUIRE(source.Shutdown(source.OwnerAccess()));
        }

        ObjectRegistry target(allocator, types, {0x474c36, 2});
        {
            auto imported = target.NewObject<RefsNode>(target.OwnerAccess(), RefsNodeId);
            REQUIRE(imported);
            const ObjectHandle imports[] = {imported.Value().Handle()};
            const TypeId importTypes[] = {RefsNodeId};
            ObjectGraphStage stage(allocator, target, types, target.OwnerAccess());
            ObjectGraphInput input;
            input.bytes = bytes.View();
            input.importTypes = importTypes;
            REQUIRE(stage.Validate(input));
            ConstructionContext transaction(target, target.OwnerAccess());
            REQUIRE(stage.CreateObjects(transaction));
            REQUIRE(stage.ApplyFields(transaction, imports));
            auto last = transaction.Get<RefsNode>(stage.Nodes()[3]);
            REQUIRE(last);
            CHECK(last.Value()->refs.Size() == MaxArrayElements &&
                  last.Value()->refs[MaxArrayElements - 1] == imports[0]);
        }

        REQUIRE(target.Shutdown(target.OwnerAccess()));
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 작업량은 입력에 선형이다: 원소 수와 노드 수를 두 배로 늘린 입력의 최소 작업량(저장·복원)이 두 배
// 이내다.
TEST(GraphLimit_WorkIsLinearInInput) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        REQUIRE(RegisterTypes(types));
        ObjectRegistry source(allocator, types, {0x474c35, 1});
        {
            struct Shape {
                usize nodes;
                usize elements;
            };

            const Shape shapes[][2] = {{{1, 20000}, {1, 40000}}, {{50, 10}, {100, 10}}};
            for (const auto& pair : shapes) {
                uint64 load[2] = {};
                uint64 save[2] = {};
                for (usize index = 0; index < 2; ++index) {
                    Array<RootRef<CountsNode>> roots(allocator);
                    auto root = NewChain(source, roots, pair[index].nodes, pair[index].elements);
                    REQUIRE(root);
                    auto bytes =
                        SaveObjectGraph(allocator, source, source.OwnerAccess(), root.Value());
                    REQUIRE(bytes);
                    load[index] = MinimumLoadWork(allocator, types, bytes.Value().View());
                    save[index] = MinimumSaveWork(allocator, source, root.Value());
                }

                CHECK(load[0] != 0 && load[1] != 0 && save[0] != 0 && save[1] != 0);
                CHECK(load[1] <= 2 * load[0]);
                CHECK(save[1] <= 2 * save[0]);
                CHECK(load[1] > load[0] && save[1] > save[0]);
                REQUIRE(source.Collect(source.OwnerAccess()));
            }
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}
