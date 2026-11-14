#include <Core/Memory/SystemAllocator.h>
#include <Object/ConstructionContext.h>
#include <Object/GeneratedFieldCodec.h>
#include <ObjectSerialization/ObjectGraph.h>
#include <TestSupport/Test.h>

// 누락 = 기준·실효 값 Required 진단(Docs/Design/ObjectFoundations.ko.md 4절 — A0-Oc, graph v1
// envelope 위)의 그래프 시험이다. GeneratedType 특수화는 생성 코드와 같은 모양으로 손으로 썼다:
// Required 단일 참조·Soft(생성자 non-null·null)·참조 배열, 참조 struct 원소 배열(원소 안 Required
// 참조), 참조 struct 필드, 참조 없는 struct 필드·원소 배열(빠진 멤버의 기준 값), MaxElements 배열,
// Required Soft를 담은 참조 없는 struct(검사 모드의 치환 생략), EditorOnly 단일 값·Required 참조.
// 파일에 없는 값은 Subject·Note 스키마를 줄인 변형 등록으로 저장해 만든다(바이트를 손으로 만들지
// 않는다 — 형식이 바뀌어도 그대로 쓴다).
using namespace rocks;

namespace required_graph_fixture {

struct Subject;

// 참조 struct다(StructRefArray 원소·struct 필드). target은 Required다.
struct Pin {
    ObjectRef<Subject> target;
    uint32 tag = 0;
};

// 참조 없는 struct다(struct 필드·원소 배열). b의 생성자 값 9가 기준이고 hint는 EditorOnly다.
struct Note {
    uint32 a = 0;
    uint32 b = 9;
    uint32 hint = 0;
};

// Required Soft 참조를 담은 참조 없는 struct다(치환 hook 대상).
struct Badge {
    explicit Badge(Allocator& allocator) noexcept : soft(allocator) {
    }

    SoftObjectRef<> soft;
};

constexpr AssetId DefaultIcon{Guid{0x49434f4e, 1}};
constexpr AssetId BadgeAsset{Guid{0x42414447, 2}};

struct Subject final : Object {
    explicit Subject(Allocator& allocator) noexcept
        : icon(allocator), emblem(allocator), refs(allocator), pins(allocator), notes(allocator),
          values(allocator), badge(allocator) {
        // 생성자가 non-null Soft 참조를 기준 값으로 둔다(빈 경로 = 대표 에셋).
        icon.asset = DefaultIcon;
        // 빠진 배열의 기준 값이 "생성자 값 유지"인지 "비움"인지 가리도록 원소 하나를 넣는다(시험
        // fixture — SystemAllocator에서 실패하지 않으며, 실패하면 빈 배열이 기준이 되어 시험이
        // 드러낸다).
        (void)values.TryPushBack(5u);
        ++Constructed;
    }

    void Trace(TraceVisitor& visitor) const noexcept;

    static inline uint32 Constructed = 0;
    ObjectRef<Subject> link;
    SoftObjectRef<> icon;
    SoftObjectRef<> emblem;
    ObjectRefArray<Subject> refs;
    StructRefArray<Pin> pins;
    Pin pin;
    Note note;
    Array<Note> notes;
    Array<uint32> values;
    uint32 count = 77;
    Badge badge;
    uint32 memo = 0;
    ObjectRef<Subject> editorLink;
};

// 수동 함수표(범용 codec) 타입이다. owner는 Required다.
struct Manual final : Object {
    void Trace(TraceVisitor& visitor) const noexcept {
        visitor.Visit(owner);
    }

    ObjectRef<Object> owner;
};

constexpr TypeId SubjectId{0x524751, 1};
constexpr TypeId PinId{0x524751, 2};
constexpr TypeId NoteId{0x524751, 3};
constexpr TypeId BadgeId{0x524751, 4};
constexpr TypeId ManualId{0x524751, 5};

constexpr Guid LinkId{0x524746, 1};
constexpr Guid IconId{0x524746, 2};
constexpr Guid EmblemId{0x524746, 3};
constexpr Guid RefsId{0x524746, 4};
constexpr Guid PinsId{0x524746, 5};
constexpr Guid PinFieldId{0x524746, 6};
constexpr Guid NoteFieldId{0x524746, 7};
constexpr Guid NotesId{0x524746, 8};
constexpr Guid ValuesId{0x524746, 9};
constexpr Guid CountId{0x524746, 10};
constexpr Guid BadgeFieldId{0x524746, 11};
constexpr Guid MemoId{0x524746, 12};
constexpr Guid EditorLinkId{0x524746, 13};
constexpr Guid GhostId{0x524746, 14};
constexpr Guid TargetId{0x52474d, 1};
constexpr Guid TagId{0x52474d, 2};
constexpr Guid NoteAId{0x52474d, 3};
constexpr Guid NoteBId{0x52474d, 4};
constexpr Guid NoteHintId{0x52474d, 5};
constexpr Guid BadgeSoftId{0x52474d, 6};
constexpr Guid OwnerId{0x52474d, 7};

constexpr uint32 EditorOnly = static_cast<uint32>(FieldFlags::EditorOnly);
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

} // namespace required_graph_fixture

namespace rocks {

template <>
struct GeneratedType<required_graph_fixture::Pin> {
    static constexpr bool ContainsStrongReference = true;
    static constexpr bool ContainsReferenceArray = false;

    static constexpr TypeId Id() noexcept {
        return required_graph_fixture::PinId;
    }

    static void Trace(const required_graph_fixture::Pin& value, TraceVisitor& visitor) noexcept {
        visitor.Visit(value.target);
    }
};

template <>
struct GeneratedType<required_graph_fixture::Note> {
    static constexpr bool ContainsStrongReference = false;
    static constexpr bool ContainsReferenceArray = false;

    static constexpr TypeId Id() noexcept {
        return required_graph_fixture::NoteId;
    }

    static void Trace(const required_graph_fixture::Note&, TraceVisitor&) noexcept {
    }
};

template <>
struct GeneratedType<required_graph_fixture::Badge> {
    static constexpr bool ContainsStrongReference = false;
    static constexpr bool ContainsReferenceArray = false;

    static constexpr TypeId Id() noexcept {
        return required_graph_fixture::BadgeId;
    }

    static void Trace(const required_graph_fixture::Badge&, TraceVisitor&) noexcept {
    }
};

} // namespace rocks

void required_graph_fixture::Subject::Trace(TraceVisitor& visitor) const noexcept {
    visitor.Visit(link);
    refs.Trace(visitor);
    pins.Trace(visitor);
    GeneratedType<Pin>::Trace(pin, visitor);
    visitor.Visit(editorLink);
}

namespace {

using namespace required_graph_fixture;

// 클래스의 struct·배열 필드 함수표다(생성 코드와 같은 모양 — read·write·writeStaged 없음).
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

// 단일 값 필드 함수표다(생성 코드와 같은 모양).
template <auto Member>
RuntimeFieldOps ScalarField(Guid id) noexcept {
    using O = typename detail::StructMemberPointer<decltype(Member)>::Owner;
    RuntimeFieldOps ops = CodecField<Member>(id);
    ops.read = [](ObjectRegistry& registry, const ObjectAccessContext& access, ObjectHandle object,
                  FieldValue& value) noexcept {
        return detail::ReadGeneratedField<O>(registry, access, object, Member, value);
    };
    ops.write = [](ObjectRegistry& registry, const ObjectAccessContext& access, ObjectHandle object,
                   const FieldValue& value) noexcept {
        return detail::WriteGeneratedField<O>(registry, access, object, Member, value);
    };
    ops.writeStaged = [](ConstructionContext& transaction, ObjectHandle object,
                         const FieldValue& value) noexcept {
        return detail::WriteGeneratedStagedField<O>(transaction, object, Member, value);
    };
    return ops;
}

// 등록 변형이다. omit은 Subject에서 뺄 필드, noteWithoutB는 Note에서 b를 빼고, ghost는 대상
// 스키마에 없는 필드(GhostId — count를 다시 쓴다)를 Subject에 더하며, refsMax는 refs의
// MaxElements다.
struct FixtureOptions {
    Span<const Guid> omit{};
    bool noteWithoutB = false;
    bool ghost = false;
    uint32 refsMax = DefaultMaxElements;
};

bool Omitted(Span<const Guid> omit, Guid id) noexcept {
    for (Guid candidate : omit) {
        if (candidate == id) {
            return true;
        }
    }

    return false;
}

bool RegisterStructs(TypeRegistry& types, bool noteWithoutB) noexcept {
    const FieldSchema pinFields[] = {
        Scalar(TargetId, "target", ValueKind::ObjectReference, SubjectId, Required),
        Scalar(TagId, "tag", ValueKind::UInt32)};
    const RuntimeStructFieldOps pinOps[] = {detail::StructFieldOps<&Pin::target>(TargetId),
                                            detail::StructFieldOps<&Pin::tag>(TagId)};
    RuntimeStructOps pin = MakeRuntimeStructOps<Pin>();
    pin.trace = [](const void* value, TraceVisitor& visitor) noexcept {
        GeneratedType<Pin>::Trace(*static_cast<const Pin*>(value), visitor);
    };
    pin.fields = pinOps;

    const FieldSchema noteFields[] = {Scalar(NoteAId, "a", ValueKind::UInt32),
                                      Scalar(NoteHintId, "hint", ValueKind::UInt32, {}, EditorOnly),
                                      Scalar(NoteBId, "b", ValueKind::UInt32)};
    const RuntimeStructFieldOps noteOps[] = {detail::StructFieldOps<&Note::a>(NoteAId),
                                             detail::StructFieldOps<&Note::hint>(NoteHintId),
                                             detail::StructFieldOps<&Note::b>(NoteBId)};
    const usize noteCount = noteWithoutB ? 2 : 3;
    RuntimeStructOps note = MakeRuntimeStructOps<Note>();
    note.fields = Span<const RuntimeStructFieldOps>(noteOps, noteCount);

    const FieldSchema badgeFields[] = {
        Scalar(BadgeSoftId, "soft", ValueKind::SoftObjectReference, {}, Required)};
    const RuntimeStructFieldOps badgeOps[] = {detail::StructFieldOps<&Badge::soft>(BadgeSoftId)};
    RuntimeStructOps badge = MakeRuntimeStructOps<Badge>();
    badge.fields = badgeOps;

    return types.RegisterStruct({PinId, 1, pinFields, {"Pin", 3}}, Move(pin)).IsOk() &&
           types
               .RegisterStruct(
                   {NoteId, 1, Span<const FieldSchema>(noteFields, noteCount), {"Note", 4}},
                   Move(note))
               .IsOk() &&
           types.RegisterStruct({BadgeId, 1, badgeFields, {"Badge", 5}}, Move(badge)).IsOk();
}

bool RegisterFixture(TypeRegistry& types, FixtureOptions options = {}) noexcept {
    if (!RegisterStructs(types, options.noteWithoutB)) {
        return false;
    }

    const FieldSchema all[] = {
        Scalar(LinkId, "link", ValueKind::ObjectReference, SubjectId, Required),
        Scalar(IconId, "icon", ValueKind::SoftObjectReference, {}, Required),
        Scalar(EmblemId, "emblem", ValueKind::SoftObjectReference, {}, Required),
        ArrayOf(RefsId, "refs", ValueKind::ObjectReference, SubjectId, options.refsMax, Required),
        ArrayOf(PinsId, "pins", ValueKind::Struct, PinId),
        Scalar(PinFieldId, "pin", ValueKind::Struct, PinId),
        Scalar(NoteFieldId, "note", ValueKind::Struct, NoteId),
        ArrayOf(NotesId, "notes", ValueKind::Struct, NoteId),
        ArrayOf(ValuesId, "values", ValueKind::UInt32, {}, 2),
        Scalar(CountId, "count", ValueKind::UInt32),
        Scalar(BadgeFieldId, "badge", ValueKind::Struct, BadgeId),
        Scalar(MemoId, "memo", ValueKind::UInt32, {}, EditorOnly),
        Scalar(EditorLinkId, "editorLink", ValueKind::ObjectReference, SubjectId,
               EditorOnly | Required),
        Scalar(GhostId, "ghost", ValueKind::UInt32)};
    const RuntimeFieldOps allOps[] = {
        ScalarField<&Subject::link>(LinkId),
        ScalarField<&Subject::icon>(IconId),
        ScalarField<&Subject::emblem>(EmblemId),
        CodecField<&Subject::refs>(RefsId),
        CodecField<&Subject::pins>(PinsId, NativeTypeKey<Pin>()),
        CodecField<&Subject::pin>(PinFieldId, NativeTypeKey<Pin>()),
        CodecField<&Subject::note>(NoteFieldId, NativeTypeKey<Note>()),
        CodecField<&Subject::notes>(NotesId, NativeTypeKey<Note>()),
        CodecField<&Subject::values>(ValuesId),
        ScalarField<&Subject::count>(CountId),
        CodecField<&Subject::badge>(BadgeFieldId, NativeTypeKey<Badge>()),
        ScalarField<&Subject::memo>(MemoId),
        ScalarField<&Subject::editorLink>(EditorLinkId),
        ScalarField<&Subject::count>(GhostId)};
    constexpr usize Total = sizeof(all) / sizeof(all[0]);
    FieldSchema fields[Total];
    RuntimeFieldOps ops[Total];
    usize count = 0;
    for (usize index = 0; index < Total; ++index) {
        const bool ghost = all[index].id == GhostId;
        if ((ghost && !options.ghost) || (!ghost && Omitted(options.omit, all[index].id))) {
            continue;
        }

        fields[count] = all[index];
        ops[count] = allOps[index];
        ++count;
    }

    RuntimeTypeOps typeOps = MakeRuntimeTypeOps<Subject>();
    typeOps.fields = Span<const RuntimeFieldOps>(ops, count);
    typeOps.completeFieldLayout = true;
    return types
        .Register({SubjectId, 1, Span<const FieldSchema>(fields, count), {"Subject", 7}},
                  Move(typeOps))
        .IsOk();
}

// 루트 A와 A가 가리키는 B를 만든다. A: refs = {B, null, B}(refCount가 2면 {B, null}), pins 둘
// ([0].target = B, [1].target null), note·notes 둘(a·b·hint를 채운다), values = {1, 2}, count = 12,
// badge.soft non-null, memo = 5. link·emblem·editorLink는 생성자 값(null)이고 B는 모두 생성자
// 값이다.
Result<RootRef<Object>> MakeGraph(ObjectRegistry& objects, usize refCount = 3) noexcept {
    ConstructionContext transaction(objects, objects.OwnerAccess());
    auto a = transaction.Create<Subject>(SubjectId);
    auto b = transaction.Create<Subject>(SubjectId);
    if (!a || !b) {
        return Result<RootRef<Object>>::Failure(!a ? a.Error() : b.Error());
    }

    auto owner = transaction.Get<Subject>(a.Value());
    if (!owner) {
        return Result<RootRef<Object>>::Failure(owner.Error());
    }

    Subject& value = *owner.Value();
    const ObjectHandle refs[] = {b.Value(), {}, b.Value()};
    if (auto assigned = transaction.AssignReferences(a.Value(), value.refs,
                                                     Span<const ObjectHandle>(refs, refCount));
        !assigned) {
        return Result<RootRef<Object>>::Failure(assigned.Error());
    }

    if (auto assigned = transaction.AssignElements(a.Value(), value.pins, 2); !assigned) {
        return Result<RootRef<Object>>::Failure(assigned.Error());
    }

    if (auto linked = transaction.SetElementReference(a.Value(), value.pins, 0,
                                                      value.pins[0].target, b.Value());
        !linked) {
        return Result<RootRef<Object>>::Failure(linked.Error());
    }

    value.pins[1].tag = 7;
    value.note.a = 4;
    value.note.b = 33;
    value.note.hint = 6;
    for (uint32 index = 0; index < 2; ++index) {
        auto added = value.notes.TryEmplaceBack();
        if (!added) {
            return Result<RootRef<Object>>::Failure(added.Error());
        }

        added.Value()->a = index + 1;
        added.Value()->b = 40 + index;
        added.Value()->hint = 2;
    }

    value.values.Clear();
    if (!value.values.TryPushBack(1u) || !value.values.TryPushBack(2u) ||
        !value.badge.soft.TryAssign(BadgeAsset, "Badge")) {
        return Result<RootRef<Object>>::Failure(ErrorCode::OutOfMemory);
    }

    value.count = 12;
    value.memo = 5;
    return transaction.Commit();
}

Result<Array<uint8>> SaveFixture(Allocator& allocator, ObjectRegistry& objects,
                                 ObjectGraphSaveOptions options = {}, usize refCount = 3) noexcept {
    auto root = MakeGraph(objects, refCount);
    if (!root) {
        return Result<Array<uint8>>::Failure(root.Error());
    }

    return SaveObjectGraph(allocator, objects, objects.OwnerAccess(), root.Value().Handle(),
                           options);
}

// 진단·Soft 수집·치환을 기록한다. 진단마다 그 순간의 Registry reservedObjects를 남긴다(1단계는
// 생성 전이라 0, 적용 뒤 Required 검사는 노드 수).
class HookLog final : public FieldCodecHooks {
public:
    struct Entry {
        FieldDiagnosticKind kind = FieldDiagnosticKind::UnknownField;
        uint32 objectIndex = 0;
        usize depth = 0;
        PathStep path[4] = {};
        uint32 reserved = 0;
    };

    Result<void> Report(const FieldDiagnostic& diagnostic) noexcept override {
        if (count < 32) {
            Entry& entry = entries[count];
            entry.kind = diagnostic.kind;
            entry.objectIndex = diagnostic.objectIndex;
            entry.depth = diagnostic.path.Size();
            for (usize index = 0; index < entry.depth && index < 4; ++index) {
                entry.path[index] = diagnostic.path[index];
            }

            entry.reserved = objects ? objects->Stats().reservedObjects : 0;
        }

        ++count;
        if (failRequired && diagnostic.kind == FieldDiagnosticKind::RequiredMissing) {
            return Result<void>::Failure(ErrorCode::DependencyFailed);
        }

        return Result<void>::Success();
    }

    Result<void> CollectSoftReference(const SoftReferenceInfo&) noexcept override {
        ++softs;
        return Result<void>::Success();
    }

    Result<const void*> SubstituteStruct(const FieldCodecFrame&, TypeId, const void*,
                                         const void* in) noexcept override {
        ++substitutions;
        return Result<const void*>::Success(in);
    }

    usize Count(FieldDiagnosticKind kind) const noexcept {
        usize found = 0;
        for (usize index = 0; index < count && index < 32; ++index) {
            found += entries[index].kind == kind;
        }

        return found;
    }

    // RequiredMissing 중 (objectIndex, 경로)가 같은 진단의 수다. element는 첫 단계의 원소 index
    // (없으면 IndexNone), member는 둘째 단계 필드(없으면 Guid{} — 경로 길이 1).
    usize RequiredAt(uint32 objectIndex, Guid field, usize element = IndexNone,
                     Guid member = {}) const noexcept {
        usize found = 0;
        for (usize index = 0; index < count && index < 32; ++index) {
            const Entry& entry = entries[index];
            const bool indexed = element != IndexNone;
            found += entry.kind == FieldDiagnosticKind::RequiredMissing &&
                     entry.objectIndex == objectIndex && entry.path[0].fieldId == field &&
                     entry.path[0].indexed == indexed &&
                     (!indexed || entry.path[0].index == element) &&
                     entry.depth == (member == Guid{} ? 1u : 2u) &&
                     (member == Guid{} || entry.path[1].fieldId == member);
        }

        return found;
    }

    ObjectRegistry* objects = nullptr;
    Entry entries[32];
    usize count = 0;
    usize softs = 0;
    usize substitutions = 0;
    bool failRequired = false;
};

} // namespace

// Record에 없는 필드(count)·struct 멤버(note.b)·배열 원소 struct 멤버(notes[i].b)·배열
// 필드(values — 생성자가 넣은 원소 5를 유지하고 비우지 않는다)는 정책 없이 생성자 값(기준)을
// 유지하고(NotFound 없음), 파일에 있는 값은 그대로 읽는다.
TEST(RequiredCheckGraph_MissingValuesKeepBaseValues) {
    SystemAllocator allocator;
    const Guid omitted[] = {CountId, ValuesId};
    FixtureOptions sparse;
    sparse.omit = omitted;
    sparse.noteWithoutB = true;
    TypeRegistry sourceTypes(allocator);
    TypeRegistry targetTypes(allocator);
    REQUIRE(RegisterFixture(sourceTypes, sparse));
    REQUIRE(RegisterFixture(targetTypes));
    {
        ObjectRegistry source(allocator, sourceTypes, {0x5247, 1});
        ObjectRegistry target(allocator, targetTypes, {0x5247, 2});
        {
            auto bytes = SaveFixture(allocator, source);
            REQUIRE(bytes);
            auto loaded = LoadObjectGraph(allocator, target, targetTypes, target.OwnerAccess(),
                                          bytes.Value().View());
            REQUIRE(loaded);
            auto borrowed = target.Resolve<Subject>(target.OwnerAccess(), loaded.Value().Handle());
            REQUIRE(borrowed);
            const Subject& value = *borrowed.Value().Get().Value();
            CHECK(value.count == 77);
            CHECK(value.values.Size() == 1 && value.values[0] == 5);
            CHECK(value.note.a == 4 && value.note.b == 9 && value.note.hint == 6);
            REQUIRE(value.notes.Size() == 2);
            CHECK(value.notes[0].a == 1 && value.notes[0].b == 9);
            CHECK(value.notes[1].a == 2 && value.notes[1].b == 9);
            CHECK(value.pins.Size() == 2 && value.pins[1].tag == 7);
            CHECK(value.icon.asset == DefaultIcon);
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(target.Shutdown(target.OwnerAccess()));
    }

    CHECK(sourceTypes.Shutdown());
    CHECK(targetTypes.Shutdown());
}

// Required 진단은 적용 뒤 staged 객체의 실효 값으로 한 번씩 온다(1단계에서는 없다 — 모르는 필드
// 진단은 생성 전에, RequiredMissing은 모든 노드를 만든 뒤에 온다): 빠진 ObjectRef(link), 생성자
// null Soft의 누락(emblem), 빠진 struct 안 Required 멤버([pin, target]), Required ObjectRefArray의
// null 원소([refs[1]]), StructRefArray 원소 안 Required 참조([pins[1], target]), 파일 null
// (editorLink, B의 badge.soft). 생성자 non-null Soft의 누락(icon)은 진단하지 않는다. Soft 수집은
// 1단계의 파일 값만 한 번이고 검사는 치환 hook을 부르지 않는다. hook이 실패하면 아무것도 공개하지
// 않고 rollback한다.
TEST(RequiredCheckGraph_EffectiveValuesAreDiagnosedOnceAfterApply) {
    SystemAllocator allocator;
    const Guid omitted[] = {LinkId, IconId, EmblemId, PinFieldId};
    FixtureOptions sparse;
    sparse.omit = omitted;
    sparse.ghost = true;
    TypeRegistry sourceTypes(allocator);
    TypeRegistry targetTypes(allocator);
    REQUIRE(RegisterFixture(sourceTypes, sparse));
    REQUIRE(RegisterFixture(targetTypes));
    {
        ObjectRegistry source(allocator, sourceTypes, {0x5247, 3});
        ObjectRegistry target(allocator, targetTypes, {0x5247, 4});
        {
            auto bytes = SaveFixture(allocator, source);
            REQUIRE(bytes);
            HookLog hooks;
            hooks.objects = &target;
            ObjectGraphLoadOptions load;
            load.hooks = &hooks;
            {
                auto loaded = LoadObjectGraph(allocator, target, targetTypes, target.OwnerAccess(),
                                              bytes.Value().View(), load);
                REQUIRE(loaded);
            }

            REQUIRE(hooks.count <= 32);
            CHECK(hooks.Count(FieldDiagnosticKind::UnknownField) == 2);
            CHECK(hooks.Count(FieldDiagnosticKind::RequiredMissing) == 11);
            usize firstRequired = IndexNone;
            usize lastUnknown = 0;
            bool afterApply = true;
            for (usize index = 0; index < hooks.count; ++index) {
                const HookLog::Entry& entry = hooks.entries[index];
                if (entry.kind == FieldDiagnosticKind::RequiredMissing) {
                    firstRequired = firstRequired == IndexNone ? index : firstRequired;
                    afterApply = afterApply && entry.reserved == 2;
                } else {
                    lastUnknown = index;
                    afterApply = afterApply && entry.reserved == 0;
                }
            }

            CHECK(afterApply);
            CHECK(firstRequired != IndexNone && lastUnknown < firstRequired);
            // A(노드 0)
            CHECK(hooks.RequiredAt(0, LinkId) == 1);
            CHECK(hooks.RequiredAt(0, EmblemId) == 1);
            CHECK(hooks.RequiredAt(0, IconId) == 0);
            CHECK(hooks.RequiredAt(0, PinFieldId, IndexNone, TargetId) == 1);
            CHECK(hooks.RequiredAt(0, RefsId, 1) == 1);
            CHECK(hooks.RequiredAt(0, RefsId, 0) == 0);
            CHECK(hooks.RequiredAt(0, PinsId, 1, TargetId) == 1);
            CHECK(hooks.RequiredAt(0, PinsId, 0, TargetId) == 0);
            CHECK(hooks.RequiredAt(0, EditorLinkId) == 1);
            CHECK(hooks.RequiredAt(0, BadgeFieldId, IndexNone, BadgeSoftId) == 0);
            // B(노드 1)
            CHECK(hooks.RequiredAt(1, LinkId) == 1);
            CHECK(hooks.RequiredAt(1, EmblemId) == 1);
            CHECK(hooks.RequiredAt(1, PinFieldId, IndexNone, TargetId) == 1);
            CHECK(hooks.RequiredAt(1, EditorLinkId) == 1);
            CHECK(hooks.RequiredAt(1, BadgeFieldId, IndexNone, BadgeSoftId) == 1);
            // 파일의 non-null Soft는 A.badge.soft 하나다(검사는 수집하지 않는다).
            CHECK(hooks.softs == 1);
            CHECK(hooks.substitutions == 0);

            REQUIRE(target.Collect(target.OwnerAccess()));
            const uint32 constructed = Subject::Constructed;
            HookLog failing;
            failing.failRequired = true;
            load.hooks = &failing;
            CHECK(LoadObjectGraph(allocator, target, targetTypes, target.OwnerAccess(),
                                  bytes.Value().View(), load)
                      .Error() == ErrorCode::DependencyFailed);
            CHECK(Subject::Constructed == constructed + 2);
            CHECK(failing.Count(FieldDiagnosticKind::RequiredMissing) == 1);
            CHECK(target.Stats().liveObjects == 0 && target.Stats().reservedObjects == 0);
            CHECK(target.Stats().roots == 0);
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(target.Shutdown(target.OwnerAccess()));
    }

    CHECK(sourceTypes.Shutdown());
    CHECK(targetTypes.Shutdown());
}

// 쿠킹 로드(excludedFlags = EditorOnly): EditorOnly 값이 없으면 기준 값이고 그 Required 참조
// (editorLink)는 null이어도 진단하지 않는다. EditorOnly 최상위 필드(memo·editorLink), struct 필드
// 멤버(note.hint), 배열 원소 struct 멤버(notes[i].hint — note 필드 없이 따로)가 있으면 생성자 전에
// InvalidArgument다.
TEST(RequiredCheckGraph_ExcludedFlagsRejectPresenceAndSkipRequired) {
    SystemAllocator allocator;
    const Guid topLevel[] = {MemoId, EditorLinkId};
    FixtureOptions memberOnly;
    memberOnly.omit = topLevel;
    const Guid elementOnlyOmit[] = {MemoId, EditorLinkId, NoteFieldId};
    FixtureOptions elementOnly;
    elementOnly.omit = elementOnlyOmit;
    TypeRegistry types(allocator);
    TypeRegistry memberTypes(allocator);
    TypeRegistry elementTypes(allocator);
    REQUIRE(RegisterFixture(types));
    REQUIRE(RegisterFixture(memberTypes, memberOnly));
    REQUIRE(RegisterFixture(elementTypes, elementOnly));
    {
        ObjectRegistry source(allocator, types, {0x5247, 5});
        ObjectRegistry members(allocator, memberTypes, {0x5247, 6});
        ObjectRegistry elements(allocator, elementTypes, {0x5247, 15});
        ObjectRegistry target(allocator, types, {0x5247, 7});
        {
            ObjectGraphSaveOptions cookedSave;
            cookedSave.policy.excludedFlags = EditorOnly;
            auto cooked = SaveFixture(allocator, source, cookedSave);
            REQUIRE(cooked);
            HookLog hooks;
            ObjectGraphLoadOptions cookedLoad;
            cookedLoad.policy.excludedFlags = EditorOnly;
            cookedLoad.hooks = &hooks;
            {
                auto loaded = LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                              cooked.Value().View(), cookedLoad);
                REQUIRE(loaded);
                auto borrowed =
                    target.Resolve<Subject>(target.OwnerAccess(), loaded.Value().Handle());
                REQUIRE(borrowed);
                const Subject& value = *borrowed.Value().Get().Value();
                CHECK(value.memo == 0 && value.note.hint == 0);
                CHECK(value.notes.Size() == 2 && value.notes[0].hint == 0);
                CHECK(value.note.b == 33 && value.count == 12);
            }

            CHECK(hooks.RequiredAt(0, EditorLinkId) == 0 && hooks.RequiredAt(1, EditorLinkId) == 0);
            CHECK(hooks.RequiredAt(0, LinkId) == 1);

            const uint32 constructed = Subject::Constructed;
            auto editor = SaveFixture(allocator, source);
            REQUIRE(editor);
            auto memberBytes = SaveFixture(allocator, members);
            REQUIRE(memberBytes);
            auto elementBytes = SaveFixture(allocator, elements);
            REQUIRE(elementBytes);
            const uint32 before = Subject::Constructed;
            CHECK(before == constructed + 6);
            CHECK(LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                  editor.Value().View(), cookedLoad)
                      .Error() == ErrorCode::InvalidArgument);
            CHECK(LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                  memberBytes.Value().View(), cookedLoad)
                      .Error() == ErrorCode::InvalidArgument);
            CHECK(LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                  elementBytes.Value().View(), cookedLoad)
                      .Error() == ErrorCode::InvalidArgument);
            // 같은 원소 멤버 데이터도 쿠킹이 아닌 로드는 받는다(거부 사유가 excludedFlags다).
            ObjectGraphLoadOptions editorLoad;
            CHECK(LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                  elementBytes.Value().View(), editorLoad)
                      .IsOk());
            CHECK(Subject::Constructed == before + 2);
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(members.Shutdown(members.OwnerAccess()));
        REQUIRE(elements.Shutdown(elements.OwnerAccess()));
        REQUIRE(target.Shutdown(target.OwnerAccess()));
    }

    CHECK(types.Shutdown());
    CHECK(memberTypes.Shutdown());
    CHECK(elementTypes.Shutdown());
}

// 검사 모드는 저장 전용 규칙을 쓰지 않는다: 대상의 MaxElements(2)를 넘는 Required 배열(refs 3개)을
// 가진 파일도 열리고(ArrayTooLong 없음 — 그 배열의 null 원소는 진단한다), Required를 품은 참조 없는
// struct(badge)를 검사해도 치환 hook이 불리지 않으며, Soft 수집은 파일의 non-null 값마다 1단계에서
// 한 번이다(A.icon·B.icon·A.badge.soft).
TEST(RequiredCheckGraph_CheckModeSkipsSaveOnlyRules) {
    SystemAllocator allocator;
    FixtureOptions narrow;
    narrow.refsMax = 2;
    TypeRegistry sourceTypes(allocator);
    TypeRegistry targetTypes(allocator);
    REQUIRE(RegisterFixture(sourceTypes));
    REQUIRE(RegisterFixture(targetTypes, narrow));
    {
        ObjectRegistry source(allocator, sourceTypes, {0x5247, 8});
        ObjectRegistry target(allocator, targetTypes, {0x5247, 9});
        {
            auto bytes = SaveFixture(allocator, source);
            REQUIRE(bytes);
            HookLog hooks;
            ObjectGraphLoadOptions load;
            load.hooks = &hooks;
            auto loaded = LoadObjectGraph(allocator, target, targetTypes, target.OwnerAccess(),
                                          bytes.Value().View(), load);
            REQUIRE(loaded);
            CHECK(hooks.Count(FieldDiagnosticKind::ArrayTooLong) == 0);
            CHECK(hooks.RequiredAt(0, RefsId, 1) == 1);
            CHECK(hooks.RequiredAt(1, BadgeFieldId, IndexNone, BadgeSoftId) == 1);
            CHECK(hooks.substitutions == 0);
            CHECK(hooks.softs == 3);
            {
                auto borrowed =
                    target.Resolve<Subject>(target.OwnerAccess(), loaded.Value().Handle());
                REQUIRE(borrowed);
                CHECK(borrowed.Value().Get().Value()->refs.Size() == 3);
            }

            // 대상 스키마로 다시 저장하면 그때 MaxElements를 건다.
            HookLog saveHooks;
            ObjectGraphSaveOptions save;
            save.hooks = &saveHooks;
            CHECK(SaveObjectGraph(allocator, target, target.OwnerAccess(), loaded.Value().Handle(),
                                  save)
                      .Error() == ErrorCode::CapacityExceeded);
            CHECK(saveHooks.Count(FieldDiagnosticKind::ArrayTooLong) == 1);
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(target.Shutdown(target.OwnerAccess()));
    }

    CHECK(sourceTypes.Shutdown());
    CHECK(targetTypes.Shutdown());
}

namespace {

// 할당 호출 수를 세는 할당기다.
class CountingAllocator final : public Allocator {
public:
    explicit CountingAllocator(Allocator& inner) noexcept : inner_(inner) {
    }

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override {
        ++allocations;
        return inner_.Allocate(bytes, alignment, tag);
    }

    Result<void> Deallocate(void* value) noexcept override {
        return inner_.Deallocate(value);
    }

    uint64 allocations = 0;

private:
    Allocator& inner_;
};

// 루트 A가 refs로 B를 count개 가리키는 그래프를 저장한다(B는 모두 생성자 값).
Result<Array<uint8>> SaveWide(Allocator& allocator, ObjectRegistry& objects, usize count) noexcept {
    ConstructionContext transaction(objects, objects.OwnerAccess());
    auto a = transaction.Create<Subject>(SubjectId);
    if (!a) {
        return Result<Array<uint8>>::Failure(a.Error());
    }

    Array<ObjectHandle> targets(allocator);
    if (auto reserved = targets.TryReserve(count); !reserved) {
        return Result<Array<uint8>>::Failure(reserved.Error());
    }

    for (usize index = 0; index < count; ++index) {
        auto b = transaction.Create<Subject>(SubjectId);
        if (!b) {
            return Result<Array<uint8>>::Failure(b.Error());
        }

        if (auto pushed = targets.TryPushBack(b.Value()); !pushed) {
            return Result<Array<uint8>>::Failure(pushed.Error());
        }
    }

    auto owner = transaction.Get<Subject>(a.Value());
    if (!owner) {
        return Result<Array<uint8>>::Failure(owner.Error());
    }

    if (auto assigned =
            transaction.AssignReferences(a.Value(), owner.Value()->refs, targets.View());
        !assigned) {
        return Result<Array<uint8>>::Failure(assigned.Error());
    }

    auto root = transaction.Commit();
    if (!root) {
        return Result<Array<uint8>>::Failure(root.Error());
    }

    return SaveObjectGraph(allocator, objects, objects.OwnerAccess(), root.Value().Handle());
}

} // namespace

// 검사 writer는 필드마다 다시 쓰인다: 노드 41개 × Required를 품은 필드 여럿을 검사해도 검사가
// 더하는 할당은 writer를 처음 잡는 몇 번뿐이다(파일의 Required를 품은 가장 큰 필드 — A.refs —
// 크기로 미리 잡아 그보다 작은 다른 출력은 다시 할당하지 않는다).
TEST(RequiredCheckGraph_CheckWriterIsReusedAcrossFields) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry source(allocator, types, {0x5247, 16});
        ObjectRegistry target(allocator, types, {0x5247, 17});
        {
            auto bytes = SaveWide(allocator, source, 40);
            REQUIRE(bytes);
            uint64 counts[2] = {};
            usize reports = 0;
            for (usize pass = 0; pass < 2; ++pass) {
                CountingAllocator counting(allocator);
                HookLog hooks;
                ObjectGraphLoadOptions load;
                load.hooks = pass == 0 ? nullptr : &hooks;
                {
                    auto loaded = LoadObjectGraph(counting, target, types, target.OwnerAccess(),
                                                  bytes.Value().View(), load);
                    REQUIRE(loaded);
                }

                counts[pass] = counting.allocations;
                reports = hooks.count;
                REQUIRE(target.Collect(target.OwnerAccess()));
            }

            // 노드마다 link·emblem·pin.target·editorLink·badge.soft가 null이다(41 × 5).
            CHECK(reports == 41 * 5);
            CHECK(counts[1] >= counts[0]);
            CHECK(counts[1] - counts[0] <= 2);
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(target.Shutdown(target.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}

// 검사 writer는 크기 한도가 없다: 파일 값은 maxValueBytes(32) 안이지만 빠진 필드의 기준 값(생성자가
// 만든 struct — pin·badge의 중첩 Record)은 그보다 커도 검사되어 진단이 온다.
TEST(RequiredCheckGraph_CheckWriterIgnoresValueLimits) {
    SystemAllocator allocator;
    const Guid omitted[] = {LinkId,  IconId,      EmblemId, RefsId,       PinsId, PinFieldId,
                            NotesId, NoteFieldId, ValuesId, BadgeFieldId, MemoId, EditorLinkId};
    FixtureOptions countOnly;
    countOnly.omit = omitted;
    TypeRegistry sourceTypes(allocator);
    TypeRegistry targetTypes(allocator);
    REQUIRE(RegisterFixture(sourceTypes, countOnly));
    REQUIRE(RegisterFixture(targetTypes));
    {
        ObjectRegistry source(allocator, sourceTypes, {0x5247, 10});
        ObjectRegistry target(allocator, targetTypes, {0x5247, 11});
        {
            auto bytes = SaveFixture(allocator, source);
            REQUIRE(bytes);
            HookLog hooks;
            ObjectGraphLoadOptions load;
            load.limits.maxValueBytes = 32;
            load.hooks = &hooks;
            auto loaded = LoadObjectGraph(allocator, target, targetTypes, target.OwnerAccess(),
                                          bytes.Value().View(), load);
            REQUIRE(loaded);
            CHECK(hooks.RequiredAt(0, PinFieldId, IndexNone, TargetId) == 1);
            CHECK(hooks.RequiredAt(0, BadgeFieldId, IndexNone, BadgeSoftId) == 1);
            CHECK(hooks.RequiredAt(0, LinkId) == 1);
            CHECK(hooks.Count(FieldDiagnosticKind::RequiredMissing) == 5);
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(target.Shutdown(target.OwnerAccess()));
    }

    CHECK(sourceTypes.Shutdown());
    CHECK(targetTypes.Shutdown());
}

namespace {

Result<void> ReadOwnerStaged(ConstructionContext& transaction, ObjectHandle object,
                             FieldValue& out) noexcept {
    auto owner = transaction.Get<Manual>(object);
    if (!owner) {
        return Result<void>::Failure(owner.Error());
    }

    out.kind = ValueKind::ObjectReference;
    out.reference = owner.Value()->owner.Handle();
    return Result<void>::Success();
}

// 수동 함수표(범용 codec)로 Manual을 등록한다. staged면 readStaged를 단다.
bool RegisterManual(TypeRegistry& types, bool staged) noexcept {
    const FieldSchema fields[] = {
        Scalar(OwnerId, "owner", ValueKind::ObjectReference, {}, Required)};
    RuntimeFieldOps ops = ScalarField<&Manual::owner>(OwnerId);
    ops.encode = &detail::EncodeThroughFieldValue;
    ops.decode = &detail::DecodeThroughFieldValue;
    ops.readStaged = staged ? &ReadOwnerStaged : nullptr;
    RuntimeTypeOps typeOps = MakeRuntimeTypeOps<Manual>();
    typeOps.fields = Span<const RuntimeFieldOps>(&ops, 1);
    typeOps.completeFieldLayout = true;
    return types.Register({ManualId, 1, fields, {"Manual", 6}}, Move(typeOps)).IsOk();
}

} // namespace

// 수동 함수표의 Required 필드는 staged 객체를 readStaged로 읽어 검사한다. 없으면 검사가
// Unsupported로 실패해 rollback하고, hooks가 없으면 검사하지 않으므로 그대로 열린다.
TEST(RequiredCheckGraph_ManualTablesNeedReadStaged) {
    SystemAllocator allocator;
    TypeRegistry stagedTypes(allocator);
    TypeRegistry bareTypes(allocator);
    REQUIRE(RegisterManual(stagedTypes, true));
    REQUIRE(RegisterManual(bareTypes, false));
    {
        ObjectRegistry source(allocator, bareTypes, {0x5247, 12});
        ObjectRegistry staged(allocator, stagedTypes, {0x5247, 13});
        ObjectRegistry bare(allocator, bareTypes, {0x5247, 14});
        {
            auto root = source.NewObject<Manual>(source.OwnerAccess(), ManualId);
            REQUIRE(root);
            auto bytes =
                SaveObjectGraph(allocator, source, source.OwnerAccess(), root.Value().Handle());
            REQUIRE(bytes);
            HookLog hooks;
            ObjectGraphLoadOptions load;
            load.hooks = &hooks;
            {
                auto loaded = LoadObjectGraph(allocator, staged, stagedTypes, staged.OwnerAccess(),
                                              bytes.Value().View(), load);
                REQUIRE(loaded);
                CHECK(hooks.RequiredAt(0, OwnerId) == 1);
            }

            CHECK(LoadObjectGraph(allocator, bare, bareTypes, bare.OwnerAccess(),
                                  bytes.Value().View(), load)
                      .Error() == ErrorCode::Unsupported);
            CHECK(bare.Stats().liveObjects == 0 && bare.Stats().reservedObjects == 0);
            auto unchecked = LoadObjectGraph(allocator, bare, bareTypes, bare.OwnerAccess(),
                                             bytes.Value().View());
            CHECK(unchecked.IsOk());
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(staged.Shutdown(staged.OwnerAccess()));
        REQUIRE(bare.Shutdown(bare.OwnerAccess()));
    }

    CHECK(stagedTypes.Shutdown());
    CHECK(bareTypes.Shutdown());
}
