#include "TextAssignInternal.h"

#include <Object/NameField.h>
#include <Object/PathKey.h>

// 대입 하나를 해석해 준비 단위 사본에 적용한다(리플렉션 B9i3c1 — ReflectionText/TextAssignments.h의
// PrepareAssignments 규칙). 경로 해석 → 같은 경로 검사 → 준비 단위(참조 없는 필드·멤버 하나, 또는
// 단일 강한 참조 위치 — B9i3c2) → 값 읽기(resolver) → 사본 위 경로 연산 또는 참조 자리 검증 →
// override 경로.
namespace rocks::reflection_text::detail {

namespace {

bool SameBytes(const void* left, const void* right, usize size) noexcept {
    const auto* a = static_cast<const uint8*>(left);
    const auto* b = static_cast<const uint8*>(right);
    for (usize index = 0; index < size; ++index) {
        if (a[index] != b[index]) {
            return false;
        }
    }

    return true;
}

// 두 단일 값이 같은 값인지다(실수는 bit 단위 — NaN payload·-0도 구별한다).
bool SameValue(const FieldValue& left, const FieldValue& right) noexcept {
    if (left.kind != right.kind || left.present != right.present) {
        return false;
    }

    if (!left.present) {
        return true;
    }

    switch (left.kind) {
    case ValueKind::Bool:
        return left.boolean == right.boolean;
    case ValueKind::UInt8:
    case ValueKind::UInt16:
    case ValueKind::UInt32:
    case ValueKind::UInt64:
        return left.unsignedInteger == right.unsignedInteger;
    case ValueKind::Int8:
    case ValueKind::Int16:
    case ValueKind::Int32:
    case ValueKind::Int64:
        return left.signedInteger == right.signedInteger;
    case ValueKind::Enum:
        return left.signedInteger == right.signedInteger && left.type == right.type;
    case ValueKind::Float32:
        return SameBytes(&left.float32, &right.float32, sizeof(float));
    case ValueKind::Float64:
        return SameBytes(&left.float64, &right.float64, sizeof(double));
    case ValueKind::Guid:
    case ValueKind::AssetId:
    case ValueKind::ClassReference:
        return left.guid == right.guid;
    case ValueKind::Utf8String:
        return left.string.View() == right.string.View();
    case ValueKind::SoftObjectReference:
        return left.guid == right.guid && left.string.View() == right.string.View();
    case ValueKind::Digest256:
        return left.digest == right.digest;
    case ValueKind::Name:
        return left.name == right.name;
    case ValueKind::ObjectReference:
    case ValueKind::WeakObjectReference:
        return left.reference == right.reference;
    default:
        return false;
    }
}

// 호출 오류(대입 내용과 무관한 실패)인지다.
bool IsCallError(ErrorCode code) noexcept {
    return code == ErrorCode::OutOfMemory || code == ErrorCode::Busy ||
           code == ErrorCode::StaleHandle || code == ErrorCode::Destroyed ||
           code == ErrorCode::WrongThread || code == ErrorCode::WrongContext ||
           code == ErrorCode::ShuttingDown;
}

// 새로 가리키는 Hard·Weak 대상을 고정한다(Publish가 다시 확인한다 — 고정은 GC 회수를 막지만
// Discard는 막지 못한다). 살아 있지 않으면 진단이다.
Result<bool> PinReferences(AssignmentState& state, const TextValue& value, TextSource source,
                           StringView pathText) noexcept {
    const FieldValue* lists[] = {&value.single};
    const Span<const FieldValue> groups[] = {Span<const FieldValue>(lists[0], 1),
                                             value.elements.View(), value.values.View()};
    for (const Span<const FieldValue>& group : groups) {
        for (usize index = 0; index < group.Size(); ++index) {
            const FieldValue& item = group[index];
            if ((item.kind != ValueKind::WeakObjectReference &&
                 item.kind != ValueKind::ObjectReference) ||
                item.reference.IsNull()) {
                continue;
            }

            auto pinned = state.target.Registry().TryPin(state.target.Access(), item.reference);
            if (!pinned) {
                if (IsCallError(pinned.Error()) && pinned.Error() != ErrorCode::StaleHandle) {
                    return Result<bool>::Failure(pinned.Error());
                }

                if (auto reported = Diagnose(state, TextDiagnosticCode::InvalidValue, source,
                                             pathText, "referenced object is not alive");
                    !reported) {
                    return Result<bool>::Failure(reported.Error());
                }

                return Result<bool>::Success(false);
            }

            if (auto kept = state.pins.TryPushBack(Move(pinned.Value())); !kept) {
                return Result<bool>::Failure(kept.Error());
            }
        }
    }

    return Result<bool>::Success(true);
}

// Map 키·Set 원소 값의 key wire를 wire 뒤에 붙인다.
Result<void> AppendKeyWire(AssignmentState& state, const FieldValue& value,
                           Array<uint8>& wire) noexcept {
    PathKeyValue key;
    key.kind = value.kind;
    key.boolean = value.boolean;
    key.unsignedInteger = value.unsignedInteger;
    key.signedInteger = value.signedInteger;
    key.guid = value.guid;
    key.digest = value.digest;
    key.text = value.string.View();
    if (value.kind != ValueKind::Name) {
        return AppendPathKey(key, wire);
    }

    auto display = AcquireNameFieldText(state.options.names, value.name, state.allocator);
    if (!display) {
        return Result<void>::Failure(display.Error());
    }

    key.text = display.Value().View();
    return AppendPathKey(key, wire);
}

// 멤버마다 나누는 적용이 이번 호출의 진단만 보도록 묶음의 실패 표시를 잠시 내려 두고 나갈 때
// 합친다(앞 대입의 진단이 뒤 대입의 진단 모으기를 멈추지 않게 한다).
class FreshFailure {
public:
    explicit FreshFailure(AssignmentState& state) noexcept
        : state_(state), previous_(state.failed) {
        state_.failed = false;
    }

    ~FreshFailure() {
        state_.failed = state_.failed || previous_;
    }

    FreshFailure(const FreshFailure&) = delete;
    FreshFailure& operator=(const FreshFailure&) = delete;

private:
    AssignmentState& state_;
    bool previous_;
};

// 대입 하나의 적용 문맥이다. 값 단위는 상자(start = PathTarget::Value(box), relative는 상자 기준
// 경로), Registry 컨테이너 단위는 준비 사본을 건 대상(start = Root().WithReferences, relative는
// 대상 기준 경로 — box 없음)이다.
struct Apply {
    AssignmentState& state;
    PathTarget start;
    bool& dirty;
    DetachedValue* box;
    const TextAssignment& assignment;
    const TextSlot& slot;
    // 대상 기준 끝 경로(override 경로의 원본)와 start 기준 경로.
    PropertyPath path;
    PropertyPath relative;
    TextSource source;
    PropertyPathFailure failure{};

    ObjectRegistry& Registry() const noexcept {
        return state.target.Registry();
    }

    const ObjectAccessContext& Access() const noexcept {
        return state.target.Access();
    }

    PathTarget Start() const noexcept {
        return start;
    }

    Result<void> Diagnostic(TextDiagnosticCode code, StringView message) noexcept {
        return Diagnose(state, code, source, assignment.path, message);
    }

    Result<void> Failed(ErrorCode code) noexcept {
        return ReportPathFailure(state, code, failure, source, assignment.path);
    }

    // 사본을 바꾼 대입을 기록한다.
    Result<void> Changed() noexcept {
        dirty = true;
        return AddOverride(state, path);
    }

    Result<void> Run() noexcept;
    Result<void> RunSet(TextValue& value) noexcept;
    Result<void> ResetStruct() noexcept;
    // 상자 안 자리 at(필드·멤버 하나)을 떼어 기본값으로 바꿔 되돌린다.
    Result<void> ResetAt(PropertyPath at) noexcept;
    // 정적 배열 struct 원소의 멤버마다 기본값으로 되돌린다(리플렉션 B9i3c3).
    Result<void> ResetMembers() noexcept;
    Result<void> ReplaceContents(const TextValue& value) noexcept;
};

Result<void> Apply::Run() noexcept {
    const bool readOnly = state.target.GetKind() != AssignmentTarget::Kind::Staged &&
                          (slot.flags & static_cast<uint32>(FieldFlags::ReadOnly)) != 0;
    const ValueKind fieldKind = slot.field->value.kind;
    switch (assignment.kind) {
    case AssignmentKind::Unset: {
        if (slot.part != TextSlotPart::Field || fieldKind != ValueKind::Optional) {
            return Diagnostic(TextDiagnosticCode::InvalidValue,
                              "only an Optional property can be unset");
        }

        if (readOnly) {
            return Diagnostic(TextDiagnosticCode::ReadOnly, "property is read-only");
        }

        Result<void> done = Result<void>::Success();
        if (slot.field->element.kind == ValueKind::Struct) {
            done = PathResetOptional(Registry(), Access(), Start(), relative, &failure);
        } else {
            FieldValue unset(state.allocator);
            unset.kind = slot.field->element.kind;
            unset.present = false;
            done = PathWrite(Registry(), Access(), Start(), relative, unset, nullptr, &failure);
        }

        return done ? Changed() : Failed(done.Error());
    }
    case AssignmentKind::Erase: {
        if (slot.part == TextSlotPart::Field) {
            return Diagnostic(TextDiagnosticCode::InvalidValue,
                              "erase needs a key or an element index");
        }

        if (readOnly) {
            return Diagnostic(TextDiagnosticCode::ReadOnly, "property is read-only");
        }

        auto done =
            slot.part == TextSlotPart::Element
                ? PathRemoveElement(Registry(), Access(), Start(), relative, nullptr, &failure)
                : PathEraseKey(Registry(), Access(), Start(), relative, nullptr, &failure);
        return done ? Changed() : Failed(done.Error());
    }
    case AssignmentKind::Set:
        break;
    default:
        return Diagnostic(TextDiagnosticCode::InvalidValue, "unknown assignment kind");
    }

    if (slot.part == TextSlotPart::SetElement) {
        // Set 원소 자리는 값이 없다: 빈 값이 원소 넣기다.
        if (!assignment.value.IsEmpty()) {
            return Diagnostic(TextDiagnosticCode::InvalidValue, "a Set element takes no value");
        }

        if (readOnly) {
            return Diagnostic(TextDiagnosticCode::ReadOnly, "property is read-only");
        }

        auto inserted = PathInsertKey(Registry(), Access(), Start(), relative, &failure);
        return inserted ? Changed() : Failed(inserted.Error());
    }

    TextValue value(state.allocator);
    auto parsed =
        ParseTextValue(state.context, slot, assignment.value, source, assignment.path, value);
    if (!parsed) {
        return Result<void>::Failure(parsed.Error());
    }

    if (!parsed.Value()) {
        state.failed = true;
        return Result<void>::Success();
    }

    auto pinned = PinReferences(state, value, source, assignment.path);
    if (!pinned || !pinned.Value()) {
        return pinned ? Result<void>::Success() : Result<void>::Failure(pinned.Error());
    }

    if (value.shape == TextValueShape::Single) {
        // 지금 값과 같으면 무동작이다(ReadOnly·Deprecated ClassRef의 같은 값 재대입 포함).
        FieldValue current(state.allocator);
        PropertyPathFailure readFailure;
        auto read = PathRead(Registry(), Access(), Start(), relative, current, &readFailure);
        if (!read && read.Error() == ErrorCode::OutOfMemory) {
            return read;
        }

        if (read && SameValue(current, value.single)) {
            return readOnly ? Result<void>::Success() : AddOverride(state, path);
        }
    }

    if (readOnly) {
        return Diagnostic(TextDiagnosticCode::ReadOnly, "property is read-only");
    }

    return RunSet(value);
}

Result<void> Apply::RunSet(TextValue& value) noexcept {
    const ValueKind fieldKind = slot.field->value.kind;
    switch (value.shape) {
    case TextValueShape::Single: {
        auto written =
            PathWrite(Registry(), Access(), Start(), relative, value.single, nullptr, &failure);
        return written ? Changed() : Failed(written.Error());
    }
    case TextValueShape::StructDefault:
        if (slot.part == TextSlotPart::MapValue) {
            auto inserted = PathInsertKey(Registry(), Access(), Start(), relative, &failure);
            return inserted ? Changed() : Failed(inserted.Error());
        }

        if (slot.part == TextSlotPart::Element) {
            if (fieldKind == ValueKind::StaticArray) {
                // 길이가 고정이라 지우고 넣을 수 없다: 원소 멤버마다 기본값으로 되돌린다.
                if (auto reset = ResetMembers(); !reset || state.failed) {
                    return reset;
                }

                return Changed();
            }

            // Array<struct> 원소: 그 자리를 지우고 기본 원소를 넣는다.
            if (auto removed =
                    PathRemoveElement(Registry(), Access(), Start(), relative, nullptr, &failure);
                !removed) {
                return Failed(removed.Error());
            }

            auto inserted =
                PathInsertElement(Registry(), Access(), Start(), relative, nullptr, &failure);
            return inserted ? Changed() : Failed(inserted.Error());
        }

        if (fieldKind == ValueKind::Optional) {
            auto made = PathEmplaceOptional(Registry(), Access(), Start(), relative, &failure);
            return made ? Changed() : Failed(made.Error());
        }

        if (auto reset = ResetStruct(); !reset || state.failed) {
            return reset;
        }

        return Changed();
    case TextValueShape::List:
    case TextValueShape::Entries:
    case TextValueShape::Empty:
        if (fieldKind != ValueKind::Array && fieldKind != ValueKind::StaticArray &&
            fieldKind != ValueKind::Map && fieldKind != ValueKind::Set) {
            return Diagnostic(TextDiagnosticCode::InvalidValue, "value does not fit the property");
        }

        if (auto replaced = ReplaceContents(value); !replaced || state.failed) {
            return replaced;
        }

        return Changed();
    }

    return Diagnostic(TextDiagnosticCode::InvalidValue, "value does not fit the property");
}

Result<void> Apply::ResetStruct() noexcept {
    // 단위 자신이면 상자를 기본값으로, 안쪽 struct면 그 자리를 따로 떼어 기본값으로 바꿔 되돌린다.
    // 값 단위만 struct 자리를 가진다(컨테이너 단위의 값은 참조다).
    if (!box) {
        return Diagnostic(TextDiagnosticCode::InvalidValue, "value does not fit the property");
    }

    if (relative.steps.Size() == 1) {
        box->ResetToDefault();
        return Result<void>::Success();
    }

    return ResetAt(relative);
}

Result<void> Apply::ResetAt(PropertyPath at) noexcept {
    DetachedValue inner(box->GetAllocator());
    if (auto detached = PathDetach(Registry(), Access(), Start(), at, inner, &failure); !detached) {
        return Failed(detached.Error());
    }

    inner.ResetToDefault();
    PreparedAttach attach;
    if (auto located =
            PathPrepareAttach(Registry(), Access(), Start(), at, inner, attach, &failure);
        !located) {
        return Failed(located.Error());
    }

    // 옛 값은 inner가 받아 이 함수를 나갈 때 반환한다.
    return attach.Publish();
}

Result<void> Apply::ResetMembers() noexcept {
    // 정적 배열의 struct 원소는 떼어 낼 수 없으므로(선택자가 있는 끝) 멤버마다 기본값으로 되돌린다.
    const StructView view = slot.structView;
    const usize count = relative.steps.Size();
    if (!box || view.IsEmpty() || count >= MaxPropertyPathSteps) {
        return Diagnostic(TextDiagnosticCode::Unsupported, "this element cannot be reset");
    }

    PathStep steps[MaxPropertyPathSteps];
    for (usize index = 0; index < count; ++index) {
        steps[index] = relative.steps[index];
    }

    FreshFailure fresh(state);
    const auto& members = view.Schema().fields;
    for (usize index = 0; index < members.Size(); ++index) {
        steps[count] = PathStep{};
        steps[count].fieldId = members[index].id;
        if (auto reset =
                ResetAt(PropertyPath{Span<const PathStep>(steps, count + 1), relative.keys});
            !reset || state.failed) {
            return reset;
        }
    }

    return Result<void>::Success();
}

Result<void> Apply::ReplaceContents(const TextValue& value) noexcept {
    const ValueKind fieldKind = slot.field->value.kind;
    PathStep steps[MaxPropertyPathSteps];
    PathKey keys[MaxPropertyPathSteps];
    const usize stepCount = relative.steps.Size();
    const usize keyCount = relative.keys.Size();
    for (usize index = 0; index < stepCount; ++index) {
        steps[index] = relative.steps[index];
    }

    for (usize index = 0; index < keyCount; ++index) {
        keys[index] = relative.keys[index];
    }

    PathStep& last = steps[stepCount - 1];
    const PropertyPath element{Span<const PathStep>(steps, stepCount),
                               Span<const PathKey>(keys, keyCount)};
    const PropertyPath keyed{Span<const PathStep>(steps, stepCount),
                             Span<const PathKey>(keys, keyCount + 1)};
    if (fieldKind == ValueKind::StaticArray) {
        // 길이가 고정이라 원소마다 쓴다(원소 수는 ParseTextValue가 N과 대조했다).
        last.selector = PathSelector::Index;
        for (usize index = 0; index < value.elements.Size(); ++index) {
            last.index = static_cast<uint32>(index);
            if (auto written = PathWrite(Registry(), Access(), Start(), element,
                                         value.elements[index], nullptr, &failure);
                !written) {
                return Failed(written.Error());
            }
        }

        return Result<void>::Success();
    }

    if (auto cleared = PathClear(Registry(), Access(), Start(), relative, &failure); !cleared) {
        return Failed(cleared.Error());
    }

    if (fieldKind == ValueKind::Array) {
        last.selector = PathSelector::Index;
        for (usize index = 0; index < value.elements.Size(); ++index) {
            last.index = static_cast<uint32>(index);
            if (auto inserted = PathInsertElement(Registry(), Access(), Start(), element,
                                                  &value.elements[index], &failure);
                !inserted) {
                return Failed(inserted.Error());
            }
        }

        return Result<void>::Success();
    }

    // Set 원소·Map 항목은 키를 경로 key로 만들어 넣는다.
    Array<uint8> wire(state.allocator);
    last.selector = PathSelector::Key;
    last.index = static_cast<uint32>(keyCount);
    for (usize index = 0; index < value.elements.Size(); ++index) {
        wire.Clear();
        if (auto made = AppendKeyWire(state, value.elements[index], wire); !made) {
            return made;
        }

        keys[keyCount] = PathKey{value.elements[index].kind, wire.View()};
        Result<void> done = Result<void>::Success();
        if (fieldKind == ValueKind::Set) {
            auto inserted = PathInsertKey(Registry(), Access(), Start(), keyed, &failure);
            done = inserted ? Result<void>::Success() : Result<void>::Failure(inserted.Error());
        } else {
            done = PathWrite(Registry(), Access(), Start(), keyed, value.values[index], nullptr,
                             &failure);
        }

        if (!done) {
            return Failed(done.Error());
        }
    }

    return Result<void>::Success();
}

enum class UnitKind : uint8 {
    None,
    Value,
    Reference,
    Container
};

struct UnitEnd {
    usize index = IndexNone;
    UnitKind kind = UnitKind::None;
};

// 경로의 준비 단위다: 위에서부터 처음 만나는 참조 없는 필드·멤버(값 단위), 단일 강한 참조
// 위치(ObjectRef 필드·멤버, 정적 배열의 ObjectRef 원소 — 참조 단위), ObjectRefArray·ObjectRefMap(그
// 원소·항목 포함 — 컨테이너 단위). 참조를 담은 인라인 struct와 정적 배열·StructRefArray의 struct
// 원소로는 내려간다. 그 밖(StructRefArray 통째·원소 통째·참조 struct 통째)은 None이다.
Result<UnitEnd> FindUnitEnd(AssignmentState& state, PropertyPath path) noexcept {
    for (usize index = 0; index < path.steps.Size(); ++index) {
        TextSlot slot;
        const PropertyPath prefix{Span<const PathStep>(path.steps.Data(), index + 1), path.keys};
        if (auto described = DescribeTextPath(state.lease, prefix, slot); !described) {
            return Result<UnitEnd>::Failure(described.Error());
        }

        const FieldSchema& field = *slot.field;
        const ValueKind target = FieldTargetType(field).kind;
        const bool copyable =
            target != ValueKind::ObjectReference &&
            (slot.structView.IsEmpty() || !slot.structView.ContainsStrongReference());
        if (copyable) {
            return Result<UnitEnd>::Success(UnitEnd{index, UnitKind::Value});
        }

        const PathSelector selector = path.steps[index].selector;
        const bool last = index + 1 == path.steps.Size();
        // ObjectRefArray·ObjectRefMap(통째·원소·항목)는 컨테이너 단위다(리플렉션 B9i3c3).
        const bool registryContainer =
            target == ValueKind::ObjectReference &&
            (field.value.kind == ValueKind::Array || field.value.kind == ValueKind::Map);
        if (registryContainer) {
            return Result<UnitEnd>::Success(last ? UnitEnd{index, UnitKind::Container} : UnitEnd{});
        }

        const bool single =
            (field.value.kind == ValueKind::ObjectReference && selector == PathSelector::None) ||
            (field.value.kind == ValueKind::StaticArray && selector == PathSelector::Index);
        if (last && target == ValueKind::ObjectReference && single) {
            return Result<UnitEnd>::Success(UnitEnd{index, UnitKind::Reference});
        }

        const bool element =
            selector == PathSelector::Index && field.element.kind == ValueKind::Struct &&
            (field.value.kind == ValueKind::StaticArray || field.value.kind == ValueKind::Array);
        const bool member = selector == PathSelector::None && field.value.kind == ValueKind::Struct;
        if (last || !(element || member)) {
            break;
        }
    }

    return Result<UnitEnd>::Success(UnitEnd{});
}

// 단일 강한 참조 위치 path를 target으로 바꾸는 간선 변경 단위를 둔다(리플렉션 B9i3c2·c3). 같은
// 위치의 단위가 이미 있으면(참조 struct `{}`가 만든 null 단위 등) 그 새 대상을 바꾼다. 지금 대상(그
// 단위의 새 대상, 없으면 객체의 값)과 같으면 무동작이다(record면 override를 넣는다 — ReadOnly
// 자리는 넣지 않는다). 다르면 ReadOnly 자리는 진단, 아니면 pinValue의 대상을 고정하고 자리와 대상을
// 검증(PathPrepareReference — ReadOnly·기대 타입·대상 생존)한다.
Result<void> AssignReference(AssignmentState& state, PropertyPath path, ObjectHandle target,
                             bool readOnly, bool record, const TextValue* pinValue,
                             TextSource source, StringView pathText) noexcept {
    ObjectRegistry& registry = state.target.Registry();
    const ObjectAccessContext& access = state.target.Access();
    const PathTarget root = state.Root();
    ReferenceUnit* existing = nullptr;
    for (usize index = 0; index < state.references.Size() && !existing; ++index) {
        if (SamePropertyPath(state.references[index]->Path(), path)) {
            existing = state.references[index].Get();
        }
    }

    PropertyPathFailure failure;
    ObjectHandle current = existing ? existing->target : ObjectHandle{};
    if (!existing) {
        FieldValue read(state.allocator);
        if (auto found = PathRead(registry, access, root, path, read, &failure); !found) {
            return ReportPathFailure(state, found.Error(), failure, source, pathText);
        }

        current = read.reference;
    }

    if (current == target) {
        return readOnly || !record ? Result<void>::Success() : AddOverride(state, path);
    }

    if (readOnly) {
        return Diagnose(state, TextDiagnosticCode::ReadOnly, source, pathText,
                        "property is read-only");
    }

    if (pinValue) {
        auto pinned = PinReferences(state, *pinValue, source, pathText);
        if (!pinned || !pinned.Value()) {
            return pinned ? Result<void>::Success() : Result<void>::Failure(pinned.Error());
        }
    }

    FieldValue value(state.allocator);
    value.kind = ValueKind::ObjectReference;
    value.reference = target;
    ReferenceUnit* unit = existing;
    UniquePtr<ReferenceUnit> made;
    if (!unit) {
        auto created = UniquePtr<ReferenceUnit>::TryMake(state.allocator, state.allocator);
        if (!created) {
            return Result<void>::Failure(created.Error());
        }

        made = Move(created.Value());
        unit = made.Get();
        if (auto copied = unit->steps.TryAppend(path.steps); !copied) {
            return copied;
        }

        if (auto reserved = state.references.TryReserve(state.references.Size() + 1); !reserved) {
            return reserved;
        }
    }

    // 앞 단위가 있으면 자리는 그대로이고 새 대상만 다시 검증한다(찾은 자리는 버린다).
    PreparedReference scratch;
    PreparedReference& located = made ? unit->prepared : scratch;
    if (auto checked = PathPrepareReference(registry, access, root, path, value, located, &failure);
        !checked) {
        return ReportPathFailure(state, checked.Error(), failure, source, pathText);
    }

    unit->target = target;
    if (made) {
        unit->previous = unit->prepared.Previous();
        (void)state.references.TryPushBack(Move(made));
    }

    return record ? AddOverride(state, path) : Result<void>::Success();
}

// 단일 강한 참조 위치의 대입이다(리플렉션 B9i3c2): Set만 받고 단일 ObjectReference 값을 간선 변경
// 단위로 둔다(AssignReference).
Result<void> ApplyReference(AssignmentState& state, const TextAssignment& assignment,
                            const TextSlot& slot, PropertyPath path, TextSource source) noexcept {
    if (assignment.kind != AssignmentKind::Set) {
        return Diagnose(state, TextDiagnosticCode::InvalidValue, source, assignment.path,
                        "a reference cannot be unset or erased; assign none");
    }

    TextValue value(state.allocator);
    auto parsed =
        ParseTextValue(state.context, slot, assignment.value, source, assignment.path, value);
    if (!parsed) {
        return Result<void>::Failure(parsed.Error());
    }

    if (!parsed.Value()) {
        state.failed = true;
        return Result<void>::Success();
    }

    if (value.shape != TextValueShape::Single || value.single.kind != ValueKind::ObjectReference) {
        return Diagnose(state, TextDiagnosticCode::InvalidValue, source, assignment.path,
                        "value does not fit the property");
    }

    const bool readOnly = state.target.GetKind() != AssignmentTarget::Kind::Staged &&
                          (slot.flags & static_cast<uint32>(FieldFlags::ReadOnly)) != 0;
    return AssignReference(state, path, value.single.reference, readOnly, true, &value, source,
                           assignment.path);
}

// 참조를 담은 struct 자리 path(인라인 struct 필드·멤버, 정적 배열·StructRefArray의 struct 원소 —
// view는 그 struct)를 기본값으로 되돌린다(리플렉션 B9i3c3 — 참조 struct `{}`). 통째로 떼어 낼 수
// 없으므로 멤버마다 기존 준비 단위로 나눈다: 값 멤버는 값 단위의 ResetToDefault, ObjectRef 멤버와
// ObjectRef 정적 배열 원소는 null 참조 단위, ObjectRefArray·ObjectRefMap 멤버는 컨테이너 단위의
// 비우기, 참조 struct 멤버와 그 정적 배열 원소는 다시 나눈다. StructRefArray 멤버는 Unsupported다.
// ReadOnly 멤버(staged 대상 제외)는 값이 이미 기본값인 참조가 아니면 진단 ReadOnly다.
Result<void> ResetReferenceStruct(AssignmentState& state, PropertyPath path, StructView view,
                                  TextSource source, StringView pathText) noexcept {
    const usize count = path.steps.Size();
    if (view.IsEmpty() || count >= MaxPropertyPathSteps) {
        return Diagnose(state, TextDiagnosticCode::Unsupported, source, pathText,
                        "this struct cannot be reset");
    }

    ObjectRegistry& registry = state.target.Registry();
    const ObjectAccessContext& access = state.target.Access();
    const bool staged = state.target.GetKind() == AssignmentTarget::Kind::Staged;
    PathStep steps[MaxPropertyPathSteps];
    for (usize index = 0; index < count; ++index) {
        steps[index] = path.steps[index];
    }

    FreshFailure fresh(state);
    const auto& members = view.Schema().fields;
    for (usize member = 0; member < members.Size() && !state.failed; ++member) {
        const FieldSchema& field = members[member];
        steps[count] = PathStep{};
        steps[count].fieldId = field.id;
        const PropertyPath child{Span<const PathStep>(steps, count + 1), path.keys};
        TextSlot slot;
        if (auto described = DescribeTextPath(state.lease, child, slot); !described) {
            return described;
        }

        const bool readOnly =
            !staged && (slot.flags & static_cast<uint32>(FieldFlags::ReadOnly)) != 0;
        const ValueKind target = FieldTargetType(field).kind;
        const bool strongStruct =
            !slot.structView.IsEmpty() && slot.structView.ContainsStrongReference();
        Result<void> done = Result<void>::Success();
        if (field.value.kind == ValueKind::StaticArray &&
            (target == ValueKind::ObjectReference || strongStruct)) {
            // 원소마다 null 참조 단위이거나 다시 나눈다.
            steps[count].selector = PathSelector::Index;
            for (uint32 element = 0; element < field.maxElements && done && !state.failed;
                 ++element) {
                steps[count].index = element;
                done = target == ValueKind::ObjectReference
                           ? AssignReference(state, child, ObjectHandle{}, readOnly, false, nullptr,
                                             source, pathText)
                           : ResetReferenceStruct(state, child, slot.structView, source, pathText);
            }
        } else if (target == ValueKind::ObjectReference &&
                   (field.value.kind == ValueKind::Array || field.value.kind == ValueKind::Map)) {
            auto container = AcquireContainer(state, child, source, pathText);
            if (!container || !container.Value()) {
                return container ? Result<void>::Success()
                                 : Result<void>::Failure(container.Error());
            }

            PropertyPathFailure failure;
            if (auto cleared = PathClear(registry, access,
                                         state.Root().WithReferences(container.Value()->stage),
                                         child, &failure);
                !cleared) {
                done = ReportPathFailure(state, cleared.Error(), failure, source, pathText);
            } else {
                container.Value()->dirty = true;
            }
        } else if (target == ValueKind::ObjectReference) {
            done = AssignReference(state, child, ObjectHandle{}, readOnly, false, nullptr, source,
                                   pathText);
        } else if (strongStruct && field.value.kind == ValueKind::Struct) {
            done = ResetReferenceStruct(state, child, slot.structView, source, pathText);
        } else if (strongStruct) {
            done = Diagnose(state, TextDiagnosticCode::Unsupported, source, pathText,
                            "a StructRefArray member cannot be reset yet");
        } else if (readOnly) {
            done = Diagnose(state, TextDiagnosticCode::ReadOnly, source, pathText,
                            "property is read-only");
        } else {
            auto unit = AcquireUnit(state, child, source, pathText);
            if (!unit || !unit.Value()) {
                return unit ? Result<void>::Success() : Result<void>::Failure(unit.Error());
            }

            unit.Value()->box.ResetToDefault();
            unit.Value()->dirty = true;
        }

        if (!done) {
            return done;
        }
    }

    return Result<void>::Success();
}

// 준비 단위가 없는 자리의 대입이다: 참조를 담은 struct 자리(인라인 struct 필드·멤버, 정적 배열·
// StructRefArray의 struct 원소)의 `{}`만 받아 멤버마다 나누고(ResetReferenceStruct) override는 그
// struct 경로로 넣는다. 그 밖(StructRefArray 통째·원소 지우기)은 Unsupported다.
Result<void> ApplyReferenceStruct(AssignmentState& state, const TextAssignment& assignment,
                                  const TextSlot& slot, PropertyPath path,
                                  TextSource source) noexcept {
    const bool structSlot =
        !slot.structView.IsEmpty() &&
        ((slot.part == TextSlotPart::Field && slot.field->value.kind == ValueKind::Struct) ||
         slot.part == TextSlotPart::Element);
    if (assignment.kind != AssignmentKind::Set || !structSlot) {
        return Diagnose(state, TextDiagnosticCode::Unsupported, source, assignment.path,
                        "this assignment to a reference-holding value is not supported yet");
    }

    TextValue value(state.allocator);
    auto parsed =
        ParseTextValue(state.context, slot, assignment.value, source, assignment.path, value);
    if (!parsed) {
        return Result<void>::Failure(parsed.Error());
    }

    if (!parsed.Value()) {
        state.failed = true;
        return Result<void>::Success();
    }

    if (value.shape != TextValueShape::StructDefault) {
        return Diagnose(state, TextDiagnosticCode::InvalidValue, source, assignment.path,
                        "value does not fit the property");
    }

    FreshFailure fresh(state);
    if (auto reset = ResetReferenceStruct(state, path, slot.structView, source, assignment.path);
        !reset || state.failed) {
        return reset;
    }

    return AddOverride(state, path);
}

} // namespace

Result<void> ProcessAssignment(AssignmentState& state, const TextAssignment& assignment) noexcept {
    const TextSource source = TextSource::Assignment(assignment.sourceIndex);
    TextPath resolved;
    auto made = ResolveTextPath(state.context, state.lease, assignment.path, source, resolved);
    if (!made) {
        return Result<void>::Failure(made.Error());
    }

    if (!made.Value()) {
        state.failed = true;
        return Result<void>::Success();
    }

    const PropertyPath path = resolved.path.View();
    PathKey keys[MaxPropertyPathSteps];
    for (usize index = 0; index < state.assigned.Count(); ++index) {
        if (SamePropertyPath(state.assigned.View(index, keys), path)) {
            return Diagnose(state, TextDiagnosticCode::DuplicateAssignment, source, assignment.path,
                            "the same path is assigned twice");
        }
    }

    if (auto kept = state.assigned.Append(path); !kept) {
        return kept;
    }

    auto unitEnd = FindUnitEnd(state, path);
    if (!unitEnd) {
        return Result<void>::Failure(unitEnd.Error());
    }

    if (unitEnd.Value().kind == UnitKind::None) {
        return ApplyReferenceStruct(state, assignment, resolved.slot, path, source);
    }

    if (unitEnd.Value().kind == UnitKind::Reference) {
        return ApplyReference(state, assignment, resolved.slot, path, source);
    }

    // 단위 경로는 끝 단계의 선택자를 뗀 필드·멤버 하나다(앞 단계는 인라인 struct와 정적 배열·
    // StructRefArray 원소라 key가 없다).
    const usize end = unitEnd.Value().index;
    PathStep unitSteps[MaxPropertyPathSteps];
    for (usize index = 0; index <= end; ++index) {
        unitSteps[index] = path.steps[index];
    }

    unitSteps[end].selector = PathSelector::None;
    unitSteps[end].index = 0;
    const PropertyPath unitPath{Span<const PathStep>(unitSteps, end + 1), {}};
    if (unitEnd.Value().kind == UnitKind::Container) {
        // 컨테이너 단위는 대상 기준 경로 그대로 준비 사본에 적용한다.
        auto container = AcquireContainer(state, unitPath, source, assignment.path);
        if (!container || !container.Value()) {
            return container ? Result<void>::Success() : Result<void>::Failure(container.Error());
        }

        ContainerUnit& unit = *container.Value();
        Apply apply{state,      state.Root().WithReferences(unit.stage),
                    unit.dirty, nullptr,
                    assignment, resolved.slot,
                    path,       path,
                    source,     {}};
        return apply.Run();
    }

    auto unit = AcquireUnit(state, unitPath, source, assignment.path);
    if (!unit || !unit.Value()) {
        return unit ? Result<void>::Success() : Result<void>::Failure(unit.Error());
    }

    Apply apply{state,
                PathTarget::Value(unit.Value()->box),
                unit.Value()->dirty,
                &unit.Value()->box,
                assignment,
                resolved.slot,
                path,
                PropertyPath{path.steps.Mid(end), path.keys},
                source,
                {}};
    return apply.Run();
}

} // namespace rocks::reflection_text::detail
