#include <Core/Memory/SystemAllocator.h>
#include <Object/ConstructionContext.h>
#include <Object/GeneratedFieldCodec.h>
#include <Serialization/Archive.h>
#include <Serialization/Record.h>
#include <TestSupport/HostFailure.h>
#include <TestSupport/Test.h>

// 누락 = 기준·실효 값 Required 진단(Docs/Design/ObjectFoundations.ko.md 4절 — A0-Oc)의 codec 문맥
// 단위 시험이다(직렬화기 없이): present 판정(MarkPresent → NotePresent)이 파일에 있는 최상위
// 필드(detail::DecodeRecordField)·struct 멤버마다 1단계에서 해석 뒤 한 번이고 빠진 멤버는 생성자
// 값을 유지하며, excludedFlags 멤버는 InvalidArgument다. Required 검사 중
// 표시(BeginRequiredCheck)가 그 트랜잭션의
// Create·SetReference·AssignReferences·AssignElements·SetElementReference·
// GetForWrite(writeStaged)·Commit을 Busy로 막고(직접 호출·hook·writer 할당 콜백 모두, 상태는
// 고정하지 않는다) 표시는 문맥과 함께 내려가며, staged encode가 Registry 상태를 바꾸지 않는다. 표시
// 중의 Rollback과 겹친 검사는 Fatal이다(자식 프로세스 음성 대조). readStaged는 단일 값 필드만 가질
// 수 있다.
using namespace rocks;

namespace required_check_fixture {

// present 판정 시험의 참조 없는 struct다(b의 생성자 값 9가 기준이다).
struct Note {
    uint32 a = 0;
    uint32 b = 9;
    uint32 hint = 0;
};

struct Linked;

// 표시 중 원소 쓰기 시험의 참조 struct다(StructRefArray 원소).
struct Slot {
    ObjectRef<Linked> target;
};

struct Linked final : Object {
    explicit Linked(Allocator& allocator) noexcept : refs(allocator), slots(allocator) {
    }

    void Trace(TraceVisitor& visitor) const noexcept;

    ObjectRef<Linked> link;
    ObjectRefArray<Linked> refs;
    Note note;
    uint32 count = 0;
    StructRefArray<Slot> slots;
};

constexpr TypeId NoteId{0x52434e, 1};
constexpr TypeId LinkedId{0x52434e, 2};
constexpr Guid AId{0x52434d, 1};
constexpr Guid BId{0x52434d, 2};
constexpr Guid HintId{0x52434d, 3};
constexpr Guid LinkId{0x52434d, 4};
constexpr Guid RefsId{0x52434d, 5};
constexpr Guid NoteFieldId{0x52434d, 6};
constexpr Guid CountId{0x52434d, 7};
constexpr Guid SlotsId{0x52434d, 8};
constexpr Guid SlotTargetId{0x52434d, 9};
constexpr TypeId SlotId{0x52434e, 3};

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

} // namespace required_check_fixture

namespace rocks {

template <>
struct GeneratedType<required_check_fixture::Note> {
    static constexpr bool ContainsStrongReference = false;
    static constexpr bool ContainsReferenceArray = false;

    static constexpr TypeId Id() noexcept {
        return required_check_fixture::NoteId;
    }
};

template <>
struct GeneratedType<required_check_fixture::Slot> {
    static constexpr bool ContainsStrongReference = true;
    static constexpr bool ContainsReferenceArray = false;

    static constexpr TypeId Id() noexcept {
        return required_check_fixture::SlotId;
    }

    static void Trace(const required_check_fixture::Slot& value, TraceVisitor& visitor) noexcept {
        visitor.Visit(value.target);
    }
};

} // namespace rocks

void required_check_fixture::Linked::Trace(TraceVisitor& visitor) const noexcept {
    visitor.Visit(link);
    refs.Trace(visitor);
    slots.Trace(visitor);
}

namespace {

using namespace required_check_fixture;

Result<TypeRegistrationToken> RegisterNote(TypeRegistry& types) noexcept {
    static const FieldSchema fields[] = {Scalar(AId, "a", ValueKind::UInt32),
                                         Scalar(BId, "b", ValueKind::UInt32),
                                         Scalar(HintId, "hint", ValueKind::UInt32, {}, EditorOnly)};
    static const RuntimeStructFieldOps ops[] = {detail::StructFieldOps<Note, &Note::a>(AId),
                                                detail::StructFieldOps<Note, &Note::b>(BId),
                                                detail::StructFieldOps<Note, &Note::hint>(HintId)};
    RuntimeStructOps structOps = MakeRuntimeStructOps<Note>();
    structOps.fields = ops;
    if (auto note = types.RegisterStruct({NoteId, 1, fields, {"Note", 4}}, Move(structOps));
        !note) {
        return note;
    }

    static const FieldSchema slotFields[] = {
        Scalar(SlotTargetId, "target", ValueKind::ObjectReference, LinkedId)};
    static const RuntimeStructFieldOps slotOps[] = {
        detail::StructFieldOps<Slot, &Slot::target>(SlotTargetId)};
    RuntimeStructOps slot = MakeRuntimeStructOps<Slot>();
    slot.trace = [](const void* value, TraceVisitor& visitor) noexcept {
        GeneratedType<Slot>::Trace(*static_cast<const Slot*>(value), visitor);
    };
    slot.fields = slotOps;
    return types.RegisterStruct({SlotId, 1, slotFields, {"Slot", 4}}, Move(slot));
}

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

Result<void> ReadLinkStaged(ConstructionContext& transaction, ObjectHandle object,
                            FieldValue& out) noexcept {
    auto owner = transaction.Get<Linked>(object);
    if (!owner) {
        return Result<void>::Failure(owner.Error());
    }

    out.kind = ValueKind::ObjectReference;
    out.reference = owner.Value()->link.Handle();
    return Result<void>::Success();
}

// readStaged를 잘못 달 자리다(struct 필드 note·배열 필드 refs — 등록이 거부해야 한다).
enum class StagedOn : uint8 {
    Nowhere,
    Struct,
    Array
};

FieldSchema ArrayOf(Guid id, const char* name, ValueKind element, TypeId type) noexcept {
    FieldSchema field = Scalar(id, name, ValueKind::Array);
    field.kind =
        element == ValueKind::ObjectReference ? FieldKind::ObjectReference : FieldKind::Value;
    field.element = ValueType{element, type};
    field.maxElements = DefaultMaxElements;
    return field;
}

Result<TypeRegistrationToken> RegisterLinked(TypeRegistry& types,
                                             StagedOn misplaced = StagedOn::Nowhere) noexcept {
    static const FieldSchema fields[] = {
        Scalar(LinkId, "link", ValueKind::ObjectReference, LinkedId, Required),
        ArrayOf(RefsId, "refs", ValueKind::ObjectReference, LinkedId),
        Scalar(NoteFieldId, "note", ValueKind::Struct, NoteId),
        Scalar(CountId, "count", ValueKind::UInt32),
        ArrayOf(SlotsId, "slots", ValueKind::Struct, SlotId)};
    static RuntimeFieldOps ops[5];
    ops[0] = ScalarField<&Linked::link>(LinkId);
    ops[0].readStaged = &ReadLinkStaged;
    ops[1] = CodecField<&Linked::refs>(RefsId);
    ops[1].readStaged = misplaced == StagedOn::Array ? &ReadLinkStaged : nullptr;
    ops[2] = CodecField<&Linked::note>(NoteFieldId, NativeTypeKey<Note>());
    ops[2].readStaged = misplaced == StagedOn::Struct ? &ReadLinkStaged : nullptr;
    ops[3] = ScalarField<&Linked::count>(CountId);
    ops[4] = CodecField<&Linked::slots>(SlotsId, NativeTypeKey<Slot>());
    RuntimeTypeOps typeOps = MakeRuntimeTypeOps<Linked>();
    typeOps.fields = ops;
    typeOps.completeFieldLayout = true;
    return types.Register({LinkedId, 1, fields, {"Linked", 6}}, Move(typeOps));
}

// Note Record다(a = 5, withHint면 hint = 3 — b는 언제나 없다).
Result<Array<uint8>> NoteRecord(Allocator& allocator, bool withHint) noexcept {
    ArchiveWriter five(allocator);
    ArchiveWriter three(allocator);
    if (!five.WriteU32(5) || !three.WriteU32(3)) {
        return Result<Array<uint8>>::Failure(ErrorCode::OutOfMemory);
    }

    RecordBuilder builder(allocator, NoteId, 1);
    if (!builder.AddField(AId, WireType::U32, five.View()) ||
        (withHint && !builder.AddField(HintId, WireType::U32, three.View()))) {
        return Result<Array<uint8>>::Failure(ErrorCode::OutOfMemory);
    }

    return builder.Build();
}

// struct Record를 Record 스택에 넣고 present 판정 지점을 기록하는 복원 문맥이다.
class PresentContext final : public FieldDecodeContext {
public:
    PresentContext(Allocator& allocator, const FieldDecodePolicy& policy) noexcept
        : FieldDecodeContext(policy, nullptr, static_cast<uint32>(WireType::U64)),
          allocator_(allocator) {
    }

    Result<RecordScope> Push(RecordReader&& reader) noexcept {
        return PushRecord(Move(reader));
    }

    // 복원 2단계처럼 진단·수집·present 판정을 버린다.
    void EnterApplyPhase() noexcept {
        SetRecording(false);
    }

    Result<RecordScope> OpenRecord(const FieldView&, TypeId, uint32, uint32) noexcept override {
        return Result<RecordScope>::Failure(ErrorCode::Unsupported);
    }

    Result<RecordScope> OpenMigratedRecord(Span<const uint8>, TypeId, uint32) noexcept override {
        return Result<RecordScope>::Failure(ErrorCode::Unsupported);
    }

    Result<RecordListReader> OpenRecordList(const FieldView&, TypeId) noexcept override {
        return Result<RecordListReader>::Failure(ErrorCode::Unsupported);
    }

    Result<RecordScope> OpenListItem(RecordListReader&, usize) noexcept override {
        return Result<RecordScope>::Failure(ErrorCode::Unsupported);
    }

    Result<Span<const uint8>> MigrateStruct(TypeId, uint32, uint32,
                                            Span<const uint8>) noexcept override {
        return Result<Span<const uint8>>::Failure(ErrorCode::Unsupported);
    }

    Result<void> AddFields(usize) noexcept override {
        return Result<void>::Success();
    }

    Result<void> AddElements(usize) noexcept override {
        return Result<void>::Success();
    }

    Result<DecodedReference> ReadReference(ArchiveReader&,
                                           const ReferenceExpectation&) noexcept override {
        return Result<DecodedReference>::Failure(ErrorCode::Unsupported);
    }

    Result<void> Spend(uint64) noexcept override {
        return Result<void>::Success();
    }

    Result<void> ReserveApply(uint64) noexcept override {
        return Result<void>::Success();
    }

    Allocator& Scratch() noexcept override {
        return allocator_;
    }

    Allocator& ObservationScratch() noexcept override {
        return allocator_;
    }

    Guid noted[4] = {};
    usize notes = 0;

protected:
    // 최상위 필드 payload를 그대로 연다(DecodeRecordField 시험).
    Result<ArchiveReader> OpenTopLevelValue(const FieldView& field) noexcept override {
        return ArchiveReader::Create(field.payload);
    }

    Result<void> NotePresent(const FieldCodecFrame& frame) noexcept override {
        if (notes < 4) {
            noted[notes] = frame.field->id;
        }

        ++notes;
        return Result<void>::Success();
    }

private:
    Allocator& allocator_;
};

// Note Record 하나를 value 자리에 해석한다(1단계 관찰 방식 — construction 없이 value에 쓴다).
Result<void> DecodeNote(Allocator& allocator, StructView view, Span<const uint8> record,
                        PresentContext& context, Note& value) noexcept {
    auto reader = RecordReader::Open(allocator, record, NoteId, 1, 1);
    if (!reader) {
        return Result<void>::Failure(reader.Error());
    }

    auto scope = context.Push(Move(reader.Value()));
    if (!scope) {
        return Result<void>::Failure(scope.Error());
    }

    return detail::DecodeStructRecord(view, nullptr, ObjectHandle{}, &value, context);
}

// staged 객체를 읽는 Required 검사 문맥이다(BeginRequiredCheck). 참조는 자리값(U64)만 쓴다.
class CheckContext final : public FieldEncodeContext {
public:
    CheckContext(Allocator& allocator, ConstructionContext& transaction, FieldCodecHooks* hooks,
                 const FieldEncodePolicy& policy) noexcept
        : FieldEncodeContext(policy, hooks, static_cast<uint32>(WireType::U64)),
          allocator_(allocator) {
        BeginRequiredCheck(transaction);
    }

    Result<void> Spend(uint64) noexcept override {
        return Result<void>::Success();
    }

    Result<void> ReserveApply(uint64) noexcept override {
        return Result<void>::Success();
    }

    Allocator& Scratch() noexcept override {
        return allocator_;
    }

    Allocator& ObservationScratch() noexcept override {
        return allocator_;
    }

    Result<void> AddFields(usize) noexcept override {
        return Result<void>::Success();
    }

    Result<void> AddElements(usize) noexcept override {
        return Result<void>::Success();
    }

    RecordLimits NestedRecordLimits() const noexcept override {
        return {};
    }

protected:
    Result<void> WriteReferenceWire(ArchiveWriter& out, ObjectHandle target,
                                    const ReferenceExpectation&) noexcept override {
        return out.WriteU64(target == ObjectHandle{} ? 0 : 1);
    }

private:
    Allocator& allocator_;
};

// RequiredMissing을 받으면 그 트랜잭션에 Create·SetReference·Commit을 시도해 오류를 기록한다.
class WritingHooks final : public FieldCodecHooks {
public:
    Result<void> Report(const FieldDiagnostic& diagnostic) noexcept override {
        ++reports;
        kind = diagnostic.kind;
        createError = transaction->Create<Linked>(LinkedId).Error();
        auto object = transaction->Get<Linked>(owner);
        readable = object.IsOk();
        if (object) {
            setError = transaction->SetReference(owner, object.Value()->link, owner).Error();
        }

        commitError = transaction->Commit().Error();
        return Result<void>::Success();
    }

    ConstructionContext* transaction = nullptr;
    ObjectHandle owner{};
    usize reports = 0;
    FieldDiagnosticKind kind = FieldDiagnosticKind::UnknownField;
    ErrorCode createError = ErrorCode::None;
    ErrorCode setError = ErrorCode::None;
    ErrorCode commitError = ErrorCode::None;
    bool readable = false;
};

// 할당할 때마다 그 트랜잭션에 SetReference를 시도해 오류를 기록하는 할당기다(검사 writer용).
class WritingAllocator final : public Allocator {
public:
    WritingAllocator(Allocator& inner, ConstructionContext& transaction,
                     ObjectHandle owner) noexcept
        : inner_(inner), transaction_(transaction), owner_(owner) {
    }

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override {
        ++calls;
        auto object = transaction_.Get<Linked>(owner_);
        if (object) {
            setError = transaction_.SetReference(owner_, object.Value()->link, owner_).Error();
        }

        return inner_.Allocate(bytes, alignment, tag);
    }

    Result<void> Deallocate(void* value) noexcept override {
        return inner_.Deallocate(value);
    }

    usize calls = 0;
    ErrorCode setError = ErrorCode::None;

private:
    Allocator& inner_;
    ConstructionContext& transaction_;
    ObjectHandle owner_;
};

} // namespace

// 최상위 필드의 present 판정 지점(detail::DecodeRecordField — 그래프 1단계 DecodeNode가 필드마다
// 부른다)은 해석이 성공한 뒤 한 번이다: 잘못된 wire로 해석이 실패하면 알리지 않고, 기록하지 않는
// 2단계는 알리지 않는다. 모르는 필드는 호출자가 이 함수에 넘기지 않는다(DecodeNode의
// SkipUnknownField).
TEST(RequiredCheck_TopLevelPresentIsNotedOnceAfterDecode) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterNote(types));
    REQUIRE(RegisterLinked(types));
    {
        auto lease = types.Acquire(LinkedId);
        REQUIRE(lease);
        const usize countIndex = lease.Value().FieldIndex(CountId);
        REQUIRE(countIndex != TypeLease::NoField);
        const FieldSchema& field = lease.Value().Schema().fields[countIndex];
        const RuntimeFieldOps* ops = lease.Value().FieldOps(countIndex);
        REQUIRE(ops);
        ArchiveWriter payload(allocator);
        REQUIRE(payload.WriteU32(41));
        const FieldView stored{CountId, static_cast<uint32>(WireType::U32), payload.View()};
        const FieldView wrongWire{CountId, static_cast<uint32>(WireType::U64), payload.View()};
        PresentContext context(allocator, {});
        {
            FieldScope scope(context, field, LinkedId);
            CHECK(detail::DecodeRecordField(*ops, nullptr, ObjectHandle{}, wrongWire, context)
                      .Error() == ErrorCode::TypeMismatch);
            CHECK(context.notes == 0);
            REQUIRE(detail::DecodeRecordField(*ops, nullptr, ObjectHandle{}, stored, context));
            CHECK(context.notes == 1 && context.noted[0] == CountId);
        }

        context.EnterApplyPhase();
        {
            FieldScope scope(context, field, LinkedId);
            REQUIRE(detail::DecodeRecordField(*ops, nullptr, ObjectHandle{}, stored, context));
            CHECK(context.notes == 1);
        }
    }

    CHECK(types.Shutdown());
}

// 파일에 있는 struct 멤버마다 1단계에서 present 판정이 한 번 오고(빠진 b는 오지 않고 생성자 값 9를
// 유지한다), 기록하지 않는 2단계는 알리지 않는다. excludedFlags(EditorOnly) 멤버가 있으면
// InvalidArgument이고 그 멤버는 present로 알리지 않는다.
TEST(RequiredCheck_PresentIsNotedOncePerStoredMember) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterNote(types));
    {
        auto lease = types.AcquireValueType(NoteId, TypeKind::Struct);
        REQUIRE(lease);
        const StructView view = lease.Value().Struct();
        auto withHint = NoteRecord(allocator, true);
        REQUIRE(withHint);
        {
            PresentContext context(allocator, {});
            Note value;
            REQUIRE(DecodeNote(allocator, view, withHint.Value().View(), context, value));
            CHECK(context.notes == 2);
            const bool distinct = context.noted[0] != context.noted[1] &&
                                  (context.noted[0] == AId || context.noted[0] == HintId) &&
                                  (context.noted[1] == AId || context.noted[1] == HintId);
            CHECK(distinct);
            CHECK(value.a == 5 && value.hint == 3 && value.b == 9);

            context.EnterApplyPhase();
            Note applied;
            REQUIRE(DecodeNote(allocator, view, withHint.Value().View(), context, applied));
            CHECK(context.notes == 2);
            CHECK(applied.a == 5 && applied.b == 9);
        }

        FieldDecodePolicy cooked;
        cooked.excludedFlags = EditorOnly;
        {
            PresentContext context(allocator, cooked);
            Note value;
            CHECK(DecodeNote(allocator, view, withHint.Value().View(), context, value).Error() ==
                  ErrorCode::InvalidArgument);
            for (usize index = 0; index < context.notes && index < 4; ++index) {
                CHECK(context.noted[index] != HintId);
            }
        }

        auto withoutHint = NoteRecord(allocator, false);
        REQUIRE(withoutHint);
        {
            PresentContext context(allocator, cooked);
            Note value;
            REQUIRE(DecodeNote(allocator, view, withoutHint.Value().View(), context, value));
            CHECK(context.notes == 1 && context.noted[0] == AId);
            CHECK(value.a == 5 && value.b == 9 && value.hint == 0);
        }
    }

    CHECK(types.Shutdown());
}

// 검사 중 표시 동안 그 트랜잭션의 Create·SetReference·AssignReferences·AssignElements·
// SetElementReference(두 형태)·GetForWrite·등록된 writeStaged·Commit은 Busy다(직접 호출, 검사
// encode가 부른 hook, 검사 writer의 할당 콜백 모두). Get은 되고, 검사 모드는
// rejectMissingRequired와 무관하게 Required null을 진단만 한다. staged encode는 Borrow·encode
// 고정·생성 없이 Registry 상태를 그대로 둔다. 표시는 문맥과 함께 내려가고 Busy가 트랜잭션을 오류로
// 고정하지 않았으므로 Create·Commit이 다시 된다.
TEST(RequiredCheck_MarkMakesStagedWritesBusyUntilTheCheckEnds) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterNote(types));
    REQUIRE(RegisterLinked(types));
    {
        ObjectRegistry objects(allocator, types, {0x5243, 1});
        {
            ConstructionContext transaction(objects, objects.OwnerAccess());
            auto first = transaction.Create<Linked>(LinkedId);
            REQUIRE(first);
            {
                auto staged = transaction.Get<Linked>(first.Value());
                REQUIRE(staged);
                REQUIRE(transaction.AssignElements(first.Value(), staged.Value()->slots, 1));
            }

            auto lease = types.Acquire(LinkedId);
            REQUIRE(lease);
            const usize linkIndex = lease.Value().FieldIndex(LinkId);
            REQUIRE(linkIndex != TypeLease::NoField);
            const ObjectStats before = objects.Stats();
            WritingHooks hooks;
            hooks.transaction = &transaction;
            hooks.owner = first.Value();
            FieldEncodePolicy strict;
            strict.rejectMissingRequired = true;
            {
                CheckContext check(allocator, transaction, &hooks, strict);
                CHECK(check.IsRequiredCheck() && check.StagedSource() == &transaction);
                CHECK(transaction.Create<Linked>(LinkedId).Error() == ErrorCode::Busy);
                auto owner = transaction.Get<Linked>(first.Value());
                REQUIRE(owner);
                CHECK(transaction.SetReference(first.Value(), owner.Value()->link, first.Value())
                          .Error() == ErrorCode::Busy);
                const ObjectHandle targets[] = {first.Value()};
                CHECK(transaction.AssignReferences(first.Value(), owner.Value()->refs, targets)
                          .Error() == ErrorCode::Busy);
                CHECK(transaction.AssignElements(first.Value(), owner.Value()->slots, 2).Error() ==
                      ErrorCode::Busy);
                CHECK(transaction
                          .SetElementReference(first.Value(), owner.Value()->slots, 0,
                                               owner.Value()->slots[0].target, first.Value())
                          .Error() == ErrorCode::Busy);
                const detail::StructElementLocation location =
                    detail::StructRefArrayAccess::Location(owner.Value()->slots, 0);
                CHECK(transaction
                          .SetElementReference(first.Value(), location,
                                               owner.Value()->slots[0].target, first.Value())
                          .Error() == ErrorCode::Busy);
                CHECK(transaction.GetForWrite<Linked>(first.Value()).Error() == ErrorCode::Busy);
                const RuntimeFieldOps* count =
                    lease.Value().FieldOps(lease.Value().FieldIndex(CountId));
                REQUIRE(count);
                FieldValue seven(allocator);
                seven.kind = ValueKind::UInt32;
                seven.unsignedInteger = 7;
                CHECK(count->writeStaged(transaction, first.Value(), seven).Error() ==
                      ErrorCode::Busy);
                CHECK(owner.Value()->count == 0);
                CHECK(transaction.Commit().Error() == ErrorCode::Busy);

                WritingAllocator writing(allocator, transaction, first.Value());
                ArchiveWriter writer(writing);
                const RuntimeFieldOps* ops = lease.Value().FieldOps(linkIndex);
                REQUIRE(ops);
                {
                    FieldScope scope(check, lease.Value().Schema().fields[linkIndex], LinkedId);
                    auto wire = ops->encode(*ops, objects, objects.OwnerAccess(), first.Value(),
                                            writer, check);
                    REQUIRE(wire);
                    CHECK(wire.Value() == static_cast<uint32>(WireType::U64));
                }

                CHECK(hooks.reports == 1);
                CHECK(hooks.kind == FieldDiagnosticKind::RequiredMissing);
                CHECK(hooks.readable);
                CHECK(hooks.createError == ErrorCode::Busy);
                CHECK(hooks.setError == ErrorCode::Busy);
                CHECK(hooks.commitError == ErrorCode::Busy);
                CHECK(writing.calls >= 1 && writing.setError == ErrorCode::Busy);
                const ObjectStats during = objects.Stats();
                CHECK(during.borrows == 0 && during.encodePins == 0);
                CHECK(during.reservedObjects == before.reservedObjects);
                CHECK(during.liveObjects == before.liveObjects);
                CHECK(during.roots == before.roots);
            }

            auto owner = transaction.GetForWrite<Linked>(first.Value());
            REQUIRE(owner);
            CHECK(owner.Value()->link.Handle() == ObjectHandle{});
            CHECK(owner.Value()->refs.IsEmpty() && owner.Value()->slots.Size() == 1);
            CHECK(owner.Value()->slots[0].target.Handle() == ObjectHandle{});
            auto second = transaction.Create<Linked>(LinkedId);
            REQUIRE(second);
            REQUIRE(transaction.SetReference(first.Value(), owner.Value()->link, second.Value()));
            auto committed = transaction.Commit();
            REQUIRE(committed);
            CHECK(objects.Stats().liveObjects == 2);
        }

        REQUIRE(objects.Collect(objects.OwnerAccess()));
        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}

// readStaged는 단일 값 필드의 선택 칸이다 — struct·배열 필드에 주면 등록이 InvalidArgument다.
TEST(RequiredCheck_ReadStagedIsOnlyForSingleValueFields) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterNote(types));
    CHECK(RegisterLinked(types, StagedOn::Struct).Error() == ErrorCode::InvalidArgument);
    CHECK(RegisterLinked(types, StagedOn::Array).Error() == ErrorCode::InvalidArgument);
    auto registered = RegisterLinked(types);
    REQUIRE(registered);
    {
        auto lease = types.Acquire(LinkedId);
        REQUIRE(lease);
        const RuntimeFieldOps* link = lease.Value().FieldOps(lease.Value().FieldIndex(LinkId));
        REQUIRE(link);
        CHECK(link->readStaged == &ReadLinkStaged);
    }

    CHECK(types.Shutdown());
}

namespace rocks::test {

void RegisterRequiredCheckNegativeControl(int (*function)(uint32) noexcept) noexcept;

// 전용 child process에서 실행한다. Fatal이 없으면 0으로 돌아가 도구 시험이 실패한다.
// 0: 검사 중 표시 동안 Rollback, 1: 같은 트랜잭션에 검사를 겹쳐 세움.
int RunRequiredCheckNegativeControl(uint32 operation) noexcept {
    PrepareFatalTestProcess();
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    if (!RegisterNote(types) || !RegisterLinked(types)) {
        return 0;
    }

    ObjectRegistry objects(allocator, types, {0x5243, 2});
    ConstructionContext transaction(objects, objects.OwnerAccess());
    if (!transaction.Create<Linked>(LinkedId)) {
        return 0;
    }

    CheckContext check(allocator, transaction, nullptr, {});
    if (operation == 0) {
        transaction.Rollback();
        return 0;
    }

    CheckContext overlapping(allocator, transaction, nullptr, {});
    return 0;
}

namespace {

struct RequiredCheckNegativeRegistration {
    RequiredCheckNegativeRegistration() noexcept {
        RegisterRequiredCheckNegativeControl(&RunRequiredCheckNegativeControl);
    }
};

RequiredCheckNegativeRegistration requiredCheckNegativeRegistration;

} // namespace

} // namespace rocks::test
