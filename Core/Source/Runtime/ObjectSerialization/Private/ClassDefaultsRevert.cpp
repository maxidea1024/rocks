#include "GraphInternal.h"

#include <Core/Text/NamePool.h>
#include <Object/ClassRef.h>
#include <Object/FieldValueParse.h>
#include <Object/NameField.h>
#include <Object/PropertyPath.h>
#include <Object/TypeRegistry.h>
#include <ObjectSerialization/ClassDefaults.h>
#include <Serialization/OptionalWire.h>
#include <Serialization/Record.h>

// 클래스 기본값으로 되돌리기다(Docs/Design/RuntimeClassMetadata.ko.md 'A5b2 decode 명세'). 레코드의
// 그 경로 payload를 경로 쓰기 연산으로 재생한다: 사전 단계(PlanRevert)가 연산 목록을 만들고 검사한
// 뒤(ReadOnly·경로·범위 — 어떤 쓰기보다 먼저) 적용 단계(ApplyRevert)가 기존 경로 쓰기 API로 쓴다.
// 값은 binary로 해석한다(텍스트 왕복 없음 — NaN payload·-0.0이 비트 그대로).
namespace rocks {
namespace {

using namespace graph_detail;

// 재생 전용 decode 문맥이다(복원 2단계처럼 기록하지 않음, hook 없음). 1단계 기록을 꺼내는 대신
// ClassRef는 대상 TypeRegistry로, Name은 대상 Registry 풀로 바로 판정한다(교차 검증 필수 1 — 한
// 번에 해석). 단일 값만 이 문맥으로 해석한다(struct·Optional<struct>는 재생이 Record를 직접 연다).
class RevertDecodeContext final : public FieldDecodeContext {
public:
    RevertDecodeContext(Allocator& allocator, TypeRegistry& types, NamePool* names,
                        TypeId type) noexcept
        : FieldDecodeContext(FieldDecodePolicy{}, nullptr, ReferenceWire), allocator_(allocator),
          types_(types), names_(names) {
        SetObject(type, 0);
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

    // 기준 참조는 늘 null이다(A5a — 레코드의 참조는 0만).
    Result<DecodedReference> ReadReference(ArchiveReader& reader,
                                           const ReferenceExpectation&) noexcept override {
        return ReadNull(reader);
    }

    Result<DecodedReference> ReadWeakReference(ArchiveReader& reader,
                                               const ReferenceExpectation&) noexcept override {
        return ReadNull(reader);
    }

    // Name은 대상 풀에 바로 intern한다(None은 기본 Name, 풀이 없으면 그 밖은 Unsupported).
    Result<Name> TakeName(StringView text) noexcept override {
        if (IsNoneNameText(text)) {
            return Result<Name>::Success(Name{});
        }

        if (names_ == nullptr) {
            return Result<Name>::Failure(ErrorCode::Unsupported);
        }

        return names_->TryIntern(text);
    }

    // ClassRef는 지금 등록에서 다시 판정한다(레코드를 만든 뒤 해제·재등록이 있을 수 있다): 등록된
    // 클래스이고 스키마 Base의 파생이어야 한다(경로 쓰기도 같은 판정을 한다).
    Result<TypeId> TakeClassReference(TypeId stored) noexcept override {
        const FieldCodecFrame* frame = Current();
        if (frame == nullptr || frame->field == nullptr) {
            return Result<TypeId>::Failure(ErrorCode::InvalidArgument);
        }

        const FieldSchema& field = *frame->field;
        const TypeId base =
            field.value.kind == ValueKind::Optional ? field.element.type : field.value.type;
        auto checked = detail::CheckClassReferenceLease(types_.Acquire(stored), base, nullptr);
        if (!checked) {
            return Result<TypeId>::Failure(checked.Error());
        }

        return Result<TypeId>::Success(stored);
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

protected:
    // 단일 값 payload는 레코드 byte 안의 독립 범위다(그래프 기본 한도).
    Result<ArchiveReader> OpenTopLevelValue(const FieldView& field) noexcept override {
        const ObjectGraphLimits graph{};
        ArchiveLimits limits;
        limits.maxTotalBytes = graph.maxBytes;
        limits.maxValueBytes = graph.maxValueBytes;
        limits.maxWork = graph.maxWork;
        limits.maxOperations = graph.maxWork;
        return ArchiveReader::Create(field.payload, limits);
    }

private:
    static Result<DecodedReference> ReadNull(ArchiveReader& reader) noexcept {
        auto local = reader.ReadU32();
        if (!local) {
            return Result<DecodedReference>::Failure(local.Error());
        }

        if (local.Value() != 0) {
            return Result<DecodedReference>::Failure(ErrorCode::InvalidArgument);
        }

        DecodedReference decoded;
        decoded.isNull = true;
        return Result<DecodedReference>::Success(decoded);
    }

    Allocator& allocator_;
    TypeRegistry& types_;
    NamePool* names_;
};

// 레코드를 다시 읽는 한도다(그래프 기본 한도 — ClassDefaults.cpp의 읽기 한도와 같다).
RecordLimits RevertReadLimits() noexcept {
    const ObjectGraphLimits graph{};
    RecordLimits limits;
    limits.archive.maxTotalBytes = graph.maxBytes;
    limits.archive.maxValueBytes = graph.maxValueBytes;
    limits.archive.maxWork = graph.maxWork;
    limits.archive.maxOperations = graph.maxWork;
    limits.maxFields = graph.maxTotalFields;
    return limits;
}

// 재생 위치 하나의 스키마 정보다.
struct RevertPosition {
    const FieldSchema* field = nullptr;
    TypeId declaringType{};
    const EnumSchema* enumeration = nullptr;
    StructView structure{};
    const EnumSchema* keyEnumeration = nullptr;
    uint32 flags = 0;
};

// 재생 중인 경로다(필드 단계만 — 원소·key 단계는 1차 범위에서 만들지 않는다).
struct RevertSteps {
    PathStep steps[MaxPropertyPathSteps]{};
    usize count = 0;
};

// 연산 하나를 계획에 더한다(경로 사본, 값은 옮긴다).
Result<void> AppendRevertOperation(Allocator& allocator, RevertPlan& plan,
                                   RevertOperation::Kind kind, const RevertSteps& path,
                                   FieldValue* value) noexcept {
    RevertOperation operation(allocator);
    operation.kind = kind;
    for (usize index = 0; index < path.count; ++index) {
        operation.steps[index] = path.steps[index];
    }

    operation.stepCount = path.count;
    if (value != nullptr) {
        operation.value = Move(*value);
    }

    if (!plan.operations.TryPushBack(Move(operation))) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    return Result<void>::Success();
}

// 경로에 필드 단계 하나를 더한 사본이다(최대 단계 수를 넘으면 InvalidArgument — 등록이 깊이를
// 묶으므로 정상 입력에서는 일어나지 않는다).
Result<RevertSteps> ChildRevertSteps(const RevertSteps& path, Guid field) noexcept {
    if (path.count >= MaxPropertyPathSteps) {
        return Result<RevertSteps>::Failure(ErrorCode::InvalidArgument);
    }

    RevertSteps child = path;
    child.steps[child.count] = PathStep{field};
    ++child.count;
    return Result<RevertSteps>::Success(child);
}

Result<void> EmitRevertValue(Allocator& allocator, RevertDecodeContext& context, RevertPlan& plan,
                             const RevertSteps& path, const RevertPosition& at, uint32 wire,
                             Span<const uint8> payload) noexcept;

// struct Record(전체 envelope byte)의 스키마 멤버마다 재생한다. Editor 레코드는 Transient 말고 모든
// 멤버를 담는다.
Result<void> EmitRevertMembers(Allocator& allocator, RevertDecodeContext& context, RevertPlan& plan,
                               const RevertSteps& path, StructView view, uint32 flags,
                               Span<const uint8> record) noexcept {
    const TypeSchema& schema = view.Schema();
    auto reader = RecordReader::Open(allocator, record, schema.id, schema.version, schema.version,
                                     RevertReadLimits());
    if (!reader) {
        return Result<void>::Failure(reader.Error());
    }

    for (usize index = 0; index < schema.fields.Size(); ++index) {
        const FieldSchema& member = schema.fields[index];
        if (HasFlag(member.flags, FieldFlags::Transient)) {
            continue;
        }

        auto found = reader.Value().Find(member.id);
        if (!found) {
            // Editor 레코드는 Transient 말고 모든 멤버를 담는다 — 없으면 레코드와 등록이 어긋났다.
            return Result<void>::Failure(found.Error());
        }

        auto child = ChildRevertSteps(path, member.id);
        if (!child) {
            return Result<void>::Failure(child.Error());
        }

        RevertPosition position;
        position.field = &member;
        position.declaringType = schema.id;
        position.enumeration = view.FieldEnum(index);
        position.structure = view.FieldStruct(index);
        position.keyEnumeration = view.FieldKeyEnum(index);
        position.flags = PropagateFieldFlags(flags, member.flags);
        if (auto emitted = EmitRevertValue(allocator, context, plan, child.Value(), position,
                                           found.Value().wireType, found.Value().payload);
            !emitted) {
            return emitted;
        }
    }

    return Result<void>::Success();
}

Result<void> EmitRevertValue(Allocator& allocator, RevertDecodeContext& context, RevertPlan& plan,
                             const RevertSteps& path, const RevertPosition& at, uint32 wire,
                             Span<const uint8> payload) noexcept {
    const FieldSchema& field = *at.field;
    // ReadOnly 위치가 하나라도 있으면 되돌리기 전체를 거부한다(사전 단계 — 아무것도 쓰지 않는다).
    if (HasFlag(at.flags, FieldFlags::ReadOnly)) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    switch (field.value.kind) {
    case ValueKind::Struct:
        if (at.structure.IsEmpty()) {
            return Result<void>::Failure(ErrorCode::Unsupported);
        }

        if (wire != static_cast<uint32>(WireType::Record)) {
            return Result<void>::Failure(ErrorCode::TypeMismatch);
        }

        // Atomic struct도 멤버마다 쓴다(모든 멤버를 쓰므로 결과는 통째 쓰기와 같다).
        return EmitRevertMembers(allocator, context, plan, path, at.structure, at.flags, payload);
    case ValueKind::InstancedStruct:
        // 기준은 늘 빈 값이다(A5a — 생성자는 struct lease를 얻을 수 없다).
        return AppendRevertOperation(allocator, plan, RevertOperation::Kind::ClearInstanced, path,
                                     nullptr);
    case ValueKind::Array:
    case ValueKind::StaticArray:
    case ValueKind::Set:
    case ValueKind::Map:
        // 1차 범위 밖이다(컨테이너 재생은 후속 — RuntimeClassMetadata 'A5b2 1차 구현 범위').
        return Result<void>::Failure(ErrorCode::Unsupported);
    case ValueKind::Optional:
        if (FieldIsAggregate(field)) {
            if (at.structure.IsEmpty()) {
                return Result<void>::Failure(ErrorCode::Unsupported);
            }

            auto archive = ArchiveReader::Create(payload);
            if (!archive) {
                return Result<void>::Failure(archive.Error());
            }

            auto optional = OptionalWireReader::Open(Move(archive.Value()), WireType::Record);
            if (!optional) {
                return Result<void>::Failure(optional.Error());
            }

            if (!optional.Value().IsSet()) {
                return AppendRevertOperation(allocator, plan, RevertOperation::Kind::ResetOptional,
                                             path, nullptr);
            }

            // set이면 기본 값을 만든 뒤(이미 set이어도 기본 값으로 바꾼다) 멤버를 쓴다.
            if (auto emplaced = AppendRevertOperation(
                    allocator, plan, RevertOperation::Kind::EmplaceOptional, path, nullptr);
                !emplaced) {
                return emplaced;
            }

            return EmitRevertMembers(allocator, context, plan, path, at.structure, at.flags,
                                     optional.Value().RecordBytes());
        }

        break;
    default:
        break;
    }

    // 단일 값(bool·정수·실수·enum·String·Text·Name·Guid·AssetId·Digest·Soft·ClassRef·참조)·
    // bitfield·Optional 단일 값: 그 위치의 FieldScope로 FieldValue를 만든다.
    FieldScope scope(context, field, at.declaringType, at.enumeration, at.structure,
                     FieldCodecFrame::NoIndex, at.keyEnumeration);
    FieldValue value(allocator);
    if (auto parsed =
            detail::ParseFieldValue(FieldView{field.id, wire, payload}, context, nullptr, &value);
        !parsed) {
        return parsed;
    }

    return AppendRevertOperation(allocator, plan, RevertOperation::Kind::Write, path, &value);
}

} // namespace

Result<void> PlanRevert(ObjectRegistry& registry, const ObjectAccessContext& access,
                        ObjectHandle object, const PropertyPathBuffer& path, Allocator& scratch,
                        RevertPlan& out, ClassDefaultsDiagnostic* diagnostic) noexcept {
    out.operations.Clear();
    out.object = object;
    if (path.count == 0 || path.count > MaxPropertyPathSteps) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 원소·key 단계는 기준 위치가 없다(A5a 경로 조회와 같다).
    for (usize index = 0; index < path.count; ++index) {
        if (path.steps[index].selector != PathSelector::None) {
            return Result<void>::Failure(ErrorCode::Unsupported);
        }
    }

    auto lease = registry.AcquireType(access, object);
    if (!lease) {
        return Result<void>::Failure(lease.Error());
    }

    auto defaults = AcquireClassDefaults(lease.Value(), scratch, diagnostic);
    if (!defaults) {
        return Result<void>::Failure(defaults.Error());
    }

    auto payload = FindDefaultPayload(lease.Value(), *defaults.Value(), path,
                                      ClassDefaultsConfiguration::Editor, scratch);
    if (!payload) {
        return Result<void>::Failure(payload.Error());
    }

    // 경로 끝 위치의 스키마와 유효 플래그(바깥 플래그 누적 — PropagateFieldFlags)를 찾는다.
    // FindDefaultPayload가 경로 모양(필드 → 비Atomic struct 멤버)을 이미 검사했다.
    RevertPosition position;
    RevertSteps steps;
    StructView view{};
    uint32 flags = 0;
    for (usize step = 0; step < path.count; ++step) {
        const bool top = step == 0;
        const Guid id = path.steps[step].fieldId;
        const usize index = top ? lease.Value().FieldIndex(id) : view.FieldIndex(id);
        if (index == TypeLease::NoField) {
            return Result<void>::Failure(ErrorCode::NotFound);
        }

        const TypeSchema& owner = top ? lease.Value().Schema() : view.Schema();
        const FieldSchema& field = owner.fields[index];
        flags = PropagateFieldFlags(flags, field.flags);
        position.field = &field;
        position.declaringType = owner.id;
        position.enumeration = top ? lease.Value().FieldEnum(index) : view.FieldEnum(index);
        position.structure = top ? lease.Value().FieldStruct(index) : view.FieldStruct(index);
        position.keyEnumeration =
            top ? lease.Value().FieldKeyEnum(index) : view.FieldKeyEnum(index);
        position.flags = flags;
        view = position.structure;
        steps.steps[step] = PathStep{id};
    }

    steps.count = path.count;
    RevertDecodeContext context(scratch, registry.Types(), registry.Names(), lease.Value().Id());
    auto emitted = EmitRevertValue(scratch, context, out, steps, position, payload.Value().wire,
                                   payload.Value().bytes);
    if (!emitted) {
        out.operations.Clear();
    }

    return emitted;
}

Result<void> ApplyRevert(ObjectRegistry& registry, const ObjectAccessContext& access,
                         const RevertPlan& plan, PropertyPathFailure* failure) noexcept {
    for (const RevertOperation& operation : plan.operations) {
        const PropertyPath path = operation.Path();
        Result<void> applied = Result<void>::Success();
        switch (operation.kind) {
        case RevertOperation::Kind::Write:
            applied =
                PathWrite(registry, access, plan.object, path, operation.value, nullptr, failure);
            break;
        case RevertOperation::Kind::ResetOptional:
            applied = PathResetOptional(registry, access, plan.object, path, failure);
            break;
        case RevertOperation::Kind::EmplaceOptional:
            applied = PathEmplaceOptional(registry, access, plan.object, path, failure);
            break;
        case RevertOperation::Kind::ClearInstanced:
            applied = PathClearInstanced(registry, access, plan.object, path, failure);
            break;
        }

        if (!applied) {
            return applied;
        }
    }

    return Result<void>::Success();
}

Result<void> RevertToDefault(ObjectRegistry& registry, const ObjectAccessContext& access,
                             ObjectHandle object, const PropertyPathBuffer& path,
                             Allocator& scratch, PropertyPathFailure* failure,
                             ClassDefaultsDiagnostic* diagnostic) noexcept {
    RevertPlan plan(scratch);
    if (auto planned = PlanRevert(registry, access, object, path, scratch, plan, diagnostic);
        !planned) {
        return planned;
    }

    return ApplyRevert(registry, access, plan, failure);
}

} // namespace rocks
