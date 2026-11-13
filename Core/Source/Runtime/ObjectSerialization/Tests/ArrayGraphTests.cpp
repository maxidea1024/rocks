#include <ReflectionTestFixtures/ReflectionTestFixtures.module.generated.h>
#include <Core/Memory/SystemAllocator.h>
#include <Object/ConstructionContext.h>
#include <Object/ModuleRegistration.h>
#include <ObjectSerialization/ObjectGraph.h>
#include <Serialization/Record.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>
#include <new>

// 리플렉션 배열 필드의 그래프 저장·복원 시험이다(Docs/Design/ReflectionFoundations.ko.md 5.6절
// codec·그래프 B5b). GeneratedType 특수화는 MetaCompiler 생성 코드(B5c)와 같은 모양으로 손으로
// 썼다: 모든 원소 종류의 Array<T>(Binds Vec3·Flags enum·소프트 참조·digest 포함), struct 원소
// 배열(원소 struct 안 배열), null 원소를 섞은 ObjectRefArray, 참조 struct 원소의 StructRefArray,
// 인라인 struct 안 ObjectRefArray·StructRefArray. 생성 Trace처럼 모든 배열 필드·멤버에
// detail::TraceArrayField를 부른다.
using namespace rocks;
using reflection_fixture::Channels;
using reflection_fixture::TextureGroup;

namespace array_fixture {

struct Node;

// 참조 없는 struct다(Array<Tagged> 원소·관찰·치환·마이그레이션). 원소 struct 안 배열(enum 원소
// 배열 포함)을 담는다.
struct Tagged {
    explicit Tagged(Allocator& allocator) noexcept
        : label(allocator), codes(allocator), notes(allocator), masks(allocator) {
    }

    uint32 id = 0;
    String label;
    Array<uint32> codes;
    Array<String> notes;
    Array<Channels> masks;
};

// 참조 struct다(StructRefArray 원소). 기대 형식이 다른 참조 멤버 둘과 배열 멤버를 담는다.
struct Slot {
    explicit Slot(Allocator& allocator) noexcept : name(allocator), weights(allocator) {
    }

    ObjectRef<Node> material;
    ObjectRef<Object> overlay;
    String name;
    Array<float> weights;
};

// 인라인 참조 struct다(클래스 필드). Registry 컨테이너 둘을 담는다.
struct Bundle {
    explicit Bundle(Allocator& allocator) noexcept : refs(allocator), slots(allocator) {
    }

    ObjectRefArray<Node> refs;
    StructRefArray<Slot> slots;
};

// 큰 NATIVE(비영속) 멤버를 가진 원소 struct다 — 작은 wire로 큰 할당을 끌어내지 못해야 한다.
struct Heavy {
    uint32 value = 0;
    uint8 native[65536] = {};
};

struct Node final : Object {
    explicit Node(Allocator& allocator) noexcept
        : flags(allocator), bytes(allocator), shorts(allocator), ints(allocator), longs(allocator),
          doubles(allocator), guids(allocator), names(allocator), assets(allocator),
          digests(allocator), softs(allocator), channels(allocator), groups(allocator),
          points(allocator), tagged(allocator), refs(allocator), slots(allocator),
          bundle(allocator), single(allocator) {
        ++Constructed;
    }

    void Trace(TraceVisitor& visitor) const noexcept;

    static inline uint32 Constructed = 0;
    Array<bool> flags;
    Array<uint8> bytes;
    Array<uint16> shorts;
    Array<int32> ints;
    Array<uint64> longs;
    Array<double> doubles;
    Array<Guid> guids;
    Array<String> names;
    Array<AssetId> assets;
    Array<Sha256Digest> digests;
    Array<SoftObjectRef<Node>> softs;
    Array<Channels> channels;
    Array<TextureGroup> groups;
    Array<Vec3> points;
    Array<Tagged> tagged;
    ObjectRefArray<Node> refs;
    StructRefArray<Slot> slots;
    Bundle bundle;
    Tagged single;
};

constexpr TypeId NodeId{0x41434c, 1};
constexpr TypeId HubId{0x41434c, 2};
constexpr TypeId LooseId{0x41434c, 3};
constexpr TypeId OtherId{0x41434c, 4};
constexpr TypeId WideId{0x41434c, 5};
constexpr TypeId HeavyHolderId{0x41434c, 6};
constexpr TypeId DeepHolderId{0x41434c, 7};
constexpr TypeId TaggedId{0x415354, 1};
constexpr TypeId SlotId{0x415354, 2};
constexpr TypeId BundleId{0x415354, 3};
constexpr TypeId HeavyId{0x415354, 4};

constexpr Guid FlagsId{0x41464e, 1};
constexpr Guid BytesId{0x41464e, 2};
constexpr Guid ShortsId{0x41464e, 3};
constexpr Guid IntsId{0x41464e, 4};
constexpr Guid LongsId{0x41464e, 5};
constexpr Guid DoublesId{0x41464e, 6};
constexpr Guid GuidsId{0x41464e, 7};
constexpr Guid NamesId{0x41464e, 8};
constexpr Guid AssetsId{0x41464e, 9};
constexpr Guid DigestsId{0x41464e, 10};
constexpr Guid SoftsId{0x41464e, 11};
constexpr Guid ChannelsId{0x41464e, 12};
constexpr Guid GroupsId{0x41464e, 13};
constexpr Guid PointsId{0x41464e, 14};
constexpr Guid TaggedFieldId{0x41464e, 15};
constexpr Guid RefsId{0x41464e, 16};
constexpr Guid SlotsId{0x41464e, 17};
constexpr Guid BundleFieldId{0x41464e, 18};
constexpr Guid SingleId{0x41464e, 19};
constexpr Guid TagIdId{0x41544d, 1};
constexpr Guid LabelId{0x41544d, 2};
constexpr Guid CodesId{0x41544d, 3};
constexpr Guid NotesId{0x41544d, 4};
constexpr Guid MasksId{0x41544d, 5};
constexpr Guid MaterialId{0x41534d, 1};
constexpr Guid OverlayId{0x41534d, 2};
constexpr Guid SlotNameId{0x41534d, 3};
constexpr Guid WeightsId{0x41534d, 4};
constexpr Guid BundleRefsId{0x41424d, 1};
constexpr Guid BundleSlotsId{0x41424d, 2};
constexpr Guid HeavyValueId{0x41484d, 1};
constexpr Guid LinksId{0x414c4b, 1};
constexpr Guid ValuesId{0x414c4b, 2};
constexpr Guid BitsId{0x414c4b, 3};
constexpr Guid ItemsId{0x414c4b, 4};

constexpr uint32 Required = static_cast<uint32>(FieldFlags::Required);

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

// 생성 GeneratedType<S>::Ops와 같은 모양이다(공통 칸은 MakeRuntimeStructOps).
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

// 클래스의 struct·배열 필드 함수표다(생성 코드와 같은 모양 — read·write·writeStaged 없음,
// valueNative는 struct 필드·enum·struct 원소 배열의 NativeTypeKey, 그 밖은 nullptr).
template <auto Member>
RuntimeFieldOps ClassCodecField(Guid id, const void* valueNative = nullptr) noexcept {
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

// 배열을 거친 깊이 시험의 struct 사슬이다: Deep<0>은 문자열 하나, Deep<N>은 Array<Deep<N − 1>>
// 하나(깊이 N + 1).
template <uint32 N>
struct Deep {
    explicit Deep(Allocator& allocator) noexcept : items(allocator) {
    }

    Array<Deep<N - 1>> items;
};

template <>
struct Deep<0> {
    explicit Deep(Allocator& allocator) noexcept : label(allocator) {
    }

    String label;
};

constexpr TypeId DeepId(uint32 level) noexcept {
    return TypeId{0x414450, level + 1};
}

constexpr Guid DeepFieldId{0x41444d, 1};
constexpr uint32 DeepTop = MaxStructDepth - 1;

} // namespace array_fixture

namespace rocks {

template <>
struct GeneratedType<array_fixture::Tagged> {
    static constexpr bool ContainsStrongReference = false;
    static constexpr bool ContainsReferenceArray = false;

    static constexpr TypeId Id() noexcept {
        return array_fixture::TaggedId;
    }

    static const TypeSchema& Schema() noexcept {
        using namespace array_fixture;
        static const FieldSchema fields[] = {
            Scalar(TagIdId, "id", ValueKind::UInt32),
            Scalar(LabelId, "label", ValueKind::Utf8String),
            ArrayOf(CodesId, "codes", ValueKind::UInt32),
            ArrayOf(NotesId, "notes", ValueKind::Utf8String, {}, MaxArrayElements),
            ArrayOf(MasksId, "masks", ValueKind::Enum, TypeIdOf<Channels>())};
        static const TypeSchema schema{Id(), 1, fields, {"Tagged", 6}};
        return schema;
    }

    static void Trace(const array_fixture::Tagged&, TraceVisitor&) noexcept {
    }

    static RuntimeStructOps Ops() noexcept {
        using namespace array_fixture;
        static const RuntimeStructFieldOps fields[] = {
            detail::StructFieldOps<&Tagged::id>(TagIdId),
            detail::StructFieldOps<&Tagged::label>(LabelId),
            detail::StructFieldOps<&Tagged::codes>(CodesId),
            detail::StructFieldOps<&Tagged::notes>(NotesId),
            detail::StructFieldOps<&Tagged::masks>(MasksId)};
        return MakeStructOps<Tagged>(fields);
    }
};

template <>
struct GeneratedType<array_fixture::Slot> {
    static constexpr bool ContainsStrongReference = true;
    static constexpr bool ContainsReferenceArray = false;

    static constexpr TypeId Id() noexcept {
        return array_fixture::SlotId;
    }

    static const TypeSchema& Schema() noexcept {
        using namespace array_fixture;
        static const FieldSchema fields[] = {
            Scalar(MaterialId, "material", ValueKind::ObjectReference, NodeId),
            Scalar(OverlayId, "overlay", ValueKind::ObjectReference),
            Scalar(SlotNameId, "name", ValueKind::Utf8String),
            ArrayOf(WeightsId, "weights", ValueKind::Float32)};
        static const TypeSchema schema{Id(), 1, fields, {"Slot", 4}};
        return schema;
    }

    static void Trace(const array_fixture::Slot& value, TraceVisitor& visitor) noexcept {
        visitor.Visit(value.material);
        visitor.Visit(value.overlay);
        detail::TraceArrayField(value.weights, visitor);
    }

    static RuntimeStructOps Ops() noexcept {
        using namespace array_fixture;
        static const RuntimeStructFieldOps fields[] = {
            detail::StructFieldOps<&Slot::material>(MaterialId),
            detail::StructFieldOps<&Slot::overlay>(OverlayId),
            detail::StructFieldOps<&Slot::name>(SlotNameId),
            detail::StructFieldOps<&Slot::weights>(WeightsId)};
        return MakeStructOps<Slot>(fields);
    }
};

template <>
struct GeneratedType<array_fixture::Bundle> {
    static constexpr bool ContainsStrongReference = true;
    static constexpr bool ContainsReferenceArray = true;

    static constexpr TypeId Id() noexcept {
        return array_fixture::BundleId;
    }

    static const TypeSchema& Schema() noexcept {
        using namespace array_fixture;
        static const FieldSchema fields[] = {
            ArrayOf(BundleRefsId, "refs", ValueKind::ObjectReference, NodeId),
            ArrayOf(BundleSlotsId, "slots", ValueKind::Struct, SlotId)};
        static const TypeSchema schema{Id(), 1, fields, {"Bundle", 6}};
        return schema;
    }

    static void Trace(const array_fixture::Bundle& value, TraceVisitor& visitor) noexcept {
        detail::TraceArrayField(value.refs, visitor);
        detail::TraceArrayField(value.slots, visitor);
    }

    static RuntimeStructOps Ops() noexcept {
        using namespace array_fixture;
        static const RuntimeStructFieldOps fields[] = {
            detail::StructFieldOps<&Bundle::refs>(BundleRefsId),
            detail::StructFieldOps<&Bundle::slots>(BundleSlotsId)};
        return MakeStructOps<Bundle>(fields);
    }
};

template <>
struct GeneratedType<array_fixture::Heavy> {
    static constexpr bool ContainsStrongReference = false;
    static constexpr bool ContainsReferenceArray = false;

    static constexpr TypeId Id() noexcept {
        return array_fixture::HeavyId;
    }

    static const TypeSchema& Schema() noexcept {
        using namespace array_fixture;
        static const FieldSchema fields[] = {Scalar(HeavyValueId, "value", ValueKind::UInt32)};
        static const TypeSchema schema{Id(), 1, fields, {"Heavy", 5}};
        return schema;
    }

    static void Trace(const array_fixture::Heavy&, TraceVisitor&) noexcept {
    }

    static RuntimeStructOps Ops() noexcept {
        using namespace array_fixture;
        static const RuntimeStructFieldOps fields[] = {
            detail::StructFieldOps<&Heavy::value>(HeavyValueId)};
        return MakeStructOps<Heavy>(fields);
    }
};

template <>
struct GeneratedType<array_fixture::Node> {
    static constexpr TypeId Id() noexcept {
        return array_fixture::NodeId;
    }

    static const TypeSchema& Schema() noexcept {
        using namespace array_fixture;
        static const FieldSchema fields[] = {
            ArrayOf(FlagsId, "flags", ValueKind::Bool),
            ArrayOf(BytesId, "bytes", ValueKind::UInt8),
            ArrayOf(ShortsId, "shorts", ValueKind::UInt16),
            ArrayOf(IntsId, "ints", ValueKind::Int32),
            ArrayOf(LongsId, "longs", ValueKind::UInt64),
            ArrayOf(DoublesId, "doubles", ValueKind::Float64),
            ArrayOf(GuidsId, "guids", ValueKind::Guid),
            ArrayOf(NamesId, "names", ValueKind::Utf8String),
            ArrayOf(AssetsId, "assets", ValueKind::AssetId),
            ArrayOf(DigestsId, "digests", ValueKind::Digest256),
            ArrayOf(SoftsId, "softs", ValueKind::SoftObjectReference, NodeId),
            ArrayOf(ChannelsId, "channels", ValueKind::Enum, TypeIdOf<Channels>()),
            ArrayOf(GroupsId, "groups", ValueKind::Enum, TypeIdOf<TextureGroup>()),
            ArrayOf(PointsId, "points", ValueKind::Struct, TypeIdOf<Vec3>()),
            ArrayOf(TaggedFieldId, "tagged", ValueKind::Struct, TaggedId),
            ArrayOf(RefsId, "refs", ValueKind::ObjectReference, NodeId),
            ArrayOf(SlotsId, "slots", ValueKind::Struct, SlotId),
            Scalar(BundleFieldId, "bundle", ValueKind::Struct, BundleId),
            Scalar(SingleId, "single", ValueKind::Struct, TaggedId)};
        static const TypeSchema schema{Id(), 1, fields, {"Node", 4}};
        return schema;
    }

    static RuntimeTypeOps Ops() noexcept {
        using namespace array_fixture;
        static const RuntimeFieldOps fields[] = {
            ClassCodecField<&Node::flags>(FlagsId),
            ClassCodecField<&Node::bytes>(BytesId),
            ClassCodecField<&Node::shorts>(ShortsId),
            ClassCodecField<&Node::ints>(IntsId),
            ClassCodecField<&Node::longs>(LongsId),
            ClassCodecField<&Node::doubles>(DoublesId),
            ClassCodecField<&Node::guids>(GuidsId),
            ClassCodecField<&Node::names>(NamesId),
            ClassCodecField<&Node::assets>(AssetsId),
            ClassCodecField<&Node::digests>(DigestsId),
            ClassCodecField<&Node::softs>(SoftsId),
            ClassCodecField<&Node::channels>(ChannelsId, NativeTypeKey<Channels>()),
            ClassCodecField<&Node::groups>(GroupsId, NativeTypeKey<TextureGroup>()),
            ClassCodecField<&Node::points>(PointsId, NativeTypeKey<Vec3>()),
            ClassCodecField<&Node::tagged>(TaggedFieldId, NativeTypeKey<Tagged>()),
            ClassCodecField<&Node::refs>(RefsId),
            ClassCodecField<&Node::slots>(SlotsId, NativeTypeKey<Slot>()),
            ClassCodecField<&Node::bundle>(BundleFieldId, NativeTypeKey<Bundle>()),
            ClassCodecField<&Node::single>(SingleId, NativeTypeKey<Tagged>())};
        auto ops = MakeRuntimeTypeOps<Node>();
        ops.fields = fields;
        ops.completeFieldLayout = true;
        return ops;
    }
};

template <uint32 N>
struct GeneratedType<array_fixture::Deep<N>> {
    static constexpr bool ContainsStrongReference = false;
    static constexpr bool ContainsReferenceArray = false;

    static constexpr TypeId Id() noexcept {
        return array_fixture::DeepId(N);
    }

    static const TypeSchema& Schema() noexcept {
        using namespace array_fixture;
        static const FieldSchema fields[] = {
            ArrayOf(DeepFieldId, "items", ValueKind::Struct, DeepId(N - 1))};
        static const TypeSchema schema{Id(), 1, fields, {"Deep", 4}};
        return schema;
    }

    static void Trace(const array_fixture::Deep<N>&, TraceVisitor&) noexcept {
    }

    static RuntimeStructOps Ops() noexcept {
        using namespace array_fixture;
        static const RuntimeStructFieldOps fields[] = {
            detail::StructFieldOps<&Deep<N>::items>(DeepFieldId)};
        return MakeStructOps<Deep<N>>(fields);
    }
};

template <>
struct GeneratedType<array_fixture::Deep<0>> {
    static constexpr bool ContainsStrongReference = false;
    static constexpr bool ContainsReferenceArray = false;

    static constexpr TypeId Id() noexcept {
        return array_fixture::DeepId(0);
    }

    static const TypeSchema& Schema() noexcept {
        using namespace array_fixture;
        static const FieldSchema fields[] = {Scalar(DeepFieldId, "label", ValueKind::Utf8String)};
        static const TypeSchema schema{Id(), 1, fields, {"Deep", 4}};
        return schema;
    }

    static void Trace(const array_fixture::Deep<0>&, TraceVisitor&) noexcept {
    }

    static RuntimeStructOps Ops() noexcept {
        using namespace array_fixture;
        static const RuntimeStructFieldOps fields[] = {
            detail::StructFieldOps<&Deep<0>::label>(DeepFieldId)};
        return MakeStructOps<Deep<0>>(fields);
    }
};

} // namespace rocks

void array_fixture::Node::Trace(TraceVisitor& visitor) const noexcept {
    detail::TraceArrayField(flags, visitor);
    detail::TraceArrayField(bytes, visitor);
    detail::TraceArrayField(shorts, visitor);
    detail::TraceArrayField(ints, visitor);
    detail::TraceArrayField(longs, visitor);
    detail::TraceArrayField(doubles, visitor);
    detail::TraceArrayField(guids, visitor);
    detail::TraceArrayField(names, visitor);
    detail::TraceArrayField(assets, visitor);
    detail::TraceArrayField(digests, visitor);
    detail::TraceArrayField(softs, visitor);
    detail::TraceArrayField(channels, visitor);
    detail::TraceArrayField(groups, visitor);
    detail::TraceArrayField(points, visitor);
    detail::TraceArrayField(tagged, visitor);
    detail::TraceArrayField(refs, visitor);
    detail::TraceArrayField(slots, visitor);
    GeneratedType<Bundle>::Trace(bundle, visitor);
    GeneratedType<Tagged>::Trace(single, visitor);
}

namespace {

using namespace array_fixture;

// 원소 struct·참조 대상 형식이 다른 fixture 변형이다. requiredReferences는 Node.refs·Bundle.refs·
// Slot.material을 Required로, bytesMax는 Node.bytes의 maxElements, taggedVersion·taggedMigrate는
// Tagged 버전과 마이그레이션, omit은 Node에서 뺄 필드, skipTaggedApply는 2단계에서 Node.tagged를
// 적용하지 않는 계약 위반 thunk(마이그레이션 커서 어긋남 시험)다.
struct FixtureOptions {
    bool requiredReferences = false;
    uint32 bytesMax = DefaultMaxElements;
    uint32 taggedVersion = 1;
    StructMigration taggedMigrate = nullptr;
    Span<const Guid> omit{};
    bool skipTaggedApply = false;
};

bool Omitted(Span<const Guid> omit, Guid id) noexcept {
    for (Guid candidate : omit) {
        if (candidate == id) {
            return true;
        }
    }

    return false;
}

Result<void> SkipTaggedInApply(const RuntimeFieldOps&, ConstructionContext* construction,
                               ObjectHandle object, const FieldView& field,
                               FieldDecodeContext& context) noexcept {
    if (construction) {
        return Result<void>::Success();
    }

    return detail::DecodeGeneratedField<Node>(nullptr, object, &Node::tagged, field, context);
}

bool RegisterFixture(ModuleRegistration& registration, const FixtureOptions& options) noexcept {
    if (!generated::ReflectionTestFixtures::RegisterModuleTypes(registration)) {
        return false;
    }

    TypeSchema tagged = GeneratedType<Tagged>::Schema();
    tagged.version = options.taggedVersion;
    RuntimeStructOps taggedOps = GeneratedType<Tagged>::Ops();
    taggedOps.migrate = options.taggedMigrate;
    if (!registration.RegisterStructIfAbsent(tagged, Move(taggedOps))) {
        return false;
    }

    FieldSchema slotFields[4];
    for (usize index = 0; index < 4; ++index) {
        slotFields[index] = GeneratedType<Slot>::Schema().fields[index];
    }

    FieldSchema bundleFields[2];
    for (usize index = 0; index < 2; ++index) {
        bundleFields[index] = GeneratedType<Bundle>::Schema().fields[index];
    }

    if (options.requiredReferences) {
        slotFields[0].flags |= Required;
        bundleFields[0].flags |= Required;
    }

    if (!registration.RegisterStructIfAbsent(
            {SlotId, 1, Span<const FieldSchema>(slotFields, 4), {"Slot", 4}},
            GeneratedType<Slot>::Ops()) ||
        !registration.RegisterStructIfAbsent(
            {BundleId, 1, Span<const FieldSchema>(bundleFields, 2), {"Bundle", 6}},
            GeneratedType<Bundle>::Ops()) ||
        !registration.RegisterStructIfAbsent(GeneratedType<Heavy>::Schema(),
                                             GeneratedType<Heavy>::Ops())) {
        return false;
    }

    const TypeSchema& generated = GeneratedType<Node>::Schema();
    const RuntimeTypeOps generatedOps = GeneratedType<Node>::Ops();
    FieldSchema fields[19];
    RuntimeFieldOps ops[19];
    usize count = 0;
    for (usize index = 0; index < generated.fields.Size(); ++index) {
        if (Omitted(options.omit, generated.fields[index].id)) {
            continue;
        }

        fields[count] = generated.fields[index];
        ops[count] = generatedOps.fields[index];
        if (fields[count].id == RefsId && options.requiredReferences) {
            fields[count].flags |= Required;
        }

        if (fields[count].id == BytesId) {
            fields[count].maxElements = options.bytesMax;
        }

        if (fields[count].id == TaggedFieldId && options.skipTaggedApply) {
            ops[count].decode = &SkipTaggedInApply;
        }

        ++count;
    }

    auto runtime = MakeRuntimeTypeOps<Node>();
    runtime.fields = Span<const RuntimeFieldOps>(ops, count);
    runtime.completeFieldLayout = true;
    return static_cast<bool>(registration.RegisterIfAbsent(
        {NodeId, 1, Span<const FieldSchema>(fields, count), {"Node", 4}}, Move(runtime)));
}

// TypeRegistry 하나와 그 fixture 등록이다. 객체 Registry를 모두 Shutdown한 뒤 Close한다.
class Fixture {
public:
    Fixture(Allocator& allocator, const FixtureOptions& options = {}) noexcept
        : types(allocator), registration(types, allocator) {
        ok = RegisterFixture(registration, options);
    }

    bool Close() noexcept {
        return registration.Unregister() && types.Shutdown();
    }

    TypeRegistry types;
    ModuleRegistration registration;
    bool ok = false;
};

// 진단·관찰·치환을 기록한다(경로는 앞 네 단계까지 복사한다).
class ArrayHooks final : public FieldCodecHooks {
public:
    explicit ArrayHooks(Allocator& allocator) noexcept : replacement(allocator) {
    }

    Result<void> Report(const FieldDiagnostic& diagnostic) noexcept override {
        if (count < 16) {
            Entry& entry = entries[count++];
            entry.kind = diagnostic.kind;
            entry.field = diagnostic.field;
            entry.value = diagnostic.value;
            entry.depth = diagnostic.path.Size();
            for (usize step = 0; step < diagnostic.path.Size() && step < 4; ++step) {
                entry.path[step] = diagnostic.path[step];
            }
        }

        return Result<void>::Success();
    }

    usize Count(FieldDiagnosticKind kind) const noexcept {
        usize found = 0;
        for (usize index = 0; index < count; ++index) {
            found += entries[index].kind == kind;
        }

        return found;
    }

    Span<const TypeId> ObservedStructs() noexcept override {
        return observe;
    }

    Result<void> ObserveStruct(const FieldCodecFrame& frame, TypeId type, const void* nativeKey,
                               const void* value) noexcept override {
        if (type == TaggedId && nativeKey == NativeTypeKey<Tagged>() && observed < 8) {
            const Tagged& tagged = *static_cast<const Tagged*>(value);
            observedIndex[observed] = frame.index;
            observedIds[observed] = tagged.id;
            observedCodes[observed] = tagged.codes.Size();
            observedNotes[observed] = tagged.notes.Size();
            ++observed;
        }

        return Result<void>::Success();
    }

    // 원소마다 같은 임시 값을 덮어써 돌려준다 — 반환값은 그 struct 값의 encode가 끝날 때까지만
    // 유효하면 되므로(원소 encode가 끝난 뒤 다음 원소를 치환한다) 올바른 hook이다.
    Result<const void*> SubstituteStruct(const FieldCodecFrame& frame, TypeId type, const void*,
                                         const void* in) noexcept override {
        if (type != TaggedId || !substitute) {
            return Result<const void*>::Success(in);
        }

        replacement.id = frame.index == FieldCodecFrame::NoIndex ? 500 : 100 + frame.index;
        if (substituted < 8) {
            substitutedIndex[substituted++] = frame.index;
        }

        return Result<const void*>::Success(&replacement);
    }

    struct Entry {
        FieldDiagnosticKind kind = FieldDiagnosticKind::UnknownField;
        Guid field{};
        int64 value = 0;
        usize depth = 0;
        PathStep path[4] = {};
    };

    Entry entries[16];
    usize count = 0;
    Span<const TypeId> observe{};
    uint32 observedIndex[8] = {};
    uint32 observedIds[8] = {};
    usize observedCodes[8] = {};
    usize observedNotes[8] = {};
    usize observed = 0;
    bool substitute = false;
    Tagged replacement;
    uint32 substitutedIndex[8] = {};
    usize substituted = 0;
};

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

Result<void> Push(Array<String>& target, const char* text) noexcept {
    auto added = target.TryEmplaceBack(target.GetAllocator());
    if (!added) {
        return Result<void>::Failure(added.Error());
    }

    return added.Value()->TryAssign(text);
}

// 값 배열(참조 없는 부분)을 채운다. small이면 원소를 줄인다.
Result<void> FillValues(Node& node, bool small) noexcept {
    bool ok = node.flags.TryPushBack(true) && node.flags.TryPushBack(false) &&
              node.bytes.TryPushBack(uint8{0}) && node.bytes.TryPushBack(uint8{7}) &&
              node.bytes.TryPushBack(uint8{255}) && node.shorts.TryPushBack(uint16{65535}) &&
              node.ints.TryPushBack(int32{-2147483647 - 1}) && node.ints.TryPushBack(int32{5}) &&
              node.longs.TryPushBack(~uint64{0}) && node.doubles.TryPushBack(1.5) &&
              node.guids.TryPushBack(Guid{1, 2}) && node.guids.TryPushBack(Guid{}) &&
              node.assets.TryPushBack(AssetId{Guid{9, 9}}) &&
              node.channels.TryPushBack(static_cast<Channels>(5)) &&
              node.channels.TryPushBack(Channels::Alpha) &&
              node.channels.TryPushBack(Channels::None) &&
              node.groups.TryPushBack(TextureGroup::EightBitData) &&
              node.points.TryPushBack(Vec3(1.5f, -2.0f, 0.25f)) &&
              node.points.TryPushBack(Vec3()) && Push(node.names, "") && Push(node.names, "배열") &&
              Push(node.single.notes, "x") && node.single.codes.TryPushBack(uint32{9});
    if (!ok) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    node.single.id = 7;
    Sha256Digest digest;
    for (usize index = 0; index < Sha256Digest::Bytes; ++index) {
        digest.bytes[index] = static_cast<uint8>(index * 3);
    }

    if (!node.digests.TryPushBack(digest)) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    // 소프트 참조: null, 경로 있는 값, 에셋만 있는 값.
    for (usize index = 0; index < 3; ++index) {
        auto added = node.softs.TryEmplaceBack(node.softs.GetAllocator());
        if (!added) {
            return Result<void>::Failure(added.Error());
        }

        if (index == 1) {
            if (auto set = added.Value()->TryAssign(AssetId{Guid{5, 5}}, "Mesh.Socket"); !set) {
                return set;
            }
        } else if (index == 2) {
            added.Value()->asset = AssetId{Guid{6, 6}};
        }
    }

    // struct 원소(원소 struct 안 배열 포함).
    const usize taggedCount = small ? 1 : 2;
    for (usize index = 0; index < taggedCount; ++index) {
        auto added = node.tagged.TryEmplaceBack(node.tagged.GetAllocator());
        if (!added) {
            return Result<void>::Failure(added.Error());
        }

        Tagged& tagged = *added.Value();
        tagged.id = static_cast<uint32>(index + 1);
        if (index == 0 &&
            (!tagged.label.TryAssign("첫째") || !tagged.codes.TryPushBack(uint32{1}) ||
             !tagged.codes.TryPushBack(uint32{2}) || !tagged.codes.TryPushBack(uint32{3}) ||
             !Push(tagged.notes, "a") || !tagged.masks.TryPushBack(Channels::Red) ||
             !tagged.masks.TryPushBack(Channels::Blue))) {
            return Result<void>::Failure(ErrorCode::OutOfMemory);
        }
    }

    return Result<void>::Success();
}

// A(값을 모두 채움)와 B를 만든다. A.refs = [B, null, A], A.slots = [{B, A, "s0", [0.5]}, {null,
// B}], A.bundle = {refs [null, B], slots [{A, null, "b0", [1, 2]}]}, B.refs = [A]. A의 root를
// 돌려준다.
Result<RootRef<Object>> MakeGraph(ObjectRegistry& objects, bool small = false) noexcept {
    ConstructionContext transaction(objects, objects.OwnerAccess());
    auto a = transaction.Create<Node>(NodeId);
    auto b = transaction.Create<Node>(NodeId);
    if (!a || !b) {
        return Result<RootRef<Object>>::Failure(!a ? a.Error() : b.Error());
    }

    auto rootNode = transaction.Get<Node>(a.Value());
    auto otherNode = transaction.Get<Node>(b.Value());
    if (!rootNode || !otherNode) {
        return Result<RootRef<Object>>::Failure(ErrorCode::InvalidArgument);
    }

    Node& root = *rootNode.Value();
    if (auto filled = FillValues(root, small); !filled) {
        return Result<RootRef<Object>>::Failure(filled.Error());
    }

    const ObjectHandle refs[] = {b.Value(), ObjectHandle{}, a.Value()};
    const ObjectHandle bundleRefs[] = {ObjectHandle{}, b.Value()};
    const ObjectHandle back[] = {a.Value()};
    bool ok = transaction.AssignReferences(a.Value(), root.refs, refs) &&
              transaction.AssignReferences(a.Value(), root.bundle.refs, bundleRefs) &&
              transaction.AssignReferences(b.Value(), otherNode.Value()->refs, back) &&
              transaction.AssignElements(a.Value(), root.slots, small ? 1 : 2) &&
              transaction.AssignElements(a.Value(), root.bundle.slots, 1);
    if (!ok) {
        return Result<RootRef<Object>>::Failure(ErrorCode::OutOfMemory);
    }

    ok = root.slots[0].name.TryAssign("s0") && root.slots[0].weights.TryPushBack(0.5f) &&
         root.bundle.slots[0].name.TryAssign("b0") &&
         root.bundle.slots[0].weights.TryPushBack(1.0f) &&
         root.bundle.slots[0].weights.TryPushBack(2.0f) &&
         transaction.SetElementReference(a.Value(), root.slots, 0, root.slots[0].material,
                                         b.Value()) &&
         transaction.SetElementReference(a.Value(), root.slots, 0, root.slots[0].overlay,
                                         a.Value()) &&
         transaction.SetElementReference(a.Value(), root.bundle.slots, 0,
                                         root.bundle.slots[0].material, a.Value());
    if (!ok || (!small && !transaction.SetElementReference(a.Value(), root.slots, 1,
                                                           root.slots[1].overlay, b.Value()))) {
        return Result<RootRef<Object>>::Failure(ErrorCode::OutOfMemory);
    }

    return transaction.Commit();
}

Result<Array<uint8>> SaveGraph(Allocator& allocator, ObjectRegistry& objects,
                               ObjectGraphSaveOptions options = {}, bool small = false) noexcept {
    auto root = MakeGraph(objects, small);
    if (!root) {
        return Result<Array<uint8>>::Failure(root.Error());
    }

    return SaveObjectGraph(allocator, objects, objects.OwnerAccess(), root.Value().Handle(),
                           options);
}

// A의 값이 MakeGraph가 넣은 그대로인지(참조는 self = A, other = B) 본다.
bool CheckFilled(const Node& node, ObjectHandle self, ObjectHandle other) noexcept {
    bool ok =
        node.flags.Size() == 2 && node.flags[0] && !node.flags[1] && node.bytes.Size() == 3 &&
        node.bytes[2] == 255 && node.shorts.Size() == 1 && node.shorts[0] == 65535 &&
        node.ints.Size() == 2 && node.ints[0] == -2147483647 - 1 && node.ints[1] == 5 &&
        node.longs.Size() == 1 && node.longs[0] == ~uint64{0} && node.doubles.Size() == 1 &&
        node.doubles[0] == 1.5 && node.guids.Size() == 2 && node.guids[0] == (Guid{1, 2}) &&
        node.guids[1] == Guid{} && node.names.Size() == 2 &&
        node.names[0].View() == StringView("") && node.names[1].View() == StringView("배열") &&
        node.assets.Size() == 1 && node.assets[0] == AssetId{Guid{9, 9}} &&
        node.digests.Size() == 1 && node.digests[0].bytes[31] == 93 && node.softs.Size() == 3 &&
        node.softs[0].IsNull() && node.softs[1].asset == AssetId{Guid{5, 5}} &&
        node.softs[1].objectPath.View() == StringView("Mesh.Socket") &&
        node.softs[2].asset == AssetId{Guid{6, 6}} && node.softs[2].objectPath.ByteCount() == 0 &&
        node.channels.Size() == 3 && static_cast<uint32>(node.channels[0]) == 5 &&
        node.channels[1] == Channels::Alpha && node.channels[2] == Channels::None &&
        node.groups.Size() == 1 && node.groups[0] == TextureGroup::EightBitData &&
        node.points.Size() == 2 && node.points[0].x == 1.5f && node.points[0].y == -2.0f &&
        node.points[0].z == 0.25f && node.points[1].x == 0.0f;
    ok = ok && node.tagged.Size() == 2 && node.tagged[0].id == 1 &&
         node.tagged[0].label.View() == StringView("첫째") && node.tagged[0].codes.Size() == 3 &&
         node.tagged[0].codes[2] == 3 && node.tagged[0].notes.Size() == 1 &&
         node.tagged[0].notes[0].View() == StringView("a") && node.tagged[0].masks.Size() == 2 &&
         node.tagged[0].masks[1] == Channels::Blue && node.tagged[1].id == 2 &&
         node.tagged[1].codes.IsEmpty() && node.single.id == 7 && node.single.codes.Size() == 1 &&
         node.single.codes[0] == 9 && node.single.notes.Size() == 1 &&
         node.single.notes[0].View() == StringView("x");
    ok = ok && node.refs.Size() == 3 && node.refs[0] == other && node.refs[1] == ObjectHandle{} &&
         node.refs[2] == self && node.slots.Size() == 2 &&
         node.slots[0].material.Handle() == other && node.slots[0].overlay.Handle() == self &&
         node.slots[0].name.View() == StringView("s0") && node.slots[0].weights.Size() == 1 &&
         node.slots[0].weights[0] == 0.5f && node.slots[1].material.Handle() == ObjectHandle{} &&
         node.slots[1].overlay.Handle() == other && node.slots[1].weights.IsEmpty();
    ok = ok && node.bundle.refs.Size() == 2 && node.bundle.refs[0] == ObjectHandle{} &&
         node.bundle.refs[1] == other && node.bundle.slots.Size() == 1 &&
         node.bundle.slots[0].material.Handle() == self &&
         node.bundle.slots[0].overlay.Handle() == ObjectHandle{} &&
         node.bundle.slots[0].name.View() == StringView("b0") &&
         node.bundle.slots[0].weights.Size() == 2 && node.bundle.slots[0].weights[1] == 2.0f;
    return ok;
}

uint64 Read64(Span<const uint8> bytes, usize at) noexcept {
    uint64 value = 0;
    for (usize index = 0; index < 8; ++index) {
        value |= uint64{bytes[at + index]} << (8 * index);
    }

    return value;
}

uint32 Read32(Span<const uint8> bytes, usize at) noexcept {
    uint32 value = 0;
    for (usize index = 0; index < 4; ++index) {
        value |= uint32{bytes[at + index]} << (8 * index);
    }

    return value;
}

void Write64(Array<uint8>& bytes, usize at, uint64 value) noexcept {
    for (usize index = 0; index < 8; ++index) {
        bytes[at + index] = static_cast<uint8>(value >> (8 * index));
    }
}

void Write32(Array<uint8>& bytes, usize at, uint32 value) noexcept {
    for (usize index = 0; index < 4; ++index) {
        bytes[at + index] = static_cast<uint8>(value >> (8 * index));
    }
}

// 저장 byte에서 필드 ID·wire의 첫 TLV payload 위치다(없으면 ~0).
usize FindPayload(Span<const uint8> bytes, Guid field, WireType wire) noexcept {
    for (usize at = 0; at + 28 <= bytes.Size(); ++at) {
        if (Read64(bytes, at) == field.high && Read64(bytes, at + 8) == field.low &&
            Read32(bytes, at + 16) == static_cast<uint32>(wire)) {
            return at + 28;
        }
    }

    return ~usize(0);
}

// 로드 결과 root가 가리키는 Node를 빌려 검사한다.
template <class Check>
bool WithRoot(ObjectRegistry& objects, ObjectHandle root, Check&& check) noexcept {
    auto borrowed = objects.Resolve<Node>(objects.OwnerAccess(), root);
    if (!borrowed) {
        return false;
    }

    auto node = borrowed.Value().Get();
    return node && check(*node.Value());
}

// 할당을 세고 가장 큰 단일 할당을 기록한다.
class TrackingAllocator final : public Allocator {
public:
    explicit TrackingAllocator(Allocator& backing) noexcept : backing_(backing) {
    }

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override {
        ++allocations;
        total += bytes;
        largest = bytes > largest ? bytes : largest;
        return backing_.Allocate(bytes, alignment, tag);
    }

    Result<void> Deallocate(void* address) noexcept override {
        return backing_.Deallocate(address);
    }

    void Reset() noexcept {
        allocations = 0;
        total = 0;
        largest = 0;
    }

    usize allocations = 0;
    usize total = 0;
    usize largest = 0;

private:
    Allocator& backing_;
};

// Tagged v1 → v2 마이그레이션이다(필드를 그대로 옮기고 버전만 올린다). 호출 수를 센다.
uint32 TaggedMigrations = 0;

Result<Array<uint8>> MigrateTagged(Allocator& allocator, TypeId type, uint32 from, uint32 to,
                                   Span<const uint8> source) noexcept {
    ++TaggedMigrations;
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

} // namespace

// 모든 원소 종류(bool·uint8·uint16·int32·uint64·double·Guid·String·AssetId·Digest·null과 non-null
// Soft·Flags enum·일반 enum·Binds Vec3·원소 struct 안 배열)와 null 원소를 섞은 ObjectRefArray,
// StructRefArray, 인라인 struct 안 ObjectRefArray·StructRefArray, struct 안 배열이 그대로 돌아오고
// 다른 Registry에서 다시 저장한 byte가 같다. 순환 참조 배열(A ↔ B, A → A)이 GC 수집에서 살아남고
// root를 놓으면 모두 회수된다.
TEST(ArrayGraph_EveryElementKindRoundTripsAndResavesIdentically) {
    SystemAllocator allocator;
    Fixture fixture(allocator);
    REQUIRE(fixture.ok);
    {
        ObjectRegistry source(allocator, fixture.types, {0x4147, 1});
        ObjectRegistry target(allocator, fixture.types, {0x4147, 2});
        {
            auto bytes = SaveGraph(allocator, source);
            REQUIRE(bytes);
            auto loaded = LoadObjectGraph(allocator, target, fixture.types, target.OwnerAccess(),
                                          bytes.Value().View());
            REQUIRE(loaded);
            const ObjectHandle self = loaded.Value().Handle();
            ObjectHandle other{};
            CHECK(WithRoot(target, self, [&](const Node& node) noexcept {
                if (node.refs.Size() == 0) {
                    return false;
                }

                other = node.refs[0];
                return CheckFilled(node, self, other);
            }));
            CHECK(WithRoot(target, other, [&](const Node& node) noexcept {
                return node.refs.Size() == 1 && node.refs[0] == self && node.slots.IsEmpty() &&
                       node.tagged.IsEmpty() && node.single.codes.IsEmpty();
            }));

            // 순환 배열 간선과 원소 struct 안 참조가 Trace로 방문되어 수집에서 산다.
            REQUIRE(target.Collect(target.OwnerAccess()));
            CHECK(target.Stats().liveObjects == 2);
            auto resaved = SaveObjectGraph(allocator, target, target.OwnerAccess(), self);
            REQUIRE(resaved);
            CHECK(SameBytes(bytes.Value().View(), resaved.Value().View()));
            loaded.Value().Reset();
            REQUIRE(target.Collect(target.OwnerAccess()));
            CHECK(target.Stats().liveObjects == 0);
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(target.Shutdown(target.OwnerAccess()));
    }

    CHECK(fixture.Close());
}

// Required 배열(원소마다 non-null): 저장은 rejectMissingRequired면 첫 null 원소에서 경로
// [refs[1]]의 RequiredMissing 뒤 InvalidArgument, 로드는 진단만 낸다 — [refs[1]], [slots[1],
// material](원소 struct 멤버), [bundle, refs[0]](인라인 struct 안 배열). 미선언 enum 원소는 기본
// 거부(생성 전), ReportAndDefault면 경로 [channels[1]]의 진단 뒤 대체 값이다.
TEST(ArrayGraph_RequiredNullElementsAndElementDiagnosticPaths) {
    SystemAllocator allocator;
    FixtureOptions required;
    required.requiredReferences = true;
    Fixture fixture(allocator, required);
    REQUIRE(fixture.ok);
    {
        ObjectRegistry source(allocator, fixture.types, {0x4147, 3});
        ObjectRegistry target(allocator, fixture.types, {0x4147, 4});
        {
            ArrayHooks saveHooks(allocator);
            ObjectGraphSaveOptions strict;
            strict.policy.rejectMissingRequired = true;
            strict.hooks = &saveHooks;
            CHECK(SaveGraph(allocator, source, strict).Error() == ErrorCode::InvalidArgument);
            REQUIRE(saveHooks.count == 1);
            CHECK(saveHooks.entries[0].kind == FieldDiagnosticKind::RequiredMissing);
            CHECK(saveHooks.entries[0].depth == 1);
            CHECK(saveHooks.entries[0].path[0].fieldId == RefsId);
            CHECK(saveHooks.entries[0].path[0].indexed && saveHooks.entries[0].path[0].index == 1);

            auto bytes = SaveGraph(allocator, source);
            REQUIRE(bytes);
            ArrayHooks hooks(allocator);
            ObjectGraphLoadOptions load;
            load.hooks = &hooks;
            CHECK(LoadObjectGraph(allocator, target, fixture.types, target.OwnerAccess(),
                                  bytes.Value().View(), load));
            CHECK(hooks.Count(FieldDiagnosticKind::RequiredMissing) == 3);
            bool refs = false;
            bool slot = false;
            bool bundle = false;
            for (usize index = 0; index < hooks.count; ++index) {
                const ArrayHooks::Entry& entry = hooks.entries[index];
                refs = refs || (entry.depth == 1 && entry.path[0].fieldId == RefsId &&
                                entry.path[0].indexed && entry.path[0].index == 1);
                slot = slot || (entry.depth == 2 && entry.path[0].fieldId == SlotsId &&
                                entry.path[0].indexed && entry.path[0].index == 1 &&
                                entry.path[1].fieldId == MaterialId && !entry.path[1].indexed);
                bundle =
                    bundle || (entry.depth == 2 && entry.path[0].fieldId == BundleFieldId &&
                               !entry.path[0].indexed && entry.path[1].fieldId == BundleRefsId &&
                               entry.path[1].indexed && entry.path[1].index == 0);
            }

            CHECK(refs && slot && bundle);

            // 미선언 enum 원소(Flags enum에 없는 비트 16).
            const usize channels =
                FindPayload(bytes.Value().View(), ChannelsId, WireType::ValueArray);
            REQUIRE(channels != ~usize(0));
            REQUIRE(Read32(bytes.Value().View(), channels + 4) == 3);
            Write64(bytes.Value(), channels + 8 + 8, 16);
            const uint32 constructed = Node::Constructed;
            CHECK(LoadObjectGraph(allocator, target, fixture.types, target.OwnerAccess(),
                                  bytes.Value().View())
                      .Error() == ErrorCode::InvalidArgument);
            CHECK(Node::Constructed == constructed);
            ArrayHooks enumHooks(allocator);
            ObjectGraphLoadOptions report;
            report.hooks = &enumHooks;
            report.policy.undeclaredEnum = UndeclaredEnumPolicy::ReportAndDefault;
            auto loaded = LoadObjectGraph(allocator, target, fixture.types, target.OwnerAccess(),
                                          bytes.Value().View(), report);
            REQUIRE(loaded);
            REQUIRE(enumHooks.Count(FieldDiagnosticKind::UndeclaredEnumValue) == 1);
            for (usize index = 0; index < enumHooks.count; ++index) {
                const ArrayHooks::Entry& entry = enumHooks.entries[index];
                if (entry.kind == FieldDiagnosticKind::UndeclaredEnumValue) {
                    CHECK(entry.value == 16);
                    CHECK(entry.depth == 1 && entry.path[0].fieldId == ChannelsId);
                    CHECK(entry.path[0].indexed && entry.path[0].index == 1);
                }
            }

            CHECK(WithRoot(target, loaded.Value().Handle(), [](const Node& node) noexcept {
                return node.channels.Size() == 3 && node.channels[1] == Channels::None;
            }));
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(target.Shutdown(target.OwnerAccess()));
    }

    CHECK(fixture.Close());
}

// 누락된 배열 필드는 생성자 값을 유지하며 Required여도 진단하지 않는다. 누락된 struct(bundle)
// 안의 Required 배열 멤버(refs)도 진단하지 않는다(누락 struct 안 배열은 생성자 값이다).
TEST(ArrayGraph_MissingArraysKeepConstructorValuesWithoutDiagnostics) {
    SystemAllocator allocator;
    const Guid omitted[] = {RefsId, SlotsId, BundleFieldId};
    FixtureOptions sparse;
    sparse.omit = omitted;
    Fixture sourceFixture(allocator, sparse);
    FixtureOptions required;
    required.requiredReferences = true;
    Fixture targetFixture(allocator, required);
    REQUIRE(sourceFixture.ok);
    REQUIRE(targetFixture.ok);
    {
        ObjectRegistry source(allocator, sourceFixture.types, {0x4147, 5});
        ObjectRegistry target(allocator, targetFixture.types, {0x4147, 6});
        {
            auto root = source.NewObject<Node>(source.OwnerAccess(), NodeId);
            REQUIRE(root);
            auto bytes =
                SaveObjectGraph(allocator, source, source.OwnerAccess(), root.Value().Handle());
            REQUIRE(bytes);
            CHECK(LoadObjectGraph(allocator, target, targetFixture.types, target.OwnerAccess(),
                                  bytes.Value().View())
                      .Error() == ErrorCode::NotFound);
            ArrayHooks hooks(allocator);
            ObjectGraphLoadOptions allow;
            allow.policy.allowMissingFields = true;
            allow.hooks = &hooks;
            auto loaded = LoadObjectGraph(allocator, target, targetFixture.types,
                                          target.OwnerAccess(), bytes.Value().View(), allow);
            REQUIRE(loaded);
            CHECK(hooks.count == 0);
            CHECK(WithRoot(target, loaded.Value().Handle(), [](const Node& node) noexcept {
                return node.refs.IsEmpty() && node.slots.IsEmpty() && node.bundle.refs.IsEmpty() &&
                       node.bundle.slots.IsEmpty();
            }));
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(target.Shutdown(target.OwnerAccess()));
    }

    CHECK(sourceFixture.Close());
    CHECK(targetFixture.Close());
}

// struct 원소 마이그레이션: 낮은 버전 원소마다(배열 원소·단일 struct 모두) 한 번 — 2단계는 보관본을
// 방문 순서대로 다시 쓴다(호출 수가 1단계 등장 수와 같다). 관찰도 마이그레이션된 원소를 한 번 본다.
// 2단계가 원소를 덜 방문하면(계약 위반 thunk) 남은 보관본 때문에 InvalidArgument로 rollback된다.
TEST(ArrayGraph_StructElementMigrationRunsOncePerElement) {
    SystemAllocator allocator;
    Fixture oldFixture(allocator);
    FixtureOptions newer;
    newer.taggedVersion = 2;
    newer.taggedMigrate = &MigrateTagged;
    Fixture newFixture(allocator, newer);
    FixtureOptions broken = newer;
    broken.skipTaggedApply = true;
    Fixture brokenFixture(allocator, broken);
    REQUIRE(oldFixture.ok && newFixture.ok && brokenFixture.ok);
    {
        ObjectRegistry source(allocator, oldFixture.types, {0x4147, 7});
        ObjectRegistry target(allocator, newFixture.types, {0x4147, 8});
        ObjectRegistry brokenTarget(allocator, brokenFixture.types, {0x4147, 9});
        {
            auto bytes = SaveGraph(allocator, source);
            REQUIRE(bytes);
            const TypeId observedIds[] = {TaggedId};
            ArrayHooks hooks(allocator);
            hooks.observe = observedIds;
            ObjectGraphLoadOptions load;
            load.hooks = &hooks;
            TaggedMigrations = 0;
            auto loaded = LoadObjectGraph(allocator, target, newFixture.types, target.OwnerAccess(),
                                          bytes.Value().View(), load);
            REQUIRE(loaded);
            // A의 tagged 원소 둘과 single, B의 single.
            CHECK(TaggedMigrations == 4);
            CHECK(hooks.observed == 4);
            CHECK(hooks.observedIndex[0] == 0 && hooks.observedIndex[1] == 1);
            CHECK(hooks.observedIndex[2] == FieldCodecFrame::NoIndex);
            CHECK(WithRoot(target, loaded.Value().Handle(), [](const Node& node) noexcept {
                return node.tagged.Size() == 2 && node.tagged[0].codes.Size() == 3 &&
                       node.tagged[0].label.View() == StringView("첫째") && node.single.id == 7;
            }));

            const uint32 constructed = Node::Constructed;
            const uint32 live = brokenTarget.Stats().liveObjects;
            TaggedMigrations = 0;
            CHECK(LoadObjectGraph(allocator, brokenTarget, brokenFixture.types,
                                  brokenTarget.OwnerAccess(), bytes.Value().View())
                      .Error() == ErrorCode::InvalidArgument);
            CHECK(TaggedMigrations == 4);
            CHECK(Node::Constructed == constructed + 2);
            CHECK(brokenTarget.Stats().liveObjects == live);
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(target.Shutdown(target.OwnerAccess()));
        REQUIRE(brokenTarget.Shutdown(brokenTarget.OwnerAccess()));
    }

    CHECK(oldFixture.Close());
    CHECK(newFixture.Close());
    CHECK(brokenFixture.Close());
}

// 관찰: 관찰 대상 원소 struct는 원소마다 한 번의 해석으로 채워지고(frame index = 원소) 그 안의 배열
// 멤버도 채워진다 — 원소 버퍼가 maxValueBytes(BudgetAllocator의 단일 할당 한도)보다 커도
// ObservationScratch로 관찰된다. 치환: 원소마다 index 프레임에서 불리고, 같은 임시 값을 원소마다
// 덮어쓰는 hook도 올바르다(반환값은 그 원소 encode 동안만 유효하면 된다).
TEST(ArrayGraph_ObservationFillsArrayMembersAndSubstitutionSeesElementIndex) {
    SystemAllocator allocator;
    Fixture fixture(allocator);
    REQUIRE(fixture.ok);
    {
        ObjectRegistry source(allocator, fixture.types, {0x4147, 10});
        ObjectRegistry target(allocator, fixture.types, {0x4147, 11});
        constexpr usize ManyNotes = 40000;
        {
            ConstructionContext transaction(source, source.OwnerAccess());
            auto created = transaction.Create<Node>(NodeId);
            REQUIRE(created);
            auto node = transaction.Get<Node>(created.Value());
            REQUIRE(node);
            REQUIRE(FillValues(*node.Value(), false));
            REQUIRE(node.Value()->single.notes.TryReserve(ManyNotes));
            for (usize index = node.Value()->single.notes.Size(); index < ManyNotes; ++index) {
                REQUIRE(Push(node.Value()->single.notes, ""));
            }

            auto root = transaction.Commit();
            REQUIRE(root);
            auto bytes =
                SaveObjectGraph(allocator, source, source.OwnerAccess(), root.Value().Handle());
            REQUIRE(bytes);

            const TypeId observedIds[] = {TaggedId};
            ArrayHooks hooks(allocator);
            hooks.observe = observedIds;
            ObjectGraphLoadOptions load;
            load.hooks = &hooks;
            // 원소 버퍼(ManyNotes × sizeof(String))가 단일 할당 한도보다 크다.
            load.limits.maxValueBytes = 1024 * 1024;
            REQUIRE(ManyNotes * sizeof(String) > load.limits.maxValueBytes);
            CHECK(LoadObjectGraph(allocator, target, fixture.types, target.OwnerAccess(),
                                  bytes.Value().View(), load));
            REQUIRE(hooks.observed == 3);
            CHECK(hooks.observedIndex[0] == 0 && hooks.observedIds[0] == 1);
            CHECK(hooks.observedCodes[0] == 3 && hooks.observedNotes[0] == 1);
            CHECK(hooks.observedIndex[1] == 1 && hooks.observedIds[1] == 2);
            CHECK(hooks.observedIndex[2] == FieldCodecFrame::NoIndex);
            CHECK(hooks.observedIds[2] == 7 && hooks.observedNotes[2] == ManyNotes);

            ArrayHooks substitute(allocator);
            substitute.substitute = true;
            ObjectGraphSaveOptions save;
            save.hooks = &substitute;
            auto substituted = SaveObjectGraph(allocator, source, source.OwnerAccess(),
                                               root.Value().Handle(), save);
            REQUIRE(substituted);
            REQUIRE(substitute.substituted == 3);
            CHECK(substitute.substitutedIndex[0] == 0 && substitute.substitutedIndex[1] == 1);
            CHECK(substitute.substitutedIndex[2] == FieldCodecFrame::NoIndex);
            auto loaded = LoadObjectGraph(allocator, target, fixture.types, target.OwnerAccess(),
                                          substituted.Value().View());
            REQUIRE(loaded);
            CHECK(WithRoot(target, loaded.Value().Handle(), [](const Node& node) noexcept {
                return node.tagged.Size() == 2 && node.tagged[0].id == 100 &&
                       node.tagged[1].id == 101 && node.tagged[0].codes.IsEmpty() &&
                       node.single.id == 500 && node.single.notes.IsEmpty();
            }));
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(target.Shutdown(target.OwnerAccess()));
    }

    CHECK(fixture.Close());
}

namespace {

// 한도 하나(원소·필드 누계, byte·객체·마이그레이션 한도)를 0부터 올려 처음 성공하는 값을 찾는다
// (저장·로드 각각, last까지). 실패는 모두 CapacityExceeded이고 로드 실패는 생성자 전이다. base는
// 다른 로드 옵션(마이그레이션 등)이다.
template <class Configure>
uint32 FirstLoadLimit(Allocator& allocator, ObjectRegistry& target, TypeRegistry& types,
                      Span<const uint8> bytes, Configure&& configure, bool& clean,
                      uint32 last = 4096, const ObjectGraphLoadOptions& base = {}) noexcept {
    for (uint32 limit = 0; limit < last; ++limit) {
        ObjectGraphLoadOptions options = base;
        configure(options.limits, limit);
        const uint32 constructed = Node::Constructed;
        auto loaded =
            LoadObjectGraph(allocator, target, types, target.OwnerAccess(), bytes, options);
        if (loaded) {
            return limit;
        }

        clean = clean && loaded.Error() == ErrorCode::CapacityExceeded &&
                Node::Constructed == constructed;
    }

    clean = false;
    return ~0u;
}

template <class Configure>
uint32 FirstSaveLimit(Allocator& allocator, ObjectRegistry& source, ObjectHandle root,
                      Configure&& configure, bool& clean) noexcept {
    for (uint32 limit = 0; limit < 4096; ++limit) {
        ObjectGraphSaveOptions options;
        configure(options.limits, limit);
        auto saved = SaveObjectGraph(allocator, source, source.OwnerAccess(), root, options);
        if (saved) {
            return limit;
        }

        clean = clean && saved.Error() == ErrorCode::CapacityExceeded;
    }

    clean = false;
    return ~0u;
}

// 배열 필드 하나짜리 보조 클래스(링크·넓은 값·큰 원소·깊은 사슬)다.
struct Hub final : Object {
    explicit Hub(Allocator& allocator) noexcept : links(allocator) {
    }

    void Trace(TraceVisitor& visitor) const noexcept {
        detail::TraceArrayField(links, visitor);
    }

    ObjectRefArray<Hub> links;
};

struct Loose final : Object {
    explicit Loose(Allocator& allocator) noexcept : links(allocator) {
    }

    void Trace(TraceVisitor& visitor) const noexcept {
        detail::TraceArrayField(links, visitor);
    }

    ObjectRefArray<Object> links;
};

struct Strict final : Object {
    explicit Strict(Allocator& allocator) noexcept : links(allocator) {
    }

    void Trace(TraceVisitor& visitor) const noexcept {
        detail::TraceArrayField(links, visitor);
    }

    ObjectRefArray<Hub> links;
};

struct Other final : Object {};

struct Wide final : Object {
    explicit Wide(Allocator& allocator) noexcept : values(allocator), bits(allocator) {
        ++Constructed;
    }

    void Trace(TraceVisitor& visitor) const noexcept {
        detail::TraceArrayField(values, visitor);
        detail::TraceArrayField(bits, visitor);
    }

    static inline uint32 Constructed = 0;
    Array<uint64> values;
    Array<bool> bits;
};

struct HeavyHolder final : Object {
    explicit HeavyHolder(Allocator& allocator) noexcept : items(allocator) {
        ++Constructed;
    }

    void Trace(TraceVisitor& visitor) const noexcept {
        detail::TraceArrayField(items, visitor);
    }

    static inline uint32 Constructed = 0;
    Array<Heavy> items;
};

struct DeepHolder final : Object {
    explicit DeepHolder(Allocator& allocator) noexcept : chain(allocator) {
    }

    void Trace(TraceVisitor& visitor) const noexcept {
        detail::TraceArrayField(chain, visitor);
    }

    Array<Deep<DeepTop>> chain;
};

// 필드 하나짜리 클래스를 등록한다(fields·ops는 0개 또는 1개).
template <class C>
Result<TypeRegistrationToken> RegisterClass(TypeRegistry& types, TypeId id, const char* name,
                                            Span<const FieldSchema> fields,
                                            Span<const RuntimeFieldOps> ops) noexcept {
    auto runtime = MakeRuntimeTypeOps<C>();
    runtime.fields = ops;
    runtime.completeFieldLayout = true;
    return types.Register({id, 1, fields, Text(name)}, Move(runtime));
}

// Deep<0>…Deep<N>을 등록하고(멤버 struct 먼저), 원소를 하나씩 더해 맨 아래 문자열까지 채운다.
template <uint32 N>
struct DeepChain {
    static bool Register(TypeRegistry& types) noexcept {
        return DeepChain<N - 1>::Register(types) &&
               types.RegisterStruct(GeneratedType<Deep<N>>::Schema(),
                                    GeneratedType<Deep<N>>::Ops());
    }

    static bool Fill(Deep<N>& level, StringView label) noexcept {
        auto added = level.items.TryEmplaceBack(level.items.GetAllocator());
        return added && DeepChain<N - 1>::Fill(*added.Value(), label);
    }
};

template <>
struct DeepChain<0> {
    static bool Register(TypeRegistry& types) noexcept {
        return static_cast<bool>(
            types.RegisterStruct(GeneratedType<Deep<0>>::Schema(), GeneratedType<Deep<0>>::Ops()));
    }

    static bool Fill(Deep<0>& level, StringView label) noexcept {
        return static_cast<bool>(level.label.TryAssign(label));
    }
};

} // namespace

// 길이 한도: 필드별 MaxElements는 저장에서만 건다(낮춘 Registry도 기존 데이터를 열고 다시 저장할 때
// CapacityExceeded). 원소 누계(maxTotalElements)와 struct 원소의 필드 누계(maxTotalFields)는 저장과
// 로드가 같은 값에서 처음 성공하고 그보다 1 작으면 실패한다(로드는 생성자 전).
TEST(ArrayGraph_PerFieldMaxElementsAndGraphTotals) {
    SystemAllocator allocator;
    Fixture wideFixture(allocator);
    FixtureOptions narrow;
    narrow.bytesMax = 2;
    Fixture narrowFixture(allocator, narrow);
    REQUIRE(wideFixture.ok && narrowFixture.ok);
    {
        ObjectRegistry source(allocator, wideFixture.types, {0x4147, 12});
        ObjectRegistry target(allocator, narrowFixture.types, {0x4147, 13});
        {
            auto root = MakeGraph(source);
            REQUIRE(root);
            auto bytes =
                SaveObjectGraph(allocator, source, source.OwnerAccess(), root.Value().Handle());
            REQUIRE(bytes);
            auto loaded = LoadObjectGraph(allocator, target, narrowFixture.types,
                                          target.OwnerAccess(), bytes.Value().View());
            REQUIRE(loaded);
            CHECK(WithRoot(target, loaded.Value().Handle(),
                           [](const Node& node) noexcept { return node.bytes.Size() == 3; }));
            CHECK(SaveObjectGraph(allocator, target, target.OwnerAccess(), loaded.Value().Handle())
                      .Error() == ErrorCode::CapacityExceeded);
            CHECK(SaveGraph(allocator, target).Error() == ErrorCode::CapacityExceeded);
            // 거부 직전에 배열 필드 경로의 ArrayTooLong(value = 원소 수)을 보고한다.
            ArrayHooks tooLong(allocator);
            ObjectGraphSaveOptions reported;
            reported.hooks = &tooLong;
            CHECK(SaveObjectGraph(allocator, target, target.OwnerAccess(), loaded.Value().Handle(),
                                  reported)
                      .Error() == ErrorCode::CapacityExceeded);
            REQUIRE(tooLong.count == 1);
            CHECK(tooLong.entries[0].kind == FieldDiagnosticKind::ArrayTooLong);
            CHECK(tooLong.entries[0].value == 3);
            CHECK(tooLong.entries[0].field == BytesId);
            CHECK(tooLong.entries[0].depth == 1 && tooLong.entries[0].path[0].fieldId == BytesId &&
                  !tooLong.entries[0].path[0].indexed);

            bool clean = true;
            auto elements = [](ObjectGraphLimits& limits, uint32 value) noexcept {
                limits.maxTotalElements = value;
            };
            const uint32 loadElements = FirstLoadLimit(allocator, source, wideFixture.types,
                                                       bytes.Value().View(), elements, clean);
            const uint32 saveElements =
                FirstSaveLimit(allocator, source, root.Value().Handle(), elements, clean);
            CHECK(clean);
            CHECK(loadElements != ~0u && loadElements > 40);
            CHECK(loadElements == saveElements);

            auto fields = [](ObjectGraphLimits& limits, uint32 value) noexcept {
                limits.maxTotalFields = value;
            };
            const uint32 loadFields = FirstLoadLimit(allocator, source, wideFixture.types,
                                                     bytes.Value().View(), fields, clean);
            const uint32 saveFields =
                FirstSaveLimit(allocator, source, root.Value().Handle(), fields, clean);
            CHECK(clean);
            CHECK(loadFields != ~0u && loadFields == saveFields);
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(target.Shutdown(target.OwnerAccess()));
    }

    CHECK(wideFixture.Close());
    CHECK(narrowFixture.Close());
}

// 값 byte 한도: U64 원소 524,287개(payload 정확히 4 MiB)는 저장·복원되고 하나 더면 저장이
// CapacityExceeded다. 배열 원소 수 상한 MaxArrayElements: bool 1,048,576개는 왕복하고 저장 count를
// 하나 늘린 입력은 로드가 생성자 전에 CapacityExceeded다.
TEST(ArrayGraph_ValueBytesAndMaxArrayElements) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    const FieldSchema fields[] = {
        ArrayOf(ValuesId, "values", ValueKind::UInt64, {}, MaxArrayElements),
        ArrayOf(BitsId, "bits", ValueKind::Bool, {}, MaxArrayElements)};
    const RuntimeFieldOps ops[] = {ClassCodecField<&Wide::values>(ValuesId),
                                   ClassCodecField<&Wide::bits>(BitsId)};
    REQUIRE(RegisterClass<Wide>(types, WideId, "Wide", fields, ops));
    {
        ObjectRegistry source(allocator, types, {0x4147, 14});
        ObjectRegistry target(allocator, types, {0x4147, 15});
        {
            auto root = source.NewObject<Wide>(source.OwnerAccess(), WideId);
            REQUIRE(root);
            {
                auto borrowed = source.Resolve<Wide>(source.OwnerAccess(), root.Value().Handle());
                REQUIRE(borrowed);
                Wide& wide = *borrowed.Value().Get().Value();
                constexpr usize Fits = (4 * 1024 * 1024 - 8) / 8;
                REQUIRE(wide.values.TryReserve(Fits + 1));
                for (usize index = 0; index < Fits; ++index) {
                    REQUIRE(wide.values.TryPushBack(uint64{index}));
                }
            }

            auto fits =
                SaveObjectGraph(allocator, source, source.OwnerAccess(), root.Value().Handle());
            REQUIRE(fits);
            auto loaded = LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                          fits.Value().View());
            REQUIRE(loaded);
            {
                auto borrowed = target.Resolve<Wide>(target.OwnerAccess(), loaded.Value().Handle());
                REQUIRE(borrowed);
                const Wide& wide = *borrowed.Value().Get().Value();
                CHECK(wide.values.Size() == 524287);
                CHECK(wide.values[524286] == 524286);
            }

            loaded.Value().Reset();
            {
                auto borrowed = source.Resolve<Wide>(source.OwnerAccess(), root.Value().Handle());
                REQUIRE(borrowed);
                Wide& wide = *borrowed.Value().Get().Value();
                REQUIRE(wide.values.TryPushBack(uint64{1}));
            }

            CHECK(SaveObjectGraph(allocator, source, source.OwnerAccess(), root.Value().Handle())
                      .Error() == ErrorCode::CapacityExceeded);
            {
                auto borrowed = source.Resolve<Wide>(source.OwnerAccess(), root.Value().Handle());
                REQUIRE(borrowed);
                Wide& wide = *borrowed.Value().Get().Value();
                wide.values.Clear();
                REQUIRE(wide.bits.TryReserve(MaxArrayElements));
                for (usize index = 0; index < MaxArrayElements; ++index) {
                    REQUIRE(wide.bits.TryPushBack(index % 3 == 0));
                }
            }

            auto bits =
                SaveObjectGraph(allocator, source, source.OwnerAccess(), root.Value().Handle());
            REQUIRE(bits);
            auto bitsLoaded = LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                              bits.Value().View());
            REQUIRE(bitsLoaded);
            bitsLoaded.Value().Reset();
            const usize payload = FindPayload(bits.Value().View(), BitsId, WireType::ValueArray);
            REQUIRE(payload != ~usize(0));
            REQUIRE(Read32(bits.Value().View(), payload + 4) == MaxArrayElements);
            Write32(bits.Value(), payload + 4, MaxArrayElements + 1);
            const uint32 constructed = Wide::Constructed;
            CHECK(
                LoadObjectGraph(allocator, target, types, target.OwnerAccess(), bits.Value().View())
                    .Error() == ErrorCode::CapacityExceeded);
            CHECK(Wide::Constructed == constructed);
            REQUIRE(target.Collect(target.OwnerAccess()));
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(target.Shutdown(target.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}

// 큰 NATIVE 멤버(64 KiB)를 가진 원소 struct 200개: wire는 작지만 2단계 원소 메모리(count ×
// sizeof)를 1단계가 선청구하므로 작은 work 한도에서는 생성자 전에 CapacityExceeded이고 기본
// 한도에서는 로드된다.
TEST(ArrayGraph_LargeNativeElementPrechargeFailsBeforeConstructors) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(types.RegisterStruct(GeneratedType<Heavy>::Schema(), GeneratedType<Heavy>::Ops()));
    const FieldSchema field = ArrayOf(ItemsId, "items", ValueKind::Struct, HeavyId);
    const RuntimeFieldOps ops[] = {
        ClassCodecField<&HeavyHolder::items>(ItemsId, NativeTypeKey<Heavy>())};
    REQUIRE(RegisterClass<HeavyHolder>(types, HeavyHolderId, "HeavyHolder",
                                       Span<const FieldSchema>(&field, 1), ops));
    {
        ObjectRegistry source(allocator, types, {0x4147, 16});
        ObjectRegistry target(allocator, types, {0x4147, 17});
        {
            auto root = source.NewObject<HeavyHolder>(source.OwnerAccess(), HeavyHolderId);
            REQUIRE(root);
            {
                auto borrowed =
                    source.Resolve<HeavyHolder>(source.OwnerAccess(), root.Value().Handle());
                REQUIRE(borrowed);
                HeavyHolder& holder = *borrowed.Value().Get().Value();
                REQUIRE(holder.items.TryReserve(200));
                for (uint32 index = 0; index < 200; ++index) {
                    auto added = holder.items.TryEmplaceBack();
                    REQUIRE(added);
                    added.Value()->value = index;
                }
            }

            auto bytes =
                SaveObjectGraph(allocator, source, source.OwnerAccess(), root.Value().Handle());
            REQUIRE(bytes);
            CHECK(bytes.Value().Size() < 64 * 1024);
            ObjectGraphLoadOptions small;
            small.limits.maxWork = 8 * 1024 * 1024;
            const uint32 constructed = HeavyHolder::Constructed;
            CHECK(LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                  bytes.Value().View(), small)
                      .Error() == ErrorCode::CapacityExceeded);
            CHECK(HeavyHolder::Constructed == constructed);
            auto loaded = LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                          bytes.Value().View());
            REQUIRE(loaded);
            auto borrowed =
                target.Resolve<HeavyHolder>(target.OwnerAccess(), loaded.Value().Handle());
            REQUIRE(borrowed);
            CHECK(borrowed.Value().Get().Value()->items.Size() == 200);
            CHECK(borrowed.Value().Get().Value()->items[199].value == 199);
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(target.Shutdown(target.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}

// 작업량 한도 스윕: 모든 실패는 CapacityExceeded이고 생성자 전이다. 처음 성공하는 한도가 1단계
// 사용량이다 — 2단계는 그래프 예산에 닿지 않으므로 그 값이면 성공하고 1 작으면 생성자 전에
// 실패한다. 저장 스윕도 모든 실패가 CapacityExceeded다.
TEST(ArrayGraph_EveryWorkLimitFailsBeforeConstructorsAndExactBudgetSucceeds) {
    SystemAllocator allocator;
    Fixture fixture(allocator);
    REQUIRE(fixture.ok);
    {
        ObjectRegistry source(allocator, fixture.types, {0x4147, 18});
        ObjectRegistry target(allocator, fixture.types, {0x4147, 19});
        {
            auto root = MakeGraph(source, true);
            REQUIRE(root);
            auto bytes =
                SaveObjectGraph(allocator, source, source.OwnerAccess(), root.Value().Handle());
            REQUIRE(bytes);
            uint64 first = 0;
            bool clean = true;
            for (uint64 work = 0; work < 4000000; ++work) {
                ObjectGraphLoadOptions options;
                options.limits.maxWork = work;
                const uint32 constructed = Node::Constructed;
                auto result = LoadObjectGraph(allocator, target, fixture.types,
                                              target.OwnerAccess(), bytes.Value().View(), options);
                if (result) {
                    first = work;
                    break;
                }

                clean = clean && result.Error() == ErrorCode::CapacityExceeded &&
                        Node::Constructed == constructed;
            }

            CHECK(clean);
            REQUIRE(first != 0);
            ObjectGraphLoadOptions exact;
            exact.limits.maxWork = first;
            CHECK(LoadObjectGraph(allocator, target, fixture.types, target.OwnerAccess(),
                                  bytes.Value().View(), exact));
            exact.limits.maxWork = first - 1;
            const uint32 constructed = Node::Constructed;
            CHECK(LoadObjectGraph(allocator, target, fixture.types, target.OwnerAccess(),
                                  bytes.Value().View(), exact)
                      .Error() == ErrorCode::CapacityExceeded);
            CHECK(Node::Constructed == constructed);

            bool saved = false;
            bool saveClean = true;
            for (uint64 work = 0; !saved && work < 4000000; ++work) {
                ObjectGraphSaveOptions options;
                options.limits.maxWork = work;
                auto result = SaveObjectGraph(allocator, source, source.OwnerAccess(),
                                              root.Value().Handle(), options);
                saved = static_cast<bool>(result);
                saveClean = saveClean && (saved || result.Error() == ErrorCode::CapacityExceeded);
            }

            CHECK(saved && saveClean);
            REQUIRE(target.Collect(target.OwnerAccess()));
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(target.Shutdown(target.OwnerAccess()));
    }

    CHECK(fixture.Close());
}

namespace {

Result<void> LoadWithFailure(test::NFailAllocator& allocator, TypeRegistry& types,
                             Span<const uint8> bytes, uint64* attempts = nullptr) noexcept {
    ObjectRegistry registry(allocator, types, {0x4147, 90});
    auto result = LoadObjectGraph(allocator, registry, types, registry.OwnerAccess(), bytes);
    if (attempts != nullptr) {
        *attempts = allocator.Attempts();
    }

    if (!result) {
        if (registry.Stats().liveObjects != 0 || registry.Stats().reservedObjects != 0) {
            return Result<void>::Failure(ErrorCode::DependencyFailed);
        }

        return Result<void>::Failure(result.Error());
    }

    result.Value().Reset();
    if (auto collected = registry.Collect(registry.OwnerAccess()); !collected) {
        return Result<void>::Failure(collected.Error());
    }

    return registry.Shutdown(registry.OwnerAccess());
}

} // namespace

// 저장·로드의 모든 할당 실패: OutOfMemory이고 부분 객체·lease·할당이 남지 않는다(2단계 실패 —
// AssignReferences·AssignElements·원소 버퍼 — 는 그래프 전체 rollback).
TEST(ArrayGraph_EveryAllocationFailureRollsBack) {
    SystemAllocator allocator;
    Fixture fixture(allocator);
    REQUIRE(fixture.ok);
    {
        ObjectRegistry objects(allocator, fixture.types, {0x4147, 20});
        {
            auto root = MakeGraph(objects, true);
            REQUIRE(root);
            test::NFailAllocator failure(allocator);
            uint64 attempts = 0;
            {
                auto baseline =
                    SaveObjectGraph(failure, objects, objects.OwnerAccess(), root.Value().Handle());
                REQUIRE(baseline);
                attempts = failure.Attempts();
            }

            const auto baselineStats = allocator.Stats();
            const auto baselineLeases = fixture.types.Stats().leases;
            bool clean = true;
            for (uint64 index = 1; index <= attempts; ++index) {
                failure.FailOnAttempt(index);
                auto saved =
                    SaveObjectGraph(failure, objects, objects.OwnerAccess(), root.Value().Handle());
                clean = clean && !saved && saved.Error() == ErrorCode::OutOfMemory &&
                        allocator.Stats().liveAllocations == baselineStats.liveAllocations &&
                        fixture.types.Stats().leases == baselineLeases &&
                        objects.Stats().borrows == 0 && objects.Stats().encodePins == 0;
            }

            CHECK(clean);
            failure.DisableFailure();
            auto bytes =
                SaveObjectGraph(allocator, objects, objects.OwnerAccess(), root.Value().Handle());
            REQUIRE(bytes);
            REQUIRE(LoadWithFailure(failure, fixture.types, bytes.Value().View(), &attempts));
            const auto loadStats = allocator.Stats();
            for (uint64 index = 1; index <= attempts; ++index) {
                failure.FailOnAttempt(index);
                auto loaded = LoadWithFailure(failure, fixture.types, bytes.Value().View());
                clean = clean && !loaded && loaded.Error() == ErrorCode::OutOfMemory &&
                        allocator.Stats().liveAllocations == loadStats.liveAllocations &&
                        fixture.types.Stats().leases == baselineLeases;
            }

            CHECK(clean);
        }

        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
    }

    CHECK(fixture.Close());
}

// 증분 GC Marking 중에도 staged 배열(AssignReferences·AssignElements·원소 참조)을 가진 그래프를
// 로드할 수 있고, 수집을 끝내도 로드한 객체와 배열 간선이 살아 있다.
TEST(ArrayGraph_StagedArraysLoadDuringMarking) {
    SystemAllocator allocator;
    Fixture fixture(allocator);
    REQUIRE(fixture.ok);
    {
        ObjectRegistry source(allocator, fixture.types, {0x4147, 21});
        ObjectRegistry target(allocator, fixture.types, {0x4147, 22});
        {
            auto bytes = SaveGraph(allocator, source);
            REQUIRE(bytes);
            // Marking에 일감이 남도록 루트 그래프 셋(객체 여섯)을 둔다.
            auto first = LoadObjectGraph(allocator, target, fixture.types, target.OwnerAccess(),
                                         bytes.Value().View());
            auto second = LoadObjectGraph(allocator, target, fixture.types, target.OwnerAccess(),
                                          bytes.Value().View());
            auto third = LoadObjectGraph(allocator, target, fixture.types, target.OwnerAccess(),
                                         bytes.Value().View());
            REQUIRE(first && second && third);
            REQUIRE(target.BeginCollection(target.OwnerAccess()));
            GcBudget slice;
            slice.maxObjects = 1;
            slice.maxNanoseconds = 0;
            REQUIRE(target.StepCollection(target.OwnerAccess(), slice));
            REQUIRE(target.CollectionPhase() == GcPhase::Marking);
            auto loaded = LoadObjectGraph(allocator, target, fixture.types, target.OwnerAccess(),
                                          bytes.Value().View());
            REQUIRE(loaded);
            for (uint32 guard = 0; guard < 4096 && target.CollectionPhase() != GcPhase::Idle;
                 ++guard) {
                REQUIRE(target.StepCollection(target.OwnerAccess(), slice));
            }

            CHECK(target.CollectionPhase() == GcPhase::Idle);
            CHECK(target.Stats().liveObjects == 8);
            const ObjectHandle self = loaded.Value().Handle();
            CHECK(WithRoot(target, self, [&](const Node& node) noexcept {
                return node.refs.Size() == 3 && CheckFilled(node, self, node.refs[0]);
            }));
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(target.Shutdown(target.OwnerAccess()));
    }

    CHECK(fixture.Close());
}

// 기대 형식 보관 수는 참조 멤버 종류 수다: 원소 4096개의 StructRefArray(원소마다 기대가 다른 참조
// 멤버 둘)를 로드해도 원소마다 기대를 보관하지 않는다 — 가장 큰 단일 할당(token·목록 색인 수준)이
// 원소 수 × 40 byte보다 작다(원소마다 두 기대를 보관하면 그 배열만 원소 × 48 byte 이상이다).
TEST(ArrayGraph_OneExpectationPerReferenceMemberKind) {
    SystemAllocator allocator;
    Fixture fixture(allocator);
    REQUIRE(fixture.ok);
    constexpr usize Elements = 4096;
    {
        ObjectRegistry source(allocator, fixture.types, {0x4147, 23});
        {
            ConstructionContext transaction(source, source.OwnerAccess());
            auto a = transaction.Create<Node>(NodeId);
            auto b = transaction.Create<Node>(NodeId);
            REQUIRE(a && b);
            auto node = transaction.Get<Node>(a.Value());
            REQUIRE(node);
            REQUIRE(transaction.AssignElements(a.Value(), node.Value()->slots, Elements));
            for (usize index = 0; index < Elements; ++index) {
                Slot& slot = node.Value()->slots[index];
                REQUIRE(transaction.SetElementReference(a.Value(), node.Value()->slots, index,
                                                        slot.material, b.Value()));
                REQUIRE(transaction.SetElementReference(a.Value(), node.Value()->slots, index,
                                                        slot.overlay, b.Value()));
            }

            auto root = transaction.Commit();
            REQUIRE(root);
            auto bytes =
                SaveObjectGraph(allocator, source, source.OwnerAccess(), root.Value().Handle());
            REQUIRE(bytes);
            TrackingAllocator tracking(allocator);
            ObjectRegistry target(tracking, fixture.types, {0x4147, 24});
            {
                tracking.Reset();
                auto loaded = LoadObjectGraph(tracking, target, fixture.types, target.OwnerAccess(),
                                              bytes.Value().View());
                REQUIRE(loaded);
                CHECK(tracking.largest < Elements * 40);
                auto borrowed = target.Resolve<Node>(target.OwnerAccess(), loaded.Value().Handle());
                REQUIRE(borrowed);
                const Node& copy = *borrowed.Value().Get().Value();
                REQUIRE(copy.slots.Size() == Elements);
                CHECK(copy.slots[Elements - 1].material.Handle() != ObjectHandle{});
                CHECK(copy.slots[Elements - 1].overlay.Handle() == copy.slots[0].material.Handle());
            }

            // 1단계는 남은 원소 누계를 목록의 항목 수 한도로 쓰므로 항목 색인(원소 × 16 byte)을
            // 할당하기 전에 CapacityExceeded다.
            {
                tracking.Reset();
                ObjectGraphLoadOptions few;
                few.limits.maxTotalElements = 100;
                const uint32 constructed = Node::Constructed;
                CHECK(LoadObjectGraph(tracking, target, fixture.types, target.OwnerAccess(),
                                      bytes.Value().View(), few)
                          .Error() == ErrorCode::CapacityExceeded);
                CHECK(Node::Constructed == constructed);
                CHECK(tracking.largest < Elements * 4);
            }

            REQUIRE(target.Shutdown(target.OwnerAccess()));
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
    }

    CHECK(fixture.Close());
}

// 저장 참조 비용은 참조 수에 선형이다: 원소 65,536개의 ObjectRefArray가 노드 1,000개를 가리켜도
// (handle → local ID 해시·노드 lease·기대 lease 캐시) 기본 예산은 물론 8 Mi 작업 안에서 저장되고,
// 로드도 같은 한도 안이다.
TEST(ArrayGraph_SaveReferenceCostIsLinear) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    const FieldSchema field = ArrayOf(LinksId, "links", ValueKind::ObjectReference, HubId);
    const RuntimeFieldOps ops[] = {ClassCodecField<&Hub::links>(LinksId)};
    REQUIRE(RegisterClass<Hub>(types, HubId, "Hub", Span<const FieldSchema>(&field, 1), ops));
    constexpr usize Nodes = 1000;
    constexpr usize Links = 65536;
    {
        ObjectRegistry source(allocator, types, {0x4147, 25});
        ObjectRegistry target(allocator, types, {0x4147, 26});
        {
            ConstructionContext transaction(source, source.OwnerAccess());
            Array<ObjectHandle> hubs(allocator);
            REQUIRE(hubs.TryReserve(Nodes));
            for (usize index = 0; index < Nodes; ++index) {
                auto created = transaction.Create<Hub>(HubId);
                REQUIRE(created);
                REQUIRE(hubs.TryPushBack(created.Value()));
            }

            Array<ObjectHandle> links(allocator);
            REQUIRE(links.TryReserve(Links));
            for (usize index = 0; index < Links; ++index) {
                REQUIRE(links.TryPushBack(hubs[(index * 7) % Nodes]));
            }

            auto rootHub = transaction.Get<Hub>(hubs[0]);
            REQUIRE(rootHub);
            REQUIRE(transaction.AssignReferences(hubs[0], rootHub.Value()->links, links.View()));
            auto root = transaction.Commit();
            REQUIRE(root);
            ObjectGraphSaveOptions save;
            save.limits.maxWork = 8 * 1024 * 1024;
            auto bytes = SaveObjectGraph(allocator, source, source.OwnerAccess(),
                                         root.Value().Handle(), save);
            REQUIRE(bytes);
            CHECK(SaveObjectGraph(allocator, source, source.OwnerAccess(), root.Value().Handle()));
            ObjectGraphLoadOptions load;
            load.limits.maxWork = 8 * 1024 * 1024;
            auto loaded = LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                          bytes.Value().View(), load);
            REQUIRE(loaded);
            CHECK(target.Stats().liveObjects == Nodes);
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(target.Shutdown(target.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}

// 대상 타입·도달성: 스키마 기대 타입과 맞지 않는 원소는 저장이 TypeMismatch다. 정적 원소 타입이
// 넓은 Registry(ObjectRefArray<Object>)에서 저장한 파일을 좁은 Registry(같은 ID의
// ObjectRefArray<Hub>)로 로드하면 도달성 검사가 생성 전에 TypeMismatch이고, 원소 local ID를 범위
// 밖으로 바꾸면 InvalidArgument, 0(null)으로 바꿔 노드가 고아가 되면 InvalidArgument다.
TEST(ArrayGraph_TargetTypeMismatchAndReachability) {
    SystemAllocator allocator;
    TypeRegistry looseTypes(allocator);
    TypeRegistry strictTypes(allocator);
    TypeRegistry checkedTypes(allocator);
    const RuntimeFieldOps hubOps[] = {ClassCodecField<&Hub::links>(LinksId)};
    const FieldSchema hubField = ArrayOf(LinksId, "links", ValueKind::ObjectReference, HubId);
    const FieldSchema anyField = ArrayOf(LinksId, "links", ValueKind::ObjectReference);
    const RuntimeFieldOps looseOps[] = {ClassCodecField<&Loose::links>(LinksId)};
    const RuntimeFieldOps strictOps[] = {ClassCodecField<&Strict::links>(LinksId)};
    TypeRegistry* const registries[] = {&looseTypes, &strictTypes, &checkedTypes};
    for (TypeRegistry* types : registries) {
        REQUIRE(RegisterClass<Hub>(*types, HubId, "Hub", Span<const FieldSchema>(&hubField, 1),
                                   hubOps));
        REQUIRE(RegisterClass<Other>(*types, OtherId, "Other", {}, {}));
    }

    REQUIRE(RegisterClass<Loose>(looseTypes, LooseId, "Loose",
                                 Span<const FieldSchema>(&anyField, 1), looseOps));
    REQUIRE(RegisterClass<Strict>(strictTypes, LooseId, "Strict",
                                  Span<const FieldSchema>(&hubField, 1), strictOps));
    // 정적 원소 타입은 Object지만 스키마 기대 타입이 Hub인 Registry.
    REQUIRE(RegisterClass<Loose>(checkedTypes, LooseId, "Loose",
                                 Span<const FieldSchema>(&hubField, 1), looseOps));
    {
        ObjectRegistry loose(allocator, looseTypes, {0x4147, 27});
        ObjectRegistry strict(allocator, strictTypes, {0x4147, 28});
        ObjectRegistry checked(allocator, checkedTypes, {0x4147, 29});
        {
            auto build = [&](ObjectRegistry& objects) noexcept -> Result<RootRef<Object>> {
                ConstructionContext transaction(objects, objects.OwnerAccess());
                auto root = transaction.Create<Loose>(LooseId);
                auto other = transaction.Create<Other>(OtherId);
                auto hub = transaction.Create<Hub>(HubId);
                if (!root || !other || !hub) {
                    return Result<RootRef<Object>>::Failure(ErrorCode::OutOfMemory);
                }

                auto value = transaction.Get<Loose>(root.Value());
                const ObjectHandle targets[] = {hub.Value(), other.Value()};
                if (!value ||
                    !transaction.AssignReferences(root.Value(), value.Value()->links, targets)) {
                    return Result<RootRef<Object>>::Failure(ErrorCode::OutOfMemory);
                }

                return transaction.Commit();
            };
            auto checkedRoot = build(checked);
            REQUIRE(checkedRoot);
            CHECK(SaveObjectGraph(allocator, checked, checked.OwnerAccess(),
                                  checkedRoot.Value().Handle())
                      .Error() == ErrorCode::TypeMismatch);

            auto looseRoot = build(loose);
            REQUIRE(looseRoot);
            auto bytes =
                SaveObjectGraph(allocator, loose, loose.OwnerAccess(), looseRoot.Value().Handle());
            REQUIRE(bytes);
            CHECK(LoadObjectGraph(allocator, loose, looseTypes, loose.OwnerAccess(),
                                  bytes.Value().View()));
            CHECK(LoadObjectGraph(allocator, strict, strictTypes, strict.OwnerAccess(),
                                  bytes.Value().View())
                      .Error() == ErrorCode::TypeMismatch);
            CHECK(strict.Stats().liveObjects == 0);

            const usize payload = FindPayload(bytes.Value().View(), LinksId, WireType::ValueArray);
            REQUIRE(payload != ~usize(0));
            REQUIRE(Read32(bytes.Value().View(), payload + 4) == 2);
            Write64(bytes.Value(), payload + 8 + 8, 9);
            CHECK(LoadObjectGraph(allocator, loose, looseTypes, loose.OwnerAccess(),
                                  bytes.Value().View())
                      .Error() == ErrorCode::InvalidArgument);
            Write64(bytes.Value(), payload + 8 + 8, 0);
            CHECK(LoadObjectGraph(allocator, loose, looseTypes, loose.OwnerAccess(),
                                  bytes.Value().View())
                      .Error() == ErrorCode::InvalidArgument);
        }

        REQUIRE(loose.Shutdown(loose.OwnerAccess()));
        REQUIRE(strict.Shutdown(strict.OwnerAccess()));
        REQUIRE(checked.Shutdown(checked.OwnerAccess()));
    }

    CHECK(looseTypes.Shutdown());
    CHECK(strictTypes.Shutdown());
    CHECK(checkedTypes.Shutdown());
}

// 배열을 거친 깊이 16(배열 안 struct 안 배열 … 맨 아래 3 MiB 문자열)의 저장·복원 작업은 문자열
// byte의 작은 배수 안이다(목록 항목·중첩 Record를 색인 전용으로 열어 깊이 배수로 불어나지 않는다).
TEST(ArrayGraph_DepthSixteenThroughArraysDoesNotMultiplyWork) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(DeepChain<DeepTop>::Register(types));
    {
        auto top = types.AcquireValueType(DeepId(DeepTop), TypeKind::Struct);
        REQUIRE(top);
        CHECK(top.Value().Struct().Depth() == MaxStructDepth);
    }

    const FieldSchema field = ArrayOf(ItemsId, "chain", ValueKind::Struct, DeepId(DeepTop));
    const RuntimeFieldOps ops[] = {
        ClassCodecField<&DeepHolder::chain>(ItemsId, NativeTypeKey<Deep<DeepTop>>())};
    REQUIRE(RegisterClass<DeepHolder>(types, DeepHolderId, "DeepHolder",
                                      Span<const FieldSchema>(&field, 1), ops));
    constexpr usize LabelBytes = 3 * 1024 * 1024;
    {
        ObjectRegistry source(allocator, types, {0x4147, 30});
        ObjectRegistry target(allocator, types, {0x4147, 31});
        {
            auto root = source.NewObject<DeepHolder>(source.OwnerAccess(), DeepHolderId);
            REQUIRE(root);
            {
                auto borrowed =
                    source.Resolve<DeepHolder>(source.OwnerAccess(), root.Value().Handle());
                REQUIRE(borrowed);
                DeepHolder& holder = *borrowed.Value().Get().Value();
                auto top = holder.chain.TryEmplaceBack(holder.chain.GetAllocator());
                REQUIRE(top);
                Array<char> text(allocator);
                REQUIRE(text.TryReserve(LabelBytes));
                for (usize index = 0; index < LabelBytes; ++index) {
                    REQUIRE(text.TryPushBack(static_cast<char>(97 + index % 26)));
                }

                // 단계마다 원소 하나를 더해 맨 아래(Deep<0>)까지 내려간다.
                REQUIRE(
                    DeepChain<DeepTop>::Fill(*top.Value(), StringView(text.Data(), text.Size())));
            }

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
            auto borrowed =
                target.Resolve<DeepHolder>(target.OwnerAccess(), loaded.Value().Handle());
            REQUIRE(borrowed);
            const Deep<DeepTop>& top = borrowed.Value().Get().Value()->chain[0];
            const Deep<DeepTop - 1>& next = top.items[0];
            CHECK(next.items.Size() == 1);
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(target.Shutdown(target.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}

// 원소 1만 개 StructRefArray의 그래프 로드 할당 수: 원소마다 블록 하나와 두 단계의 항목 Record
// 색인(항목마다 두 배열)뿐이다 — 원소 수에 비례하는 상수 배(원소당 6 이하)다.
TEST(ArrayGraph_TenThousandStructElementsLoadAllocationCount) {
    SystemAllocator allocator;
    Fixture fixture(allocator);
    REQUIRE(fixture.ok);
    constexpr usize Elements = 10000;
    {
        ObjectRegistry source(allocator, fixture.types, {0x4147, 32});
        {
            ConstructionContext transaction(source, source.OwnerAccess());
            auto created = transaction.Create<Node>(NodeId);
            REQUIRE(created);
            auto node = transaction.Get<Node>(created.Value());
            REQUIRE(node);
            REQUIRE(transaction.AssignElements(created.Value(), node.Value()->slots, Elements));
            auto root = transaction.Commit();
            REQUIRE(root);
            auto bytes =
                SaveObjectGraph(allocator, source, source.OwnerAccess(), root.Value().Handle());
            REQUIRE(bytes);
            TrackingAllocator tracking(allocator);
            ObjectRegistry target(tracking, fixture.types, {0x4147, 33});
            {
                tracking.Reset();
                auto loaded = LoadObjectGraph(tracking, target, fixture.types, target.OwnerAccess(),
                                              bytes.Value().View());
                REQUIRE(loaded);
                CHECK(tracking.allocations <= 6 * Elements);
                CHECK(tracking.allocations >= Elements);
            }

            REQUIRE(target.Shutdown(target.OwnerAccess()));
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
    }

    CHECK(fixture.Close());
}

// byte·객체·마이그레이션·값 한도도 0부터 올리는 스윕의 모든 실패가 CapacityExceeded이고 생성자
// 전이다. 파일 byte 한도는 정확히 파일 크기에서, 객체 한도는 노드 수에서 처음 성공하고,
// 마이그레이션 한도는 struct 원소 마이그레이션 결과 누계가 들어가야 성공한다.
TEST(ArrayGraph_ByteObjectMigrationAndValueLimitSweeps) {
    SystemAllocator allocator;
    Fixture oldFixture(allocator);
    FixtureOptions newer;
    newer.taggedVersion = 2;
    newer.taggedMigrate = &MigrateTagged;
    Fixture newFixture(allocator, newer);
    REQUIRE(oldFixture.ok && newFixture.ok);
    {
        ObjectRegistry source(allocator, oldFixture.types, {0x4147, 34});
        ObjectRegistry target(allocator, oldFixture.types, {0x4147, 35});
        ObjectRegistry migrated(allocator, newFixture.types, {0x4147, 36});
        {
            auto bytes = SaveGraph(allocator, source, {}, true);
            REQUIRE(bytes);
            bool clean = true;
            const uint32 fileBytes = static_cast<uint32>(bytes.Value().Size());
            const uint32 firstBytes = FirstLoadLimit(
                allocator, target, oldFixture.types, bytes.Value().View(),
                [](ObjectGraphLimits& limits, uint32 value) noexcept { limits.maxBytes = value; },
                clean, fileBytes + 1);
            CHECK(firstBytes == fileBytes);
            const uint32 firstObjects = FirstLoadLimit(
                allocator, target, oldFixture.types, bytes.Value().View(),
                [](ObjectGraphLimits& limits, uint32 value) noexcept { limits.maxObjects = value; },
                clean);
            CHECK(firstObjects == 2);
            const uint32 firstValue = FirstLoadLimit(
                allocator, target, oldFixture.types, bytes.Value().View(),
                [](ObjectGraphLimits& limits, uint32 value) noexcept {
                    limits.maxValueBytes = value;
                },
                clean, fileBytes + 1);
            CHECK(firstValue != ~0u && firstValue < fileBytes);
            TaggedMigrations = 0;
            const uint32 firstMigration = FirstLoadLimit(
                allocator, migrated, newFixture.types, bytes.Value().View(),
                [](ObjectGraphLimits& limits, uint32 value) noexcept {
                    limits.maxMigrationBytes = value;
                },
                clean, fileBytes + 1);
            CHECK(firstMigration != ~0u && firstMigration > 0);
            CHECK(TaggedMigrations > 0);
            CHECK(clean);
            REQUIRE(target.Collect(target.OwnerAccess()));
            REQUIRE(migrated.Collect(migrated.OwnerAccess()));
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(target.Shutdown(target.OwnerAccess()));
        REQUIRE(migrated.Shutdown(migrated.OwnerAccess()));
    }

    CHECK(oldFixture.Close());
    CHECK(newFixture.Close());
}

namespace {

// 로드가 처음 성공하는 최소 작업 한도다(1단계 사용량 — 2단계는 그래프 예산에 닿지 않는다).
uint64 MinimumLoadWork(Allocator& allocator, ObjectRegistry& target, TypeRegistry& types,
                       Span<const uint8> bytes, FieldCodecHooks* hooks) noexcept {
    uint64 low = 0;
    uint64 high = 64 * 1024 * 1024;
    while (low < high) {
        const uint64 middle = low + (high - low) / 2;
        ObjectGraphLoadOptions options;
        options.limits.maxWork = middle;
        options.hooks = hooks;
        if (LoadObjectGraph(allocator, target, types, target.OwnerAccess(), bytes, options)) {
            high = middle;
        } else {
            low = middle + 1;
        }
    }

    return low;
}

} // namespace

// 관찰 할당은 한 번만 청구한다: 관찰이 없을 때와 있을 때의 최소 작업 한도 차이는 관찰 임시 값·원소
// 버퍼·문자열 사본으로 할당한 byte × 1(2단계는 관찰하지 않으므로 두 배가 아니다)에 관찰 목록의
// 검사·정렬 몫(ID 하나 = 1 + 64)을 더한 값과 정확히 같다.
TEST(ArrayGraph_ObservationAllocationsAreChargedOnce) {
    SystemAllocator allocator;
    Fixture fixture(allocator);
    REQUIRE(fixture.ok);
    {
        ObjectRegistry source(allocator, fixture.types, {0x4147, 37});
        ObjectRegistry target(allocator, fixture.types, {0x4147, 38});
        {
            auto bytes = SaveGraph(allocator, source);
            REQUIRE(bytes);
            const TypeId observedIds[] = {TaggedId};
            ArrayHooks plain(allocator);
            ArrayHooks observing(allocator);
            observing.observe = observedIds;
            TrackingAllocator tracking(allocator);
            ObjectGraphLoadOptions options;
            options.hooks = &plain;
            tracking.Reset();
            REQUIRE(LoadObjectGraph(tracking, target, fixture.types, target.OwnerAccess(),
                                    bytes.Value().View(), options));
            const usize plainBytes = tracking.total;
            options.hooks = &observing;
            tracking.Reset();
            REQUIRE(LoadObjectGraph(tracking, target, fixture.types, target.OwnerAccess(),
                                    bytes.Value().View(), options));
            const usize observingBytes = tracking.total;
            REQUIRE(observing.observed == 4);
            // 관찰 목록 배열(TypeId 하나)은 예산을 청구하지 않는 로드 자체의 할당이다.
            REQUIRE(observingBytes > plainBytes + sizeof(TypeId));
            const uint64 observationBytes = observingBytes - plainBytes - sizeof(TypeId);
            const uint64 plainWork =
                MinimumLoadWork(allocator, target, fixture.types, bytes.Value().View(), &plain);
            observing.observed = 0;
            const uint64 observingWork =
                MinimumLoadWork(allocator, target, fixture.types, bytes.Value().View(), &observing);
            CHECK(observingWork - plainWork == observationBytes + 1 + 64);
            REQUIRE(target.Collect(target.OwnerAccess()));
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(target.Shutdown(target.OwnerAccess()));
    }

    CHECK(fixture.Close());
}

// 목록 항목(struct 원소)의 높은 버전: 기본 거부(생성자 전 Unsupported),
// LoadNewerSkippingUnknown이면 원소마다 경로 [tagged[i]]의 NewerVersion 진단 뒤 현재 스키마로
// 읽는다(단일 struct는 [single]).
TEST(ArrayGraph_StructElementNewerVersionPolicy) {
    SystemAllocator allocator;
    FixtureOptions newer;
    newer.taggedVersion = 2;
    Fixture newFixture(allocator, newer);
    Fixture oldFixture(allocator);
    REQUIRE(newFixture.ok && oldFixture.ok);
    {
        ObjectRegistry source(allocator, newFixture.types, {0x4147, 39});
        ObjectRegistry target(allocator, oldFixture.types, {0x4147, 40});
        {
            auto bytes = SaveGraph(allocator, source);
            REQUIRE(bytes);
            const uint32 constructed = Node::Constructed;
            CHECK(LoadObjectGraph(allocator, target, oldFixture.types, target.OwnerAccess(),
                                  bytes.Value().View())
                      .Error() == ErrorCode::Unsupported);
            CHECK(Node::Constructed == constructed);
            ArrayHooks hooks(allocator);
            ObjectGraphLoadOptions load;
            load.hooks = &hooks;
            load.policy.newerVersion = NewerVersionPolicy::LoadNewerSkippingUnknown;
            auto loaded = LoadObjectGraph(allocator, target, oldFixture.types, target.OwnerAccess(),
                                          bytes.Value().View(), load);
            REQUIRE(loaded);
            // A의 tagged 원소 둘·single, B의 single.
            CHECK(hooks.Count(FieldDiagnosticKind::NewerVersion) == 4);
            usize elements = 0;
            usize singles = 0;
            for (usize index = 0; index < hooks.count; ++index) {
                const ArrayHooks::Entry& entry = hooks.entries[index];
                if (entry.kind != FieldDiagnosticKind::NewerVersion || entry.depth != 1 ||
                    entry.value != 2) {
                    continue;
                }

                elements += entry.path[0].fieldId == TaggedFieldId && entry.path[0].indexed &&
                            entry.path[0].index == elements;
                singles += entry.path[0].fieldId == SingleId && !entry.path[0].indexed;
            }

            CHECK(elements == 2 && singles == 2);
            CHECK(WithRoot(target, loaded.Value().Handle(), [](const Node& node) noexcept {
                return node.tagged.Size() == 2 && node.tagged[0].codes.Size() == 3;
            }));
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(target.Shutdown(target.OwnerAccess()));
    }

    CHECK(newFixture.Close());
    CHECK(oldFixture.Close());
}

// 원소 struct 멤버 안의 enum 배열(tagged[0].masks)의 미선언 값: 기본은 생성자 전 거부,
// ReportAndDefault면 경로 [tagged[0], masks[1]]의 진단 뒤 대체 값(Flags — 알려진 비트만 남김)이다.
TEST(ArrayGraph_UndeclaredEnumElementInsideStructElement) {
    SystemAllocator allocator;
    Fixture fixture(allocator);
    REQUIRE(fixture.ok);
    {
        ObjectRegistry source(allocator, fixture.types, {0x4147, 41});
        ObjectRegistry target(allocator, fixture.types, {0x4147, 42});
        {
            auto bytes = SaveGraph(allocator, source);
            REQUIRE(bytes);
            const usize masks = FindPayload(bytes.Value().View(), MasksId, WireType::ValueArray);
            REQUIRE(masks != ~usize(0));
            REQUIRE(Read32(bytes.Value().View(), masks + 4) == 2);
            REQUIRE(Read64(bytes.Value().View(), masks + 16) == 4);
            Write64(bytes.Value(), masks + 16, 16 | 4);
            const uint32 constructed = Node::Constructed;
            CHECK(LoadObjectGraph(allocator, target, fixture.types, target.OwnerAccess(),
                                  bytes.Value().View())
                      .Error() == ErrorCode::InvalidArgument);
            CHECK(Node::Constructed == constructed);
            ArrayHooks hooks(allocator);
            ObjectGraphLoadOptions report;
            report.hooks = &hooks;
            report.policy.undeclaredEnum = UndeclaredEnumPolicy::ReportAndDefault;
            auto loaded = LoadObjectGraph(allocator, target, fixture.types, target.OwnerAccess(),
                                          bytes.Value().View(), report);
            REQUIRE(loaded);
            REQUIRE(hooks.Count(FieldDiagnosticKind::UndeclaredEnumValue) == 1);
            const ArrayHooks::Entry& entry = hooks.entries[0];
            CHECK(entry.kind == FieldDiagnosticKind::UndeclaredEnumValue);
            CHECK(entry.value == (16 | 4));
            CHECK(entry.depth == 2);
            CHECK(entry.path[0].fieldId == TaggedFieldId && entry.path[0].indexed &&
                  entry.path[0].index == 0);
            CHECK(entry.path[1].fieldId == MasksId && entry.path[1].indexed &&
                  entry.path[1].index == 1);
            CHECK(WithRoot(target, loaded.Value().Handle(), [](const Node& node) noexcept {
                return node.tagged[0].masks.Size() == 2 &&
                       node.tagged[0].masks[1] == Channels::Blue;
            }));
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(target.Shutdown(target.OwnerAccess()));
    }

    CHECK(fixture.Close());
}

namespace {

// 같은 native의 별칭 타입 수백 개로 서로 다른 (기대, 실제) 쌍을 만드는 클래스다.
struct Ref final : Object {
    explicit Ref(Allocator& allocator) noexcept : links(allocator) {
    }

    void Trace(TraceVisitor& visitor) const noexcept {
        detail::TraceArrayField(links, visitor);
    }

    ObjectRefArray<Object> links;
};

constexpr TypeId RefType(usize index) noexcept {
    return TypeId{0x415246, index + 1};
}

} // namespace

// 도달성 검사의 (기대, 실제) 캐시와 기대 lease는 해시 표다: 원소 65,536개가 서로 다른 쌍 300개를
// 번갈아 가리켜도 6 Mi 작업 안에서 로드된다(선형 탐색이면 원소마다 쌍 목록의 절반을 훑어 약 1천만
// 작업이 더 든다).
TEST(ArrayGraph_ReachabilityTypeCacheScalesWithDistinctPairs) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    constexpr usize Kinds = 301;
    constexpr usize Links = 65536;
    // 모든 별칭의 링크 원소 기대 타입은 RefType(0)이다.
    const FieldSchema field = ArrayOf(LinksId, "links", ValueKind::ObjectReference, RefType(0));
    const RuntimeFieldOps ops[] = {ClassCodecField<&Ref::links>(LinksId)};
    for (usize kind = 0; kind < Kinds; ++kind) {
        REQUIRE(RegisterClass<Ref>(types, RefType(kind), "Ref", Span<const FieldSchema>(&field, 1),
                                   ops));
    }

    {
        ObjectRegistry source(allocator, types, {0x4147, 43});
        ObjectRegistry target(allocator, types, {0x4147, 44});
        {
            ConstructionContext transaction(source, source.OwnerAccess());
            Array<ObjectHandle> objects(allocator);
            REQUIRE(objects.TryReserve(Kinds));
            for (usize kind = 0; kind < Kinds; ++kind) {
                auto created = transaction.Create<Ref>(RefType(kind));
                REQUIRE(created);
                REQUIRE(objects.TryPushBack(created.Value()));
            }

            Array<ObjectHandle> links(allocator);
            REQUIRE(links.TryReserve(Links));
            for (usize index = 0; index < Links; ++index) {
                REQUIRE(links.TryPushBack(objects[1 + index % (Kinds - 1)]));
            }

            auto rootRef = transaction.Get<Ref>(objects[0]);
            REQUIRE(rootRef);
            REQUIRE(transaction.AssignReferences(objects[0], rootRef.Value()->links, links.View()));
            auto root = transaction.Commit();
            REQUIRE(root);
            auto bytes =
                SaveObjectGraph(allocator, source, source.OwnerAccess(), root.Value().Handle());
            REQUIRE(bytes);
            ObjectGraphLoadOptions load;
            load.limits.maxWork = 6 * 1024 * 1024;
            auto loaded = LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                          bytes.Value().View(), load);
            REQUIRE(loaded);
            CHECK(target.Stats().liveObjects == Kinds);
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(target.Shutdown(target.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}
