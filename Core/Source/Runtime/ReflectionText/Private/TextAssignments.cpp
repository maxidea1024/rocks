#include "TextAssignInternal.h"

#include <Object/ConstructionContext.h>
#include <Object/ObjectBatchPublish.h>

// 대입 묶음의 수명·단위·override 경로·게시다(리플렉션 B9i3c1 — ReflectionText/TextAssignments.h).
// 대입 하나를 사본에 적용하는 일은 TextAssignApply.cpp다.
namespace rocks::reflection_text {

namespace detail {

Result<void> PathCopies::Append(PropertyPath path) noexcept {
    usize bytes = 0;
    for (usize index = 0; index < path.keys.Size(); ++index) {
        bytes += path.keys[index].wire.Size();
    }

    if (auto reserved = steps_.TryReserve(steps_.Size() + path.steps.Size()); !reserved) {
        return reserved;
    }

    if (auto reserved = keys_.TryReserve(keys_.Size() + path.keys.Size()); !reserved) {
        return reserved;
    }

    if (auto reserved = bytes_.TryReserve(bytes_.Size() + bytes); !reserved) {
        return reserved;
    }

    if (auto reserved = entries_.TryReserve(entries_.Size() + 1); !reserved) {
        return reserved;
    }

    Entry entry;
    entry.stepOffset = steps_.Size();
    entry.stepCount = path.steps.Size();
    entry.keyOffset = keys_.Size();
    entry.keyCount = path.keys.Size();
    for (usize index = 0; index < path.steps.Size(); ++index) {
        (void)steps_.TryPushBack(path.steps[index]);
    }

    for (usize index = 0; index < path.keys.Size(); ++index) {
        const PathKey& key = path.keys[index];
        KeyRecord record;
        record.kind = key.kind;
        record.offset = bytes_.Size();
        record.size = key.wire.Size();
        (void)keys_.TryPushBack(record);
        for (usize at = 0; at < key.wire.Size(); ++at) {
            (void)bytes_.TryPushBack(key.wire[at]);
        }
    }

    (void)entries_.TryPushBack(entry);
    return Result<void>::Success();
}

PropertyPath PathCopies::View(usize index, PathKey* keys) const noexcept {
    const Entry& entry = entries_[index];
    for (usize at = 0; at < entry.keyCount; ++at) {
        const KeyRecord& record = keys_[entry.keyOffset + at];
        keys[at].kind = record.kind;
        keys[at].wire = Span<const uint8>(bytes_.View().Data() + record.offset, record.size);
    }

    return PropertyPath{
        Span<const PathStep>(steps_.View().Data() + entry.stepOffset, entry.stepCount),
        Span<const PathKey>(keys, entry.keyCount)};
}

AssignmentState::AssignmentState(const AssignmentTarget& target_, const TextCodecOptions& options_,
                                 TextDiagnostics& diagnostics, Allocator& allocator_) noexcept
    : target(target_), options(options_),
      context(target_.Registry().Types(), options, diagnostics, allocator_), allocator(allocator_),
      units(allocator_), references(allocator_), containers(allocator_), elements(allocator_),
      assigned(allocator_), overrideSteps(allocator_), overrideEnds(allocator_),
      overrideViews(allocator_), pins(allocator_) {
}

PathTarget AssignmentState::Root() const noexcept {
    switch (target.GetKind()) {
    case AssignmentTarget::Kind::Staged:
        return PathTarget::Staged(*target.Transaction(), target.Handle());
    case AssignmentTarget::Kind::Value:
        return PathTarget::Value(*target.StructType(), target.StructType()->Struct(),
                                 target.Storage(), target.ValueAllocator());
    case AssignmentTarget::Kind::Live:
        break;
    }

    return PathTarget(target.Handle());
}

Result<void> Diagnose(AssignmentState& state, TextDiagnosticCode code, TextSource source,
                      StringView pathText, StringView message) noexcept {
    state.failed = true;
    return state.context.Diagnostics().Add(code, source, pathText, message);
}

Result<void> ReportPathFailure(AssignmentState& state, ErrorCode code,
                               const PropertyPathFailure& failure, TextSource source,
                               StringView pathText) noexcept {
    TextDiagnosticCode diagnostic = TextDiagnosticCode::InvalidValue;
    StringView message = "value does not fit the property";
    switch (failure.reason) {
    case PropertyPathError::ReadOnly:
        diagnostic = TextDiagnosticCode::ReadOnly;
        message = "property is read-only";
        break;
    case PropertyPathError::TooManyElements:
    case PropertyPathError::IndexOutOfRange:
        diagnostic = TextDiagnosticCode::OutOfRange;
        message = "element count or index out of range";
        break;
    case PropertyPathError::KeyNotFound:
        message = "key not found";
        break;
    case PropertyPathError::OptionalUnset:
        message = "optional value is unset";
        break;
    case PropertyPathError::InvalidValue:
    case PropertyPathError::InvalidKey:
    case PropertyPathError::NotOptional:
    case PropertyPathError::NotMap:
    case PropertyPathError::NotArray:
    case PropertyPathError::NotStruct:
    case PropertyPathError::NotKeyed:
    case PropertyPathError::IndexRequired:
    case PropertyPathError::KeyRequired:
    case PropertyPathError::IndexNotAllowed:
        break;
    case PropertyPathError::AggregateValue:
    case PropertyPathError::NoPathAccess:
    case PropertyPathError::FixedLength:
    case PropertyPathError::NotCopyable:
        diagnostic = TextDiagnosticCode::Unsupported;
        message = failure.reason == PropertyPathError::FixedLength
                      ? StringView("fixed-length array elements cannot be inserted or removed")
                  : failure.reason == PropertyPathError::NotCopyable
                      ? StringView("value cannot be copied for an assignment batch")
                      : StringView("property cannot be assigned as text");
        break;
    case PropertyPathError::None:
        switch (code) {
        case ErrorCode::InvalidArgument:
        case ErrorCode::TypeMismatch:
        case ErrorCode::NotFound:
            break;
        case ErrorCode::Overflow:
            diagnostic = TextDiagnosticCode::OutOfRange;
            message = "integer out of range";
            break;
        case ErrorCode::Unsupported:
            diagnostic = TextDiagnosticCode::Unsupported;
            message = "value is not supported here";
            break;
        default:
            return Result<void>::Failure(code);
        }

        break;
    default:
        return Result<void>::Failure(code);
    }

    return Diagnose(state, diagnostic, source, pathText, message);
}

Result<AssignmentUnit*> AcquireUnit(AssignmentState& state, PropertyPath unitPath,
                                    TextSource source, StringView pathText) noexcept {
    for (usize index = 0; index < state.units.Size(); ++index) {
        if (SamePropertyPath(state.units[index]->Path(), unitPath)) {
            return Result<AssignmentUnit*>::Success(state.units[index].Get());
        }
    }

    auto made = UniquePtr<AssignmentUnit>::TryMake(state.allocator, state.target.ValueAllocator(),
                                                   state.allocator);
    if (!made) {
        return Result<AssignmentUnit*>::Failure(made.Error());
    }

    AssignmentUnit& unit = *made.Value();
    if (auto copied = unit.steps.TryAppend(unitPath.steps); !copied) {
        return Result<AssignmentUnit*>::Failure(copied.Error());
    }

    if (auto reserved = state.units.TryReserve(state.units.Size() + 1); !reserved) {
        return Result<AssignmentUnit*>::Failure(reserved.Error());
    }

    PropertyPathFailure failure;
    auto detached = PathDetach(state.target.Registry(), state.target.Access(), state.Root(),
                               unitPath, unit.box, &failure);
    if (!detached) {
        if (auto reported = ReportPathFailure(state, detached.Error(), failure, source, pathText);
            !reported) {
            return Result<AssignmentUnit*>::Failure(reported.Error());
        }

        return Result<AssignmentUnit*>::Success(nullptr);
    }

    (void)state.units.TryPushBack(Move(made.Value()));
    return Result<AssignmentUnit*>::Success(&unit);
}

Result<ContainerUnit*> AcquireContainer(AssignmentState& state, PropertyPath unitPath,
                                        TextSource source, StringView pathText) noexcept {
    for (usize index = 0; index < state.containers.Size(); ++index) {
        if (SamePropertyPath(state.containers[index]->Path(), unitPath)) {
            return Result<ContainerUnit*>::Success(state.containers[index].Get());
        }
    }

    auto made = UniquePtr<ContainerUnit>::TryMake(state.allocator, state.allocator);
    if (!made) {
        return Result<ContainerUnit*>::Failure(made.Error());
    }

    ContainerUnit& unit = *made.Value();
    if (auto copied = unit.steps.TryAppend(unitPath.steps); !copied) {
        return Result<ContainerUnit*>::Failure(copied.Error());
    }

    if (auto reserved = state.containers.TryReserve(state.containers.Size() + 1); !reserved) {
        return Result<ContainerUnit*>::Failure(reserved.Error());
    }

    // 원소 수 읽기가 컨테이너를 사본에 묶는다(지금 내용 복사 — 객체는 읽기 Borrow).
    PropertyPathFailure failure;
    auto bound = PathCount(state.target.Registry(), state.target.Access(),
                           state.Root().WithReferences(unit.stage), unitPath, &failure);
    if (!bound) {
        if (auto reported = ReportPathFailure(state, bound.Error(), failure, source, pathText);
            !reported) {
            return Result<ContainerUnit*>::Failure(reported.Error());
        }

        return Result<ContainerUnit*>::Success(nullptr);
    }

    (void)state.containers.TryPushBack(Move(made.Value()));
    return Result<ContainerUnit*>::Success(&unit);
}

Result<ElementsUnit*> AcquireElements(AssignmentState& state, PropertyPath unitPath,
                                      TextSource source, StringView pathText) noexcept {
    for (usize index = 0; index < state.elements.Size(); ++index) {
        if (SamePropertyPath(state.elements[index]->Path(), unitPath)) {
            return Result<ElementsUnit*>::Success(state.elements[index].Get());
        }
    }

    auto made = UniquePtr<ElementsUnit>::TryMake(state.allocator, state.allocator);
    if (!made) {
        return Result<ElementsUnit*>::Failure(made.Error());
    }

    ElementsUnit& unit = *made.Value();
    if (auto copied = unit.steps.TryAppend(unitPath.steps); !copied) {
        return Result<ElementsUnit*>::Failure(copied.Error());
    }

    if (auto reserved = state.elements.TryReserve(state.elements.Size() + 1); !reserved) {
        return Result<ElementsUnit*>::Failure(reserved.Error());
    }

    // 배열에 묶는다(원소 수·변경 계수 기록 — 객체는 읽기 Borrow).
    PropertyPathFailure failure;
    auto bound = PathPrepareStructElements(state.target.Registry(), state.target.Access(),
                                           state.Root(), unitPath, unit.stage, &failure);
    if (!bound) {
        if (auto reported = ReportPathFailure(state, bound.Error(), failure, source, pathText);
            !reported) {
            return Result<ElementsUnit*>::Failure(reported.Error());
        }

        return Result<ElementsUnit*>::Success(nullptr);
    }

    (void)state.elements.TryPushBack(Move(made.Value()));
    return Result<ElementsUnit*>::Success(&unit);
}

Result<void> AddOverride(AssignmentState& state, PropertyPath path) noexcept {
    // 위에서부터 처음 만나는 컨테이너 필드·Atomic struct에서 자른다.
    usize count = path.steps.Size();
    for (usize index = 0; index < path.steps.Size(); ++index) {
        TextSlot slot;
        const PropertyPath prefix{Span<const PathStep>(path.steps.Data(), index + 1), path.keys};
        if (auto described = DescribeTextPath(state.lease, prefix, slot); !described) {
            return described;
        }

        const FieldSchema& field = *slot.field;
        const bool atomic =
            field.value.kind == ValueKind::Struct && !slot.structView.IsEmpty() &&
            (slot.structView.Schema().flags & static_cast<uint32>(TypeFlags::Atomic)) != 0;
        if (ValueKindIsContainer(field.value.kind) || atomic) {
            count = index + 1;
            break;
        }
    }

    const usize start =
        state.overrideEnds.IsEmpty() ? 0 : state.overrideEnds[state.overrideEnds.Size() - 1];
    for (usize entry = 0; entry < state.overrideViews.Size(); ++entry) {
        const PropertyPath& existing = state.overrideViews[entry];
        if (existing.steps.Size() != count) {
            continue;
        }

        bool same = true;
        for (usize index = 0; index < count && same; ++index) {
            same = existing.steps[index].fieldId == path.steps[index].fieldId;
        }

        if (same) {
            return Result<void>::Success();
        }
    }

    if (auto reserved = state.overrideSteps.TryReserve(start + count); !reserved) {
        return reserved;
    }

    if (auto reserved = state.overrideEnds.TryReserve(state.overrideEnds.Size() + 1); !reserved) {
        return reserved;
    }

    if (auto reserved = state.overrideViews.TryReserve(state.overrideEnds.Size() + 1); !reserved) {
        return reserved;
    }

    for (usize index = 0; index < count; ++index) {
        PathStep step;
        step.fieldId = path.steps[index].fieldId;
        (void)state.overrideSteps.TryPushBack(step);
    }

    (void)state.overrideEnds.TryPushBack(start + count);
    // 단계 버퍼가 옮겨졌을 수 있으므로 view를 모두 다시 만든다.
    state.overrideViews.Clear();
    usize begin = 0;
    for (usize entry = 0; entry < state.overrideEnds.Size(); ++entry) {
        const usize end = state.overrideEnds[entry];
        (void)state.overrideViews.TryPushBack(PropertyPath{
            Span<const PathStep>(state.overrideSteps.View().Data() + begin, end - begin), {}});
        begin = end;
    }

    return Result<void>::Success();
}

} // namespace detail

bool MakeTextAssignment(StringView text, uint32 sourceIndex, TextAssignment& out) noexcept {
    StringView path;
    StringView value;
    if (!SplitTextAssignment(text, path, value)) {
        return false;
    }

    out.kind = AssignmentKind::Set;
    out.path = path;
    out.value = value;
    out.sourceIndex = sourceIndex;
    return true;
}

AssignmentTarget AssignmentTarget::Live(ObjectRegistry& registry, const ObjectAccessContext& access,
                                        ObjectHandle object) noexcept {
    AssignmentTarget target;
    target.kind_ = Kind::Live;
    target.registry_ = &registry;
    target.access_ = &access;
    target.object_ = object;
    return target;
}

AssignmentTarget AssignmentTarget::Staged(ConstructionContext& transaction,
                                          ObjectHandle object) noexcept {
    AssignmentTarget target;
    target.kind_ = Kind::Staged;
    target.registry_ = &transaction.Registry();
    target.access_ = &transaction.Access();
    target.object_ = object;
    target.transaction_ = &transaction;
    return target;
}

AssignmentTarget AssignmentTarget::Value(ObjectRegistry& registry,
                                         const ObjectAccessContext& access,
                                         const TypeLease& structType, void* storage,
                                         Allocator& allocator) noexcept {
    AssignmentTarget target;
    target.kind_ = Kind::Value;
    target.registry_ = &registry;
    target.access_ = &access;
    target.structType_ = &structType;
    target.storage_ = storage;
    target.allocator_ = &allocator;
    return target;
}

Allocator& AssignmentTarget::ValueAllocator() const noexcept {
    return allocator_ ? *allocator_ : registry_->ObjectAllocator();
}

PublishedAssignments::PublishedAssignments() noexcept = default;
PublishedAssignments::~PublishedAssignments() = default;
PublishedAssignments::PublishedAssignments(PublishedAssignments&&) noexcept = default;
PublishedAssignments& PublishedAssignments::operator=(PublishedAssignments&&) noexcept = default;

Span<const PropertyPath> PublishedAssignments::OverridePaths() const noexcept {
    return state_ ? state_->overrideViews.View() : Span<const PropertyPath>();
}

usize PublishedAssignments::UnitCount() const noexcept {
    return state_ ? state_->units.Size() : 0;
}

PropertyPath PublishedAssignments::UnitPath(usize index) const noexcept {
    return state_->units[index]->Path();
}

DetachedValue& PublishedAssignments::OldValue(usize index) noexcept {
    return state_->units[index]->box;
}

usize PublishedAssignments::ReferenceCount() const noexcept {
    return state_ ? state_->references.Size() : 0;
}

PropertyPath PublishedAssignments::ReferencePath(usize index) const noexcept {
    return state_->references[index]->Path();
}

ObjectHandle PublishedAssignments::OldReference(usize index) const noexcept {
    return state_->references[index]->previous;
}

usize PublishedAssignments::ContainerCount() const noexcept {
    return state_ ? state_->containers.Size() : 0;
}

PropertyPath PublishedAssignments::ContainerPath(usize index) const noexcept {
    return state_->containers[index]->Path();
}

Span<const ObjectHandle> PublishedAssignments::OldReferences(usize index) const noexcept {
    return state_->containers[index]->stage.Previous();
}

PreparedAssignments::PreparedAssignments() noexcept = default;
PreparedAssignments::~PreparedAssignments() = default;
PreparedAssignments::PreparedAssignments(PreparedAssignments&&) noexcept = default;
PreparedAssignments& PreparedAssignments::operator=(PreparedAssignments&&) noexcept = default;

Result<void> PreparedAssignments::Append(const TextAssignment& assignment) noexcept {
    if (!state_ || state_->consumed || state_->failed) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (auto processed = detail::ProcessAssignment(*state_, assignment); !processed) {
        return processed;
    }

    return state_->failed ? Result<void>::Failure(ErrorCode::InvalidArgument)
                          : Result<void>::Success();
}

Span<const PropertyPath> PreparedAssignments::OverridePaths() const noexcept {
    return state_ ? state_->overrideViews.View() : Span<const PropertyPath>();
}

bool PreparedAssignments::Touches(PropertyPath unit) const noexcept {
    if (!state_) {
        return false;
    }

    const Span<const PropertyPath> paths = state_->overrideViews.View();
    for (usize entry = 0; entry < paths.Size(); ++entry) {
        const PropertyPath& path = paths[entry];
        const usize common =
            path.steps.Size() < unit.steps.Size() ? path.steps.Size() : unit.steps.Size();
        bool same = common > 0;
        for (usize index = 0; index < common && same; ++index) {
            same = path.steps[index].fieldId == unit.steps[index].fieldId;
        }

        if (same) {
            return true;
        }
    }

    return false;
}

Result<PublishedAssignments> PreparedAssignments::Publish() noexcept {
    using PublishResult = Result<PublishedAssignments>;
    if (!state_ || state_->consumed || state_->failed) {
        return PublishResult::Failure(ErrorCode::InvalidArgument);
    }

    detail::AssignmentState& state = *state_;
    state.consumed = true;
    ObjectRegistry& registry = state.target.Registry();
    const ObjectAccessContext& access = state.target.Access();
    // 사본을 바꾸지 않은 단위(값이 같은 대입뿐)는 게시하지 않는다 — 먼저 앞으로 모으고 나머지를
    // 비운다(게시 구간 밖).
    usize kept = 0;
    for (usize index = 0; index < state.units.Size(); ++index) {
        if (state.units[index]->dirty) {
            if (kept != index) {
                state.units[kept] = Move(state.units[index]);
            }

            ++kept;
        }
    }

    if (kept < state.units.Size()) {
        (void)state.units.TryRemoveAt(kept, state.units.Size() - kept);
    }

    // 컨테이너 단위도 사본을 바꾸지 않았으면 게시하지 않는다.
    kept = 0;
    for (usize index = 0; index < state.containers.Size(); ++index) {
        if (state.containers[index]->dirty) {
            if (kept != index) {
                state.containers[kept] = Move(state.containers[index]);
            }

            ++kept;
        }
    }

    if (kept < state.containers.Size()) {
        (void)state.containers.TryRemoveAt(kept, state.containers.Size() - kept);
    }

    // 원소 지우기 단위도 지운 원소가 없으면 게시하지 않는다.
    kept = 0;
    for (usize index = 0; index < state.elements.Size(); ++index) {
        if (state.elements[index]->stage.HasRemovals()) {
            if (kept != index) {
                state.elements[kept] = Move(state.elements[index]);
            }

            ++kept;
        }
    }

    if (kept < state.elements.Size()) {
        (void)state.elements.TryRemoveAt(kept, state.elements.Size() - kept);
    }

    // 1. 게시 자리(읽기 Borrow — 변경 표지를 올리지 않는다). 죽은 owner(Discard·파괴 요청 — 경로
    // 연산은 Destroyed)는 BeginBatchPublish와 같이 StaleHandle로 알린다.
    const bool live = state.target.GetKind() == AssignmentTarget::Kind::Live;
    const auto ownerError = [live](ErrorCode code) noexcept {
        return live && code == ErrorCode::Destroyed ? ErrorCode::StaleHandle : code;
    };
    const PathTarget root = state.Root();
    for (usize index = 0; index < state.units.Size(); ++index) {
        detail::AssignmentUnit& unit = *state.units[index];
        if (auto located =
                PathPrepareAttach(registry, access, root, unit.Path(), unit.box, unit.prepared);
            !located) {
            return PublishResult::Failure(ownerError(located.Error()));
        }
    }

    // 참조 단위: 게시된 객체는 자리와 옛 대상을 지금 찾는다(대상 생존·타입도 다시 검증한다).
    FieldValue reference(state.allocator);
    reference.kind = ValueKind::ObjectReference;
    if (live) {
        for (usize index = 0; index < state.references.Size(); ++index) {
            detail::ReferenceUnit& unit = *state.references[index];
            reference.reference = unit.target;
            if (auto located = PathPrepareReference(registry, access, root, unit.Path(), reference,
                                                    unit.prepared);
                !located) {
                return PublishResult::Failure(ownerError(located.Error()));
            }

            unit.previous = unit.prepared.Previous();
        }

        // 컨테이너 단위: owner 생존과 같은 컨테이너인지 다시 확인하고(사본을 건 원소 수 읽기) 새
        // 버퍼를 만든다(구간 밖 할당). 옛 내용은 변경 표지가 묶을 때와 같음을 보증한다.
        for (usize index = 0; index < state.containers.Size(); ++index) {
            detail::ContainerUnit& unit = *state.containers[index];
            if (auto located =
                    PathCount(registry, access, root.WithReferences(unit.stage), unit.Path());
                !located) {
                return PublishResult::Failure(ownerError(located.Error()));
            }

            if (auto prepared = unit.stage.PreparePublish(); !prepared) {
                return PublishResult::Failure(prepared.Error());
            }
        }

        // 원소 지우기 단위: owner 생존·같은 배열·변경 계수를 다시 확인하고 지울 원소 안의 참조를
        // 모은 뒤(간선 변경 {참조, null}) 떼어 낼 블록 목록을 잡는다(구간 밖 할당).
        for (usize index = 0; index < state.elements.Size(); ++index) {
            detail::ElementsUnit& unit = *state.elements[index];
            if (auto located =
                    PathPrepareStructElements(registry, access, root, unit.Path(), unit.stage);
                !located) {
                return PublishResult::Failure(ownerError(located.Error()));
            }

            if (auto prepared = unit.stage.PreparePublish(); !prepared) {
                return PublishResult::Failure(prepared.Error());
            }
        }
    }

    if (live) {
        // 2. owner Borrow와 묶음 게시 구간(표지·새 대상 재확인·관찰자 예약) → 무할당 게시.
        // 컨테이너는 옛 원소마다 {원소, null}, 새 원소마다 {null, 원소}를 선언한다(그대로 남는
        // 원소는 검사합에서 상쇄된다).
        usize targetCount = state.pins.Size() + state.references.Size();
        usize edgeCount = state.references.Size();
        for (usize index = 0; index < state.containers.Size(); ++index) {
            const PreparedReferences& stage = state.containers[index]->stage;
            targetCount += stage.Current().Size();
            edgeCount += stage.Previous().Size() + stage.Current().Size();
        }

        for (usize index = 0; index < state.elements.Size(); ++index) {
            edgeCount += state.elements[index]->stage.RemovedReferences().Size();
        }

        Array<BatchTargetCheck> targets(state.allocator);
        Array<BatchEdgeChange> edges(state.allocator);
        if (auto reserved = targets.TryReserve(targetCount); !reserved) {
            return PublishResult::Failure(reserved.Error());
        }

        if (auto reserved = edges.TryReserve(edgeCount); !reserved) {
            return PublishResult::Failure(reserved.Error());
        }

        for (usize index = 0; index < state.containers.Size(); ++index) {
            const PreparedReferences& stage = state.containers[index]->stage;
            for (const ObjectHandle previous : stage.Previous()) {
                if (!previous.IsNull()) {
                    (void)edges.TryPushBack(BatchEdgeChange{previous, ObjectHandle{}});
                }
            }

            for (const ObjectHandle current : stage.Current()) {
                if (!current.IsNull()) {
                    (void)targets.TryPushBack(BatchTargetCheck{current, stage.TargetType()});
                    (void)edges.TryPushBack(BatchEdgeChange{ObjectHandle{}, current});
                }
            }
        }

        for (usize index = 0; index < state.elements.Size(); ++index) {
            for (const ObjectHandle removed : state.elements[index]->stage.RemovedReferences()) {
                (void)edges.TryPushBack(BatchEdgeChange{removed, ObjectHandle{}});
            }
        }

        // Weak 고정은 생존만, 참조 단위의 새 대상은 자리의 C++ 대상 타입까지 본다.
        for (usize index = 0; index < state.pins.Size(); ++index) {
            (void)targets.TryPushBack(BatchTargetCheck{state.pins[index].Handle(), nullptr});
        }

        for (usize index = 0; index < state.references.Size(); ++index) {
            const detail::ReferenceUnit& unit = *state.references[index];
            if (!unit.target.IsNull()) {
                (void)targets.TryPushBack(
                    BatchTargetCheck{unit.target, unit.prepared.TargetType()});
            }

            (void)edges.TryPushBack(BatchEdgeChange{unit.previous, unit.target});
        }

        auto owner = registry.Resolve(access, state.target.Handle());
        if (!owner) {
            return PublishResult::Failure(ownerError(owner.Error()));
        }

        auto batch = registry.BeginBatchPublish(access, state.target.Handle(), state.stamp,
                                                targets.View(), edges.View());
        if (!batch) {
            return PublishResult::Failure(batch.Error());
        }

        for (usize index = 0; index < state.units.Size(); ++index) {
            state.units[index]->prepared.Publish(batch.Value());
        }

        for (usize index = 0; index < state.references.Size(); ++index) {
            detail::ReferenceUnit& unit = *state.references[index];
            unit.prepared.Publish(batch.Value(), unit.target);
        }

        for (usize index = 0; index < state.containers.Size(); ++index) {
            state.containers[index]->stage.Publish(batch.Value());
        }

        // 원소 압축은 마지막이다(앞 단위는 원소를 묶을 때 index로 찾았다).
        for (usize index = 0; index < state.elements.Size(); ++index) {
            state.elements[index]->stage.Publish(batch.Value());
        }

        batch.Value().End();
        // 떼어 낸 원소는 구간 밖에서 소멸한다.
        for (usize index = 0; index < state.elements.Size(); ++index) {
            state.elements[index]->stage.ReleaseRemoved();
        }
    } else {
        for (usize index = 0; index < state.units.Size(); ++index) {
            if (auto published = state.units[index]->prepared.Publish(); !published) {
                return PublishResult::Failure(published.Error());
            }
        }

        // staged 객체의 참조는 트랜잭션의 staged 간선 쓰기다(실패는 호출자의 rollback이 받는다).
        for (usize index = 0; index < state.references.Size(); ++index) {
            detail::ReferenceUnit& unit = *state.references[index];
            reference.reference = unit.target;
            if (auto written = PathWrite(registry, access, root, unit.Path(), reference);
                !written) {
                return PublishResult::Failure(written.Error());
            }
        }

        // staged 객체의 컨테이너는 트랜잭션의 AssignReferences·AssignMapReferences다.
        for (usize index = 0; index < state.containers.Size(); ++index) {
            if (auto published = state.containers[index]->stage.Publish(); !published) {
                return PublishResult::Failure(published.Error());
            }
        }

        // staged 객체의 원소 지우기는 트랜잭션 안 압축이다(앞 단위 뒤 — 원소를 묶을 때 index로
        // 찾았다).
        for (usize index = 0; index < state.elements.Size(); ++index) {
            if (auto published = state.elements[index]->stage.Publish(); !published) {
                return PublishResult::Failure(published.Error());
            }
        }
    }

    // 3. 대상 고정을 놓는다.
    state.pins.Clear();
    PublishedAssignments published;
    published.state_ = Move(state_);
    return PublishResult::Success(Move(published));
}

Result<PreparedAssignments> PrepareAssignments(const AssignmentTarget& target,
                                               Span<const TextAssignment> assignments,
                                               const TextCodecOptions& options,
                                               TextDiagnostics& diagnostics) noexcept {
    using PrepareResult = Result<PreparedAssignments>;
    ObjectRegistry& registry = target.Registry();
    TextCodecOptions effective = options;
    if (!effective.names) {
        effective.names = registry.Names();
    } else if (effective.names != registry.Names()) {
        return PrepareResult::Failure(ErrorCode::InvalidArgument);
    }

    if (target.GetKind() == AssignmentTarget::Kind::Value) {
        const TypeLease* type = target.StructType();
        if (!type || !type->IsValid() || type->Kind() != TypeKind::Struct || !target.Storage()) {
            return PrepareResult::Failure(ErrorCode::InvalidArgument);
        }

        if (type->Struct().ContainsStrongReference()) {
            return PrepareResult::Failure(ErrorCode::Unsupported);
        }
    }

    Allocator& allocator = target.ValueAllocator();
    auto made = UniquePtr<detail::AssignmentState>::TryMake(allocator, target, effective,
                                                            diagnostics, allocator);
    if (!made) {
        return PrepareResult::Failure(made.Error());
    }

    detail::AssignmentState& state = *made.Value();
    if (auto started = state.context.Start(); !started) {
        return PrepareResult::Failure(started.Error());
    }

    // 대상 타입 lease와 Live 대상의 변경 표지(사본을 만들기 전에 읽는다).
    auto lease = target.GetKind() == AssignmentTarget::Kind::Live
                     ? registry.AcquireType(target.Access(), target.Handle())
                 : target.GetKind() == AssignmentTarget::Kind::Staged
                     ? target.Transaction()->AcquireType(target.Handle())
                     : target.StructType()->Clone();
    if (!lease) {
        return PrepareResult::Failure(lease.Error());
    }

    state.lease = Move(lease.Value());
    if (target.GetKind() == AssignmentTarget::Kind::Live) {
        auto stamp = registry.MutationStamp(target.Access(), target.Handle());
        if (!stamp) {
            return PrepareResult::Failure(stamp.Error());
        }

        state.stamp = stamp.Value();
    }

    for (usize index = 0; index < assignments.Size(); ++index) {
        if (state.context.Diagnostics().Full()) {
            break;
        }

        if (auto processed = detail::ProcessAssignment(state, assignments[index]); !processed) {
            return PrepareResult::Failure(processed.Error());
        }
    }

    if (state.failed) {
        return PrepareResult::Failure(ErrorCode::InvalidArgument);
    }

    PreparedAssignments prepared;
    prepared.state_ = Move(made.Value());
    return PrepareResult::Success(Move(prepared));
}

} // namespace rocks::reflection_text
