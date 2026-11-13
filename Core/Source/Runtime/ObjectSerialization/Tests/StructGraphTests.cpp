#include <ReflectionTestFixtures/ReflectionFixture.generated.h>
#include <Core/Memory/SystemAllocator.h>
#include <Object/ConstructionContext.h>
#include <ObjectSerialization/ObjectGraph.h>
#include <Serialization/Record.h>
#include <TestSupport/Test.h>
#include <new>

// 리플렉션 struct의 그래프 저장·복원 시험이다(Docs/Design/ReflectionFoundations.ko.md 4.4·4.6절).
// GeneratedType 특수화는 MetaCompiler 생성 코드(B4b)와 같은 모양으로 손으로 썼다: 3단 중첩(Holder →
// Middle → Leaf), 참조 struct(Holder::target, Required), struct 안 enum·String·EditorOnly 멤버.
using namespace rocks;
using reflection_fixture::TextureGroup;

namespace struct_fixture {

struct Owner;

struct Leaf {
    explicit Leaf(Allocator& allocator) noexcept : label(allocator) {
    }

    uint32 count = 0;
    String label;
    TextureGroup group = TextureGroup::World;
};

struct Middle {
    explicit Middle(Allocator& allocator) noexcept : leaf(allocator) {
    }

    Leaf leaf;
    int64 offset = 0;
    uint16 note = 0;
};

struct Holder {
    explicit Holder(Allocator& allocator) noexcept : middle(allocator) {
    }

    ObjectRef<Owner> target;
    Middle middle;
    uint8 flag = 0;
};

struct Owner final : Object {
    explicit Owner(Allocator& allocator) noexcept : holder(allocator), extra(allocator) {
        ++Constructed;
    }

    void Trace(TraceVisitor& visitor) const noexcept;

    static inline uint32 Constructed = 0;
    Holder holder;
    uint32 plain = 0;
    Leaf extra;
};

constexpr TypeId LeafId{0x534c, 1};
constexpr TypeId MiddleId{0x534c, 2};
constexpr TypeId HolderId{0x534c, 3};
constexpr TypeId OwnerId{0x534c, 4};
constexpr Guid CountId{0x5346, 1};
constexpr Guid LabelId{0x5346, 2};
constexpr Guid GroupId{0x5346, 3};
constexpr Guid LeafFieldId{0x5346, 4};
constexpr Guid OffsetId{0x5346, 5};
constexpr Guid NoteId{0x5346, 6};
constexpr Guid TargetId{0x5346, 7};
constexpr Guid MiddleFieldId{0x5346, 8};
constexpr Guid FlagId{0x5346, 9};
constexpr Guid HolderFieldId{0x5346, 10};
constexpr Guid PlainId{0x5346, 11};
constexpr Guid ExtraId{0x5346, 12};
constexpr Guid ShadowId{0x5346, 13};

constexpr uint32 EditorOnly = static_cast<uint32>(FieldFlags::EditorOnly);
constexpr uint32 Required = static_cast<uint32>(FieldFlags::Required);

FieldSchema Field(Guid id, const char* name, ValueKind kind, TypeId type = {},
                  uint32 flags = 0) noexcept {
    usize bytes = 0;
    while (name[bytes] != '\0') {
        ++bytes;
    }

    FieldSchema field;
    field.id = id;
    field.kind = kind == ValueKind::ObjectReference ? FieldKind::ObjectReference : FieldKind::Value;
    field.flags = flags;
    field.name = Span<const char>(name, bytes);
    field.value = ValueType{kind, type};
    return field;
}

template <class S>
RuntimeStructOps MakeStructOps(Span<const RuntimeStructFieldOps> fields) noexcept {
    RuntimeStructOps ops;
    ops.size = sizeof(S);
    ops.alignment = alignof(S);
    ops.nativeType = NativeTypeKey<S>();
    ops.construct = [](void* memory, Allocator& allocator) noexcept {
        ::new (memory) S(allocator);
    };
    ops.destroy = [](void* memory) noexcept { static_cast<S*>(memory)->~S(); };
    if (GeneratedType<S>::ContainsStrongReference) {
        ops.trace = [](const void* value, TraceVisitor& visitor) noexcept {
            GeneratedType<S>::Trace(*static_cast<const S*>(value), visitor);
        };
    }

    ops.fields = fields;
    return ops;
}

// 클래스의 struct 필드 함수표다(생성 코드와 같은 모양 — read·write·writeStaged 없음).
template <auto Member>
RuntimeFieldOps ClassStructField(Guid id) noexcept {
    using Traits = detail::StructMemberPointer<decltype(Member)>;
    using O = typename Traits::Owner;
    return {
        id,
        nullptr,
        {},
        nullptr,
        +[](const RuntimeFieldOps&, ObjectRegistry& registry, const ObjectAccessContext& access,
            ObjectHandle object, ArchiveWriter& out, FieldEncodeContext& context) noexcept {
            return detail::EncodeGeneratedField<O>(registry, access, object, Member, out, context);
        },
        +[](const RuntimeFieldOps&, ConstructionContext* construction, ObjectHandle object,
            const FieldView& field, FieldDecodeContext& context) noexcept {
            return detail::DecodeGeneratedField<O>(construction, object, Member, field, context);
        },
        NativeTypeKey<typename Traits::Member>()};
}

// 클래스의 단일 값 필드 함수표다(생성 코드와 같은 모양).
template <auto Member>
RuntimeFieldOps ClassScalarField(Guid id) noexcept {
    using O = typename detail::StructMemberPointer<decltype(Member)>::Owner;
    return {
        id,
        [](ObjectRegistry& registry, const ObjectAccessContext& access, ObjectHandle object,
           FieldValue& value) noexcept {
            return detail::ReadGeneratedField<O>(registry, access, object, Member, value);
        },
        [](ObjectRegistry& registry, const ObjectAccessContext& access, ObjectHandle object,
           const FieldValue& value) noexcept {
            return detail::WriteGeneratedField<O>(registry, access, object, Member, value);
        },
        [](ConstructionContext& transaction, ObjectHandle object,
           const FieldValue& value) noexcept {
            return detail::WriteGeneratedStagedField<O>(transaction, object, Member, value);
        },
        +[](const RuntimeFieldOps&, ObjectRegistry& registry, const ObjectAccessContext& access,
            ObjectHandle object, ArchiveWriter& out, FieldEncodeContext& context) noexcept {
            return detail::EncodeGeneratedField<O>(registry, access, object, Member, out, context);
        },
        +[](const RuntimeFieldOps&, ConstructionContext* construction, ObjectHandle object,
            const FieldView& field, FieldDecodeContext& context) noexcept {
            return detail::DecodeGeneratedField<O>(construction, object, Member, field, context);
        }};
}

} // namespace struct_fixture

namespace rocks {

template <>
struct GeneratedType<struct_fixture::Leaf> {
    static constexpr bool ContainsStrongReference = false;

    static constexpr TypeId Id() noexcept {
        return struct_fixture::LeafId;
    }

    static const TypeSchema& Schema() noexcept {
        using namespace struct_fixture;
        static const FieldSchema fields[] = {
            Field(CountId, "count", ValueKind::UInt32),
            Field(LabelId, "label", ValueKind::Utf8String),
            Field(GroupId, "group", ValueKind::Enum, TypeIdOf<TextureGroup>())};
        static const TypeSchema schema{Id(), 1, fields, {"Leaf", 4}};
        return schema;
    }

    static void Trace(const struct_fixture::Leaf&, TraceVisitor&) noexcept {
    }

    static RuntimeStructOps Ops() noexcept {
        using namespace struct_fixture;
        static const RuntimeStructFieldOps fields[] = {
            detail::StructFieldOps<&Leaf::count>(CountId),
            detail::StructFieldOps<&Leaf::label>(LabelId),
            detail::StructFieldOps<&Leaf::group>(GroupId)};
        return MakeStructOps<Leaf>(fields);
    }
};

template <>
struct GeneratedType<struct_fixture::Middle> {
    static constexpr bool ContainsStrongReference = false;

    static constexpr TypeId Id() noexcept {
        return struct_fixture::MiddleId;
    }

    static const TypeSchema& Schema() noexcept {
        using namespace struct_fixture;
        static const FieldSchema fields[] = {
            Field(LeafFieldId, "leaf", ValueKind::Struct, LeafId),
            Field(OffsetId, "offset", ValueKind::Int64),
            Field(NoteId, "note", ValueKind::UInt16, {}, EditorOnly)};
        static const TypeSchema schema{Id(), 1, fields, {"Middle", 6}};
        return schema;
    }

    static void Trace(const struct_fixture::Middle&, TraceVisitor&) noexcept {
    }

    static RuntimeStructOps Ops() noexcept {
        using namespace struct_fixture;
        static const RuntimeStructFieldOps fields[] = {
            detail::StructFieldOps<&Middle::leaf>(LeafFieldId),
            detail::StructFieldOps<&Middle::offset>(OffsetId),
            detail::StructFieldOps<&Middle::note>(NoteId)};
        return MakeStructOps<Middle>(fields);
    }
};

template <>
struct GeneratedType<struct_fixture::Holder> {
    static constexpr bool ContainsStrongReference = true;

    static constexpr TypeId Id() noexcept {
        return struct_fixture::HolderId;
    }

    static const TypeSchema& Schema() noexcept {
        using namespace struct_fixture;
        static const FieldSchema fields[] = {
            Field(TargetId, "target", ValueKind::ObjectReference, OwnerId, Required),
            Field(MiddleFieldId, "middle", ValueKind::Struct, MiddleId),
            Field(FlagId, "flag", ValueKind::UInt8)};
        static const TypeSchema schema{Id(), 1, fields, {"Holder", 6}};
        return schema;
    }

    static void Trace(const struct_fixture::Holder& value, TraceVisitor& visitor) noexcept {
        visitor.Visit(value.target);
    }

    static RuntimeStructOps Ops() noexcept {
        using namespace struct_fixture;
        static const RuntimeStructFieldOps fields[] = {
            detail::StructFieldOps<&Holder::target>(TargetId),
            detail::StructFieldOps<&Holder::middle>(MiddleFieldId),
            detail::StructFieldOps<&Holder::flag>(FlagId)};
        return MakeStructOps<Holder>(fields);
    }
};

template <>
struct GeneratedType<struct_fixture::Owner> {
    static constexpr TypeId Id() noexcept {
        return struct_fixture::OwnerId;
    }

    static const TypeSchema& Schema() noexcept {
        using namespace struct_fixture;
        static const FieldSchema fields[] = {
            Field(HolderFieldId, "holder", ValueKind::Struct, HolderId),
            Field(PlainId, "plain", ValueKind::UInt32),
            Field(ExtraId, "extra", ValueKind::Struct, LeafId, EditorOnly)};
        static const TypeSchema schema{Id(), 1, fields, {"Owner", 5}};
        return schema;
    }

    static RuntimeTypeOps Ops() noexcept {
        using namespace struct_fixture;
        static const RuntimeFieldOps fields[] = {ClassStructField<&Owner::holder>(HolderFieldId),
                                                 ClassScalarField<&Owner::plain>(PlainId),
                                                 ClassStructField<&Owner::extra>(ExtraId)};
        auto ops = MakeRuntimeTypeOps<Owner>();
        ops.fields = fields;
        ops.completeFieldLayout = true;
        return ops;
    }
};

} // namespace rocks

void struct_fixture::Owner::Trace(TraceVisitor& visitor) const noexcept {
    GeneratedType<Holder>::Trace(holder, visitor);
}

namespace {

using namespace struct_fixture;

// 진단·관찰·치환을 기록한다.
class StructHooks final : public FieldCodecHooks {
public:
    Result<void> Report(const FieldDiagnostic& diagnostic) noexcept override {
        if (count < 16) {
            kinds[count] = diagnostic.kind;
            fields[count] = diagnostic.field;
            paths[count] = diagnostic.path.Size();
            ++count;
        }

        return Result<void>::Success();
    }

    usize Count(FieldDiagnosticKind kind, usize pathSize = ~usize(0)) const noexcept {
        usize found = 0;
        for (usize index = 0; index < count; ++index) {
            found += kinds[index] == kind && (pathSize == ~usize(0) || paths[index] == pathSize);
        }

        return found;
    }

    Span<const TypeId> ObservedStructs() noexcept override {
        return observe;
    }

    Result<void> ObserveStruct(const FieldCodecFrame&, TypeId type, const void* nativeKey,
                               const void* value) noexcept override {
        if (failObserve) {
            return Result<void>::Failure(ErrorCode::DependencyFailed);
        }

        if (type == LeafId && nativeKey == NativeTypeKey<Leaf>() && observed < 4) {
            observedCounts[observed++] = static_cast<const Leaf*>(value)->count;
        }

        if (type == MiddleId && nativeKey == NativeTypeKey<Middle>()) {
            ++observedMiddles;
            lastMiddleLeafCount = static_cast<const Middle*>(value)->leaf.count;
        }

        return Result<void>::Success();
    }

    Result<const void*> SubstituteStruct(const FieldCodecFrame&, TypeId type, const void*,
                                         const void* in) noexcept override {
        substitutedHolder = substitutedHolder || type == HolderId;
        if (type == LeafId && replacement) {
            return Result<const void*>::Success(nullLeaf ? nullptr : replacement);
        }

        if (type == MiddleId && middleReplacement) {
            return Result<const void*>::Success(middleReplacement);
        }

        return Result<const void*>::Success(in);
    }

    FieldDiagnosticKind kinds[16] = {};
    Guid fields[16] = {};
    usize paths[16] = {};
    usize count = 0;
    Span<const TypeId> observe{};
    uint32 observedCounts[4] = {};
    usize observed = 0;
    const Leaf* replacement = nullptr;
    const Middle* middleReplacement = nullptr;
    bool nullLeaf = false;
    bool substitutedHolder = false;
    bool failObserve = false;
    usize observedMiddles = 0;
    uint32 lastMiddleLeafCount = 0;
};

// 저장 byte에서 중첩 Record(schema)의 버전을 바꾼다(첫 번째 것). 찾으면 true다.
bool PatchNestedVersion(Array<uint8>& bytes, TypeId schema, uint32 version) noexcept {
    const uint8 magic[] = {'R', 'O', 'C', 'K', 'S', 'R', 'E', 'C'};
    for (usize at = 0; at + 44 <= bytes.Size(); ++at) {
        bool match = true;
        for (usize index = 0; match && index < 8; ++index) {
            match = bytes[at + index] == magic[index];
        }

        uint64 high = 0;
        uint64 low = 0;
        for (usize index = 0; match && index < 8; ++index) {
            high |= uint64{bytes[at + 12 + index]} << (8 * index);
            low |= uint64{bytes[at + 20 + index]} << (8 * index);
        }

        if (match && high == schema.high && low == schema.low) {
            for (usize index = 0; index < 4; ++index) {
                bytes[at + 28 + index] = static_cast<uint8>(version >> (8 * index));
            }

            return true;
        }
    }

    return false;
}

// 저장 byte에서 필드 ID의 I64 값(첫 번째 TLV)을 바꾼다. 찾으면 true다.
bool PatchI64Field(Array<uint8>& bytes, Guid field, int64 value) noexcept {
    for (usize at = 0; at + 36 <= bytes.Size(); ++at) {
        uint64 high = 0;
        uint64 low = 0;
        uint32 wire = 0;
        uint64 length = 0;
        for (usize index = 0; index < 8; ++index) {
            high |= uint64{bytes[at + index]} << (8 * index);
            low |= uint64{bytes[at + 8 + index]} << (8 * index);
            length |= uint64{bytes[at + 20 + index]} << (8 * index);
        }

        for (usize index = 0; index < 4; ++index) {
            wire |= uint32{bytes[at + 16 + index]} << (8 * index);
        }

        if (high == field.high && low == field.low && wire == static_cast<uint32>(WireType::I64) &&
            length == 8) {
            for (usize index = 0; index < 8; ++index) {
                bytes[at + 28 + index] =
                    static_cast<uint8>(static_cast<uint64>(value) >> (8 * index));
            }

            return true;
        }
    }

    return false;
}

// 다른 struct ID의 Record를 돌려주는 잘못된 override다.
Result<Array<uint8>> WrongSchemaOverride(Allocator& allocator, TypeId, uint32, uint32 to,
                                         Span<const uint8>, void*) noexcept {
    RecordBuilder builder(allocator, MiddleId, to);
    return builder.Build();
}

// Leaf v1 → v2 마이그레이션이다(필드를 그대로 옮기고 버전만 올린다). 호출 수를 센다.
uint32 MigrationCalls = 0;

Result<Array<uint8>> MigrateLeaf(Allocator& allocator, TypeId type, uint32 from, uint32 to,
                                 Span<const uint8> source) noexcept {
    ++MigrationCalls;
    auto reader = RecordReader::Open(allocator, source, type, from, from);
    if (!reader) {
        return Result<Array<uint8>>::Failure(reader.Error());
    }

    RecordBuilder builder(allocator, type, to);
    for (usize index = 0; index < reader.Value().FieldCount(); ++index) {
        auto field = reader.Value().Next();
        if (!field) {
            return Result<Array<uint8>>::Failure(field.Error());
        }

        if (auto added =
                builder.AddField(field.Value().id, field.Value().wireType, field.Value().payload);
            !added) {
            return Result<Array<uint8>>::Failure(added.Error());
        }
    }

    return builder.Build();
}

Result<Array<uint8>> UnsupportedOverride(Allocator&, TypeId, uint32, uint32, Span<const uint8>,
                                         void*) noexcept {
    return Result<Array<uint8>>::Failure(ErrorCode::Unsupported);
}

// 버전을 올리지 않고 그대로 돌려주는 잘못된 override다(결과는 현재 버전이어야 한다).
Result<Array<uint8>> SameVersionOverride(Allocator& allocator, TypeId, uint32, uint32,
                                         Span<const uint8> source, void*) noexcept {
    Array<uint8> copy(allocator);
    if (auto reserved = copy.TryReserve(source.Size()); !reserved) {
        return Result<Array<uint8>>::Failure(reserved.Error());
    }

    for (uint8 byte : source) {
        if (!copy.TryPushBack(byte)) {
            return Result<Array<uint8>>::Failure(ErrorCode::OutOfMemory);
        }
    }

    return Result<Array<uint8>>::Success(Move(copy));
}

// leafVersion·leafMigrate·shadow(Leaf에 count를 다시 쓰는 모르는 필드)·withHolder(Owner의 holder
// 필드 유무)로 고른 fixture를 등록한다.
struct FixtureOptions {
    uint32 leafVersion = 1;
    StructMigration leafMigrate = nullptr;
    bool shadow = false;
    bool withHolder = true;
};

bool RegisterFixture(TypeRegistry& types, FixtureOptions options = {}) noexcept {
    if (!GeneratedType<TextureGroup>::Register(types)) {
        return false;
    }

    TypeSchema leaf = GeneratedType<Leaf>::Schema();
    leaf.version = options.leafVersion;
    RuntimeStructOps leafOps = GeneratedType<Leaf>::Ops();
    leafOps.migrate = options.leafMigrate;
    static FieldSchema shadowFields[4];
    static RuntimeStructFieldOps shadowOps[4];
    if (options.shadow) {
        for (usize index = 0; index < 3; ++index) {
            shadowFields[index] = leaf.fields[index];
            shadowOps[index] = leafOps.fields[index];
        }

        shadowFields[3] = Field(ShadowId, "shadow", ValueKind::UInt32);
        shadowOps[3] = detail::StructFieldOps<&Leaf::count>(ShadowId);
        leaf.fields = Span<const FieldSchema>(shadowFields, 4);
        leafOps.fields = Span<const RuntimeStructFieldOps>(shadowOps, 4);
    }

    if (!types.RegisterStruct(leaf, Move(leafOps)) ||
        !types.RegisterStruct(GeneratedType<Middle>::Schema(), GeneratedType<Middle>::Ops()) ||
        !types.RegisterStruct(GeneratedType<Holder>::Schema(), GeneratedType<Holder>::Ops())) {
        return false;
    }

    if (options.withHolder) {
        return types.Register(GeneratedType<Owner>::Schema(), GeneratedType<Owner>::Ops()).IsOk();
    }

    // holder 필드 없는 Owner 스키마(누락 struct 안 Required 진단용).
    static const FieldSchema fields[] = {
        Field(PlainId, "plain", ValueKind::UInt32),
        Field(ExtraId, "extra", ValueKind::Struct, LeafId, EditorOnly)};
    static const RuntimeFieldOps ops[] = {ClassScalarField<&Owner::plain>(PlainId),
                                          ClassStructField<&Owner::extra>(ExtraId)};
    auto runtime = MakeRuntimeTypeOps<Owner>();
    runtime.fields = ops;
    runtime.completeFieldLayout = true;
    return types.Register({OwnerId, 1, fields, {"Owner", 5}}, Move(runtime)).IsOk();
}

// A(값을 채움)와 A.holder.target이 가리키는 B를 만들고 A의 root를 돌려준다.
Result<RootRef<Object>> MakeGraph(ObjectRegistry& objects) noexcept {
    ConstructionContext transaction(objects, objects.OwnerAccess());
    auto a = transaction.Create<Owner>(OwnerId);
    auto b = transaction.Create<Owner>(OwnerId);
    if (!a || !b) {
        return Result<RootRef<Object>>::Failure(!a ? a.Error() : b.Error());
    }

    auto owner = transaction.Get<Owner>(a.Value());
    if (!owner) {
        return Result<RootRef<Object>>::Failure(owner.Error());
    }

    Owner& value = *owner.Value();
    value.plain = 7;
    value.holder.flag = 3;
    value.holder.middle.offset = -5;
    value.holder.middle.note = 11;
    value.holder.middle.leaf.count = 42;
    value.holder.middle.leaf.group = TextureGroup::EightBitData;
    value.extra.count = 9;
    if (!value.holder.middle.leaf.label.TryAssign("중첩") || !value.extra.label.TryAssign("편집")) {
        return Result<RootRef<Object>>::Failure(ErrorCode::OutOfMemory);
    }

    if (auto linked = transaction.SetReference(a.Value(), value.holder.target, b.Value());
        !linked) {
        return Result<RootRef<Object>>::Failure(linked.Error());
    }

    return transaction.Commit();
}

bool SameBytes(Span<const uint8> left, Span<const uint8> right) noexcept {
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

// 그래프 하나를 저장한다.
Result<Array<uint8>> SaveFixture(Allocator& allocator, ObjectRegistry& objects,
                                 ObjectGraphSaveOptions options = {}) noexcept {
    auto root = MakeGraph(objects);
    if (!root) {
        return Result<Array<uint8>>::Failure(root.Error());
    }

    return SaveObjectGraph(allocator, objects, objects.OwnerAccess(), root.Value().Handle(),
                           options);
}

} // namespace

TEST(StructGraph_NestedReferenceStructRoundTrips) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry source(allocator, types, {0x5347, 1});
        ObjectRegistry target(allocator, types, {0x5347, 2});
        {
            auto bytes = SaveFixture(allocator, source);
            REQUIRE(bytes);
            auto loaded = LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                          bytes.Value().View());
            REQUIRE(loaded);
            {
                auto borrowed =
                    target.Resolve<Owner>(target.OwnerAccess(), loaded.Value().Handle());
                REQUIRE(borrowed);
                const Owner& owner = *borrowed.Value().Get().Value();
                CHECK(owner.plain == 7);
                CHECK(owner.holder.flag == 3);
                CHECK(owner.holder.middle.offset == -5);
                CHECK(owner.holder.middle.note == 11);
                CHECK(owner.holder.middle.leaf.count == 42);
                CHECK(owner.holder.middle.leaf.label.View() == StringView("중첩"));
                CHECK(owner.holder.middle.leaf.group == TextureGroup::EightBitData);
                CHECK(owner.extra.count == 9);
                CHECK(owner.extra.label.View() == StringView("편집"));
                CHECK(owner.holder.target.Handle() != ObjectHandle{});
            }

            // B는 A.holder.target(struct 안 참조)으로만 도달한다 — Trace가 방문해 수집에서 산다.
            REQUIRE(target.Collect(target.OwnerAccess()));
            CHECK(target.Stats().liveObjects == 2);
            auto resaved =
                SaveObjectGraph(allocator, target, target.OwnerAccess(), loaded.Value().Handle());
            REQUIRE(resaved);
            CHECK(SameBytes(bytes.Value().View(), resaved.Value().View()));
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(target.Shutdown(target.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}

// EditorOnly는 최상위 struct 필드(extra)와 struct 안 멤버(Middle::note) 모두 제외되고, 누락 허용
// 플래그로 다시 읽는다.
TEST(StructGraph_EditorOnlyExclusionAndAllowedMissing) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry source(allocator, types, {0x5347, 3});
        ObjectRegistry target(allocator, types, {0x5347, 4});
        {
            ObjectGraphSaveOptions save;
            save.policy.excludedFlags = EditorOnly;
            auto bytes = SaveFixture(allocator, source, save);
            REQUIRE(bytes);
            const uint32 constructed = Owner::Constructed;
            CHECK(LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                  bytes.Value().View())
                      .Error() == ErrorCode::NotFound);
            CHECK(Owner::Constructed == constructed);
            ObjectGraphLoadOptions load;
            load.policy.allowMissingFlags = EditorOnly;
            auto loaded = LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                          bytes.Value().View(), load);
            REQUIRE(loaded);
            auto borrowed = target.Resolve<Owner>(target.OwnerAccess(), loaded.Value().Handle());
            REQUIRE(borrowed);
            const Owner& owner = *borrowed.Value().Get().Value();
            CHECK(owner.holder.middle.leaf.count == 42);
            CHECK(owner.holder.middle.note == 0);
            CHECK(owner.extra.count == 0);
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(target.Shutdown(target.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}

// 낮은 struct 버전: 타입이 등록한 마이그레이션이 등장마다 한 번만 불리고(2단계는 보관 결과를
// 재사용), 관찰도 같은 한 번의 해석으로 온다. override는 Unsupported면 타입 마이그레이션으로
// 넘어가고, 결과가 현재 버전이 아니면 Unsupported다. 마이그레이션이 없으면 Unsupported다.
TEST(StructGraph_OlderStructVersionMigratesOncePerOccurrence) {
    SystemAllocator allocator;
    TypeRegistry oldTypes(allocator);
    TypeRegistry newTypes(allocator);
    TypeRegistry bareTypes(allocator);
    REQUIRE(RegisterFixture(oldTypes));
    REQUIRE(RegisterFixture(newTypes, {2, &MigrateLeaf}));
    REQUIRE(RegisterFixture(bareTypes, {2, nullptr}));
    {
        ObjectRegistry source(allocator, oldTypes, {0x5347, 5});
        ObjectRegistry target(allocator, newTypes, {0x5347, 6});
        ObjectRegistry bare(allocator, bareTypes, {0x5347, 7});
        {
            auto bytes = SaveFixture(allocator, source);
            REQUIRE(bytes);
            const TypeId observedIds[] = {LeafId};
            StructHooks hooks;
            hooks.observe = observedIds;
            ObjectGraphLoadOptions load;
            load.hooks = &hooks;
            MigrationCalls = 0;
            auto loaded = LoadObjectGraph(allocator, target, newTypes, target.OwnerAccess(),
                                          bytes.Value().View(), load);
            REQUIRE(loaded);
            // A의 두 Leaf와 B의 두 Leaf, 각각 한 번.
            CHECK(MigrationCalls == 4);
            CHECK(hooks.observed == 4);
            CHECK(hooks.observedCounts[0] == 42);
            CHECK(hooks.observedCounts[1] == 9);
            auto borrowed = target.Resolve<Owner>(target.OwnerAccess(), loaded.Value().Handle());
            REQUIRE(borrowed);
            CHECK(borrowed.Value().Get().Value()->holder.middle.leaf.label.View() ==
                  StringView("중첩"));

            // override가 Unsupported면 타입 마이그레이션을 쓴다.
            load.migrateStruct = &UnsupportedOverride;
            MigrationCalls = 0;
            CHECK(LoadObjectGraph(allocator, target, newTypes, target.OwnerAccess(),
                                  bytes.Value().View(), load));
            CHECK(MigrationCalls == 4);

            // 결과가 현재 버전이 아니면 생성 전에 Unsupported다.
            load.migrateStruct = &SameVersionOverride;
            const uint32 constructed = Owner::Constructed;
            CHECK(LoadObjectGraph(allocator, target, newTypes, target.OwnerAccess(),
                                  bytes.Value().View(), load)
                      .Error() == ErrorCode::Unsupported);
            CHECK(Owner::Constructed == constructed);

            // 마이그레이션이 없으면 Unsupported다.
            CHECK(LoadObjectGraph(allocator, bare, bareTypes, bare.OwnerAccess(),
                                  bytes.Value().View())
                      .Error() == ErrorCode::Unsupported);
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(target.Shutdown(target.OwnerAccess()));
        REQUIRE(bare.Shutdown(bare.OwnerAccess()));
    }

    CHECK(oldTypes.Shutdown());
    CHECK(newTypes.Shutdown());
    CHECK(bareTypes.Shutdown());
}

// 높은 struct 버전: 기본 거부, LoadNewerSkippingUnknown이면 path 있는 NewerVersion 진단 뒤 읽는다.
TEST(StructGraph_NewerStructVersionPolicy) {
    SystemAllocator allocator;
    TypeRegistry oldTypes(allocator);
    TypeRegistry newTypes(allocator);
    REQUIRE(RegisterFixture(oldTypes));
    REQUIRE(RegisterFixture(newTypes, {2, &MigrateLeaf}));
    {
        ObjectRegistry source(allocator, newTypes, {0x5347, 8});
        ObjectRegistry target(allocator, oldTypes, {0x5347, 9});
        {
            auto bytes = SaveFixture(allocator, source);
            REQUIRE(bytes);
            CHECK(LoadObjectGraph(allocator, target, oldTypes, target.OwnerAccess(),
                                  bytes.Value().View())
                      .Error() == ErrorCode::Unsupported);
            StructHooks hooks;
            ObjectGraphLoadOptions load;
            load.policy.newerVersion = NewerVersionPolicy::LoadNewerSkippingUnknown;
            load.hooks = &hooks;
            auto loaded = LoadObjectGraph(allocator, target, oldTypes, target.OwnerAccess(),
                                          bytes.Value().View(), load);
            REQUIRE(loaded);
            // holder.middle.leaf(path 3)와 extra(path 1), 객체 둘.
            CHECK(hooks.Count(FieldDiagnosticKind::NewerVersion, 3) == 2);
            CHECK(hooks.Count(FieldDiagnosticKind::NewerVersion, 1) == 2);
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(target.Shutdown(target.OwnerAccess()));
    }

    CHECK(oldTypes.Shutdown());
    CHECK(newTypes.Shutdown());
}

// struct 안의 모르는 필드(path 포함 진단·Reject), 누락 필드(NotFound·허용), 누락된 struct 필드 안의
// Required 후손 진단.
TEST(StructGraph_UnknownAndMissingMembers) {
    SystemAllocator allocator;
    TypeRegistry plainTypes(allocator);
    TypeRegistry shadowTypes(allocator);
    TypeRegistry holderlessTypes(allocator);
    REQUIRE(RegisterFixture(plainTypes));
    REQUIRE(RegisterFixture(shadowTypes, {1, nullptr, true}));
    REQUIRE(RegisterFixture(holderlessTypes, {1, nullptr, false, false}));
    {
        ObjectRegistry plain(allocator, plainTypes, {0x5347, 10});
        ObjectRegistry shadow(allocator, shadowTypes, {0x5347, 11});
        ObjectRegistry holderless(allocator, holderlessTypes, {0x5347, 12});
        {
            auto shadowBytes = SaveFixture(allocator, shadow);
            REQUIRE(shadowBytes);
            StructHooks hooks;
            ObjectGraphLoadOptions load;
            load.hooks = &hooks;
            CHECK(LoadObjectGraph(allocator, plain, plainTypes, plain.OwnerAccess(),
                                  shadowBytes.Value().View(), load));
            CHECK(hooks.Count(FieldDiagnosticKind::UnknownField, 3) == 2);
            CHECK(hooks.Count(FieldDiagnosticKind::UnknownField, 1) == 2);
            CHECK(hooks.fields[0] == ShadowId);
            load.policy.unknownField = UnknownFieldPolicy::Reject;
            const uint32 constructed = Owner::Constructed;
            CHECK(LoadObjectGraph(allocator, plain, plainTypes, plain.OwnerAccess(),
                                  shadowBytes.Value().View(), load)
                      .Error() == ErrorCode::InvalidArgument);
            CHECK(Owner::Constructed == constructed);

            auto plainBytes = SaveFixture(allocator, plain);
            REQUIRE(plainBytes);
            CHECK(LoadObjectGraph(allocator, shadow, shadowTypes, shadow.OwnerAccess(),
                                  plainBytes.Value().View())
                      .Error() == ErrorCode::NotFound);
            ObjectGraphLoadOptions allow;
            allow.policy.allowMissingFields = true;
            CHECK(LoadObjectGraph(allocator, shadow, shadowTypes, shadow.OwnerAccess(),
                                  plainBytes.Value().View(), allow));

            // holder가 없는 데이터: 누락된 struct 필드 안의 Required 참조를 path 2로 진단한다.
            auto holderlessRoot = [&]() noexcept -> Result<RootRef<Object>> {
                ConstructionContext transaction(holderless, holderless.OwnerAccess());
                auto created = transaction.Create<Owner>(OwnerId);
                if (!created) {
                    return Result<RootRef<Object>>::Failure(created.Error());
                }

                return transaction.Commit();
            }();
            REQUIRE(holderlessRoot);
            auto holderlessBytes = SaveObjectGraph(allocator, holderless, holderless.OwnerAccess(),
                                                   holderlessRoot.Value().Handle());
            REQUIRE(holderlessBytes);
            StructHooks missing;
            allow.hooks = &missing;
            CHECK(LoadObjectGraph(allocator, plain, plainTypes, plain.OwnerAccess(),
                                  holderlessBytes.Value().View(), allow));
            CHECK(missing.Count(FieldDiagnosticKind::RequiredMissing, 2) == 1);
        }

        REQUIRE(plain.Shutdown(plain.OwnerAccess()));
        REQUIRE(shadow.Shutdown(shadow.OwnerAccess()));
        REQUIRE(holderless.Shutdown(holderless.OwnerAccess()));
    }

    CHECK(plainTypes.Shutdown());
    CHECK(shadowTypes.Shutdown());
    CHECK(holderlessTypes.Shutdown());
}

// 작업량·필드 누계 한도의 모든 실패가 생성자 전에 일어난다(중첩 struct 포함).
TEST(StructGraph_EveryLimitFailureHappensBeforeConstructors) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry source(allocator, types, {0x5347, 13});
        ObjectRegistry target(allocator, types, {0x5347, 14});
        {
            auto bytes = SaveFixture(allocator, source);
            REQUIRE(bytes);
            bool loaded = false;
            for (uint64 work = 0; !loaded && work < 1000000; ++work) {
                ObjectGraphLoadOptions options;
                options.limits.maxWork = work;
                const uint32 constructed = Owner::Constructed;
                auto result = LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                              bytes.Value().View(), options);
                if (!result) {
                    CHECK(result.Error() == ErrorCode::CapacityExceeded);
                    CHECK(Owner::Constructed == constructed);
                    continue;
                }

                loaded = true;
            }

            CHECK(loaded);
            // 객체 둘 × (Owner 3 + Holder 3 + Middle 3 + Leaf 3 + extra Leaf 3) = 30 TLV.
            ObjectGraphLoadOptions fields;
            fields.limits.maxTotalFields = 29;
            const uint32 constructed = Owner::Constructed;
            CHECK(LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                  bytes.Value().View(), fields)
                      .Error() == ErrorCode::CapacityExceeded);
            CHECK(Owner::Constructed == constructed);
            fields.limits.maxTotalFields = 30;
            CHECK(LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                  bytes.Value().View(), fields));
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(target.Shutdown(target.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}

// struct hook 계약: 관찰 대상은 등록된 참조 없는 struct여야 하고(NotFound·InvalidArgument), 치환은
// 참조 없는 struct에만 불리며 null은 InvalidArgument다.
TEST(StructGraph_StructHooksContract) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry source(allocator, types, {0x5347, 15});
        ObjectRegistry target(allocator, types, {0x5347, 16});
        {
            Leaf replacement(allocator);
            replacement.count = 1000;
            StructHooks hooks;
            hooks.replacement = &replacement;
            ObjectGraphSaveOptions save;
            save.hooks = &hooks;
            auto bytes = SaveFixture(allocator, source, save);
            REQUIRE(bytes);
            CHECK(!hooks.substitutedHolder);
            auto loaded = LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                          bytes.Value().View());
            REQUIRE(loaded);
            auto borrowed = target.Resolve<Owner>(target.OwnerAccess(), loaded.Value().Handle());
            REQUIRE(borrowed);
            CHECK(borrowed.Value().Get().Value()->holder.middle.leaf.count == 1000);
            CHECK(borrowed.Value().Get().Value()->extra.count == 1000);

            hooks.nullLeaf = true;
            CHECK(SaveFixture(allocator, source, save).Error() == ErrorCode::InvalidArgument);

            StructHooks observeHolder;
            const TypeId holderIds[] = {HolderId};
            observeHolder.observe = holderIds;
            ObjectGraphLoadOptions load;
            load.hooks = &observeHolder;
            CHECK(LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                  bytes.Value().View(), load)
                      .Error() == ErrorCode::InvalidArgument);
            const TypeId unknownIds[] = {TypeId{0x9999, 9}};
            observeHolder.observe = unknownIds;
            CHECK(LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                  bytes.Value().View(), load)
                      .Error() == ErrorCode::NotFound);
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(target.Shutdown(target.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}

// 큰 멤버(3 MiB 문자열)를 가진 struct도 값 한도 안이면 저장·복원되고, 중첩 Record를 제자리로 쓰므로
// 저장·복원 작업이 중첩 깊이 배수로 불어나지 않는다(라벨 byte의 작은 배수 안).
TEST(StructGraph_LargeMembersAndBoundedWork) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry source(allocator, types, {0x5347, 17});
        ObjectRegistry target(allocator, types, {0x5347, 18});
        constexpr usize LabelBytes = 3 * 1024 * 1024;
        {
            ConstructionContext transaction(source, source.OwnerAccess());
            auto created = transaction.Create<Owner>(OwnerId);
            REQUIRE(created);
            auto owner = transaction.Get<Owner>(created.Value());
            REQUIRE(owner);
            String& label = owner.Value()->holder.middle.leaf.label;
            {
                Array<char> text(allocator);
                REQUIRE(text.TryReserve(LabelBytes));
                for (usize index = 0; index < LabelBytes; ++index) {
                    REQUIRE(text.TryPushBack(static_cast<char>(97 + index % 26)));
                }

                REQUIRE(label.TryAssign(StringView(text.Data(), text.Size())));
            }

            owner.Value()->holder.middle.leaf.count = 5;
            auto root = transaction.Commit();
            REQUIRE(root);

            // 깊이 3의 struct 안 라벨: 저장·복원은 라벨 byte의 10배 작업 안에서 끝난다.
            ObjectGraphSaveOptions save;
            save.limits.maxWork = 10 * LabelBytes;
            auto bytes = SaveObjectGraph(allocator, source, source.OwnerAccess(),
                                         root.Value().Handle(), save);
            REQUIRE(bytes);
            ObjectGraphLoadOptions load;
            load.limits.maxWork = 10 * LabelBytes;
            auto loaded = LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                          bytes.Value().View(), load);
            REQUIRE(loaded);
            auto borrowed = target.Resolve<Owner>(target.OwnerAccess(), loaded.Value().Handle());
            REQUIRE(borrowed);
            const Owner& copy = *borrowed.Value().Get().Value();
            CHECK(copy.holder.middle.leaf.label.ByteCount() == LabelBytes);
            CHECK(copy.holder.middle.leaf.count == 5);
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(target.Shutdown(target.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}

// 저장 쪽 한도: 필드 누계는 로드와 같은 기준이고(30 TLV), 작업 한도의 모든 실패는
// CapacityExceeded다.
TEST(StructGraph_SaveLimits) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry source(allocator, types, {0x5347, 19});
        {
            auto root = MakeGraph(source);
            REQUIRE(root);
            ObjectGraphSaveOptions fields;
            fields.limits.maxTotalFields = 29;
            CHECK(SaveObjectGraph(allocator, source, source.OwnerAccess(), root.Value().Handle(),
                                  fields)
                      .Error() == ErrorCode::CapacityExceeded);
            fields.limits.maxTotalFields = 30;
            CHECK(SaveObjectGraph(allocator, source, source.OwnerAccess(), root.Value().Handle(),
                                  fields));
            bool saved = false;
            for (uint64 work = 0; !saved && work < 1000000; ++work) {
                ObjectGraphSaveOptions options;
                options.limits.maxWork = work;
                auto result = SaveObjectGraph(allocator, source, source.OwnerAccess(),
                                              root.Value().Handle(), options);
                if (!result) {
                    CHECK(result.Error() == ErrorCode::CapacityExceeded);
                    continue;
                }

                saved = true;
            }

            CHECK(saved);
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}

// 저장된 struct 버전 0은 InvalidArgument, 높은 버전 건너뛰기와 모르는 필드 Reject를 함께 주면
// Reject가 우선하고, 마이그레이션 결과가 다른 struct면 TypeMismatch다(모두 생성자 전).
TEST(StructGraph_VersionEdgeCases) {
    SystemAllocator allocator;
    TypeRegistry plainTypes(allocator);
    TypeRegistry newerShadowTypes(allocator);
    TypeRegistry newTypes(allocator);
    REQUIRE(RegisterFixture(plainTypes));
    REQUIRE(RegisterFixture(newerShadowTypes, {2, &MigrateLeaf, true}));
    REQUIRE(RegisterFixture(newTypes, {2, &MigrateLeaf}));
    {
        ObjectRegistry plain(allocator, plainTypes, {0x5347, 20});
        ObjectRegistry newerShadow(allocator, newerShadowTypes, {0x5347, 21});
        ObjectRegistry target(allocator, newTypes, {0x5347, 22});
        {
            auto bytes = SaveFixture(allocator, plain);
            REQUIRE(bytes);
            auto newer = SaveFixture(allocator, newerShadow);
            REQUIRE(newer);
            auto old = SaveFixture(allocator, plain);
            REQUIRE(old);
            const uint32 constructed = Owner::Constructed;
            REQUIRE(PatchNestedVersion(bytes.Value(), LeafId, 0));
            CHECK(LoadObjectGraph(allocator, plain, plainTypes, plain.OwnerAccess(),
                                  bytes.Value().View())
                      .Error() == ErrorCode::InvalidArgument);

            StructHooks hooks;
            ObjectGraphLoadOptions load;
            load.hooks = &hooks;
            load.policy.newerVersion = NewerVersionPolicy::LoadNewerSkippingUnknown;
            load.policy.unknownField = UnknownFieldPolicy::Reject;
            CHECK(LoadObjectGraph(allocator, plain, plainTypes, plain.OwnerAccess(),
                                  newer.Value().View(), load)
                      .Error() == ErrorCode::InvalidArgument);

            ObjectGraphLoadOptions wrong;
            wrong.migrateStruct = &WrongSchemaOverride;
            CHECK(LoadObjectGraph(allocator, target, newTypes, target.OwnerAccess(),
                                  old.Value().View(), wrong)
                      .Error() == ErrorCode::TypeMismatch);
            CHECK(Owner::Constructed == constructed);
        }

        REQUIRE(plain.Shutdown(plain.OwnerAccess()));
        REQUIRE(newerShadow.Shutdown(newerShadow.OwnerAccess()));
        REQUIRE(target.Shutdown(target.OwnerAccess()));
    }

    CHECK(plainTypes.Shutdown());
    CHECK(newerShadowTypes.Shutdown());
    CHECK(newTypes.Shutdown());
}

// struct 안 미선언 enum: 기본은 생성 전 거부, ReportAndDefault면 path 4(holder·middle·leaf·group)
// 진단 뒤 대체 값.
TEST(StructGraph_UndeclaredEnumInsideStruct) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry source(allocator, types, {0x5347, 23});
        ObjectRegistry target(allocator, types, {0x5347, 24});
        {
            auto bytes = SaveFixture(allocator, source);
            REQUIRE(bytes);
            REQUIRE(PatchI64Field(bytes.Value(), GroupId, 5));
            const uint32 constructed = Owner::Constructed;
            CHECK(LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                  bytes.Value().View())
                      .Error() == ErrorCode::InvalidArgument);
            CHECK(Owner::Constructed == constructed);
            StructHooks hooks;
            ObjectGraphLoadOptions load;
            load.hooks = &hooks;
            load.policy.undeclaredEnum = UndeclaredEnumPolicy::ReportAndDefault;
            auto loaded = LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                          bytes.Value().View(), load);
            REQUIRE(loaded);
            CHECK(hooks.Count(FieldDiagnosticKind::UndeclaredEnumValue, 4) == 1);
            auto borrowed = target.Resolve<Owner>(target.OwnerAccess(), loaded.Value().Handle());
            REQUIRE(borrowed);
            CHECK(borrowed.Value().Get().Value()->holder.middle.leaf.group == TextureGroup::World);
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(target.Shutdown(target.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}

// 관찰과 치환의 중첩: Middle·Leaf를 함께 관찰하면 Leaf는 Middle 안에서 제자리로 채워진 채 관찰되고,
// 관찰 실패는 생성 전 로드 실패다. Middle을 치환하면 그 안의 Leaf도 치환본에서 읽는다. 누락된
// Leaf(Required 후손 없음)는 RequiredMissing을 내지 않는다.
TEST(StructGraph_NestedObservationAndSubstitution) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry source(allocator, types, {0x5347, 25});
        ObjectRegistry target(allocator, types, {0x5347, 26});
        {
            auto bytes = SaveFixture(allocator, source);
            REQUIRE(bytes);
            const TypeId both[] = {LeafId, MiddleId};
            StructHooks hooks;
            hooks.observe = both;
            ObjectGraphLoadOptions load;
            load.hooks = &hooks;
            CHECK(LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                  bytes.Value().View(), load));
            CHECK(hooks.observed == 4);
            CHECK(hooks.observedMiddles == 2);
            hooks.failObserve = true;
            hooks.observed = 0;
            const uint32 constructed = Owner::Constructed;
            CHECK(LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                  bytes.Value().View(), load)
                      .Error() == ErrorCode::DependencyFailed);
            CHECK(Owner::Constructed == constructed);

            Middle replacement(allocator);
            replacement.offset = 99;
            replacement.leaf.count = 777;
            StructHooks substitute;
            substitute.middleReplacement = &replacement;
            ObjectGraphSaveOptions save;
            save.hooks = &substitute;
            auto substituted = SaveFixture(allocator, source, save);
            REQUIRE(substituted);
            auto loaded = LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                          substituted.Value().View());
            REQUIRE(loaded);
            auto borrowed = target.Resolve<Owner>(target.OwnerAccess(), loaded.Value().Handle());
            REQUIRE(borrowed);
            CHECK(borrowed.Value().Get().Value()->holder.middle.offset == 99);
            CHECK(borrowed.Value().Get().Value()->holder.middle.leaf.count == 777);
            CHECK(borrowed.Value().Get().Value()->extra.count == 9);

            ObjectGraphSaveOptions editor;
            editor.policy.excludedFlags = EditorOnly;
            auto excluded = SaveFixture(allocator, source, editor);
            REQUIRE(excluded);
            StructHooks missing;
            ObjectGraphLoadOptions allow;
            allow.hooks = &missing;
            allow.policy.allowMissingFlags = EditorOnly;
            CHECK(LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                  excluded.Value().View(), allow));
            // B의 null target(값 있는 Required null) 하나뿐이고, 누락된 Leaf(Required 후손 없음)는
            // 진단을 내지 않는다.
            CHECK(missing.Count(FieldDiagnosticKind::RequiredMissing) == 1);
            CHECK(missing.Count(FieldDiagnosticKind::RequiredMissing, 2) == 1);
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(target.Shutdown(target.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}
