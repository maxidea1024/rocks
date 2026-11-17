#include "TextAssignApplyInternal.h"
#include "TextJsonInternal.h"

// JSON import다(리플렉션 B9i4 — ReflectionText/TextJson.h의 ImportJson). JSON 객체를 스키마와 함께
// 따라가며 struct는 멤버마다 내려가고(병합), 그 밖의 자리는 대입 하나(미리 읽은 값 — 통째 교체,
// Optional null은 Unset)로 대입 묶음에 넣는다(ProcessResolvedAssignment). struct 원소 배열·Map 값
// struct는 비우기 대입 뒤 원소·항목마다 `{}` 대입과 멤버 병합으로 펼친다(명령행 `a=`·`a[i]={}`·
// `m[k]={}`와 같다).
namespace rocks::reflection_text {

namespace {

using detail::FieldSlot;
using detail::JsonPathBuilder;
using detail::PartSlot;
using detail::SchemaScope;

constexpr ValueKind NullableKinds[] = {ValueKind::ObjectReference, ValueKind::WeakObjectReference,
                                       ValueKind::SoftObjectReference, ValueKind::ClassReference};

bool IsNullableKind(ValueKind kind) noexcept {
    for (const ValueKind nullable : NullableKinds) {
        if (kind == nullable) {
            return true;
        }
    }

    return false;
}

bool IsNumberKind(ValueKind kind) noexcept {
    switch (kind) {
    case ValueKind::UInt8:
    case ValueKind::UInt16:
    case ValueKind::UInt32:
    case ValueKind::UInt64:
    case ValueKind::Int8:
    case ValueKind::Int16:
    case ValueKind::Int32:
    case ValueKind::Int64:
    case ValueKind::Float32:
    case ValueKind::Float64:
        return true;
    default:
        return false;
    }
}

// JSON import 한 번의 상태다.
struct Importer {
    detail::AssignmentState& state;
    JsonPathBuilder path;
    uint32 depth = 0;

    TextCodecContext& Context() noexcept {
        return state.context;
    }

    bool Stopped() const noexcept {
        return state.context.Diagnostics().Full();
    }

    // 진단이 쌓인 수다(저장한 것과 넘친 것).
    uint64 DiagnosticCount() const noexcept {
        return state.context.Diagnostics().Count() + state.context.Diagnostics().Omitted();
    }

    Result<void> PathText(String& out) noexcept;
    Result<void> Report(TextDiagnosticCode code, JsonValue at, StringView message) noexcept;
    Result<void> Enter() noexcept;
    Result<void> Members(const SchemaScope& scope, uint32 flags, JsonValue object) noexcept;
    Result<void> Field(const SchemaScope& scope, usize index, uint32 flags,
                       JsonValue value) noexcept;
    Result<void> Whole(const TextSlot& slot, JsonValue value) noexcept;
    Result<void> StructAt(const TextSlot& slot, JsonValue value) noexcept;
    Result<void> ScalarAt(const TextSlot& slot, JsonValue value) noexcept;
    Result<void> Elements(const TextSlot& slot, JsonValue value) noexcept;
    Result<void> Entries(const TextSlot& slot, JsonValue value) noexcept;
    Result<bool> Scalar(const TextScalarType& type, JsonValue value, StringView pathText,
                        FieldValue& out) noexcept;
    Result<bool> FlagNames(const TextScalarType& type, JsonValue value, StringView pathText,
                           FieldValue& out) noexcept;
    Result<void> Assign(AssignmentKind kind, const TextSlot& slot, JsonValue at,
                        TextValue* value) noexcept;
};

// 지금 경로의 텍스트다(진단 경로). 쓸 수 없는 key(resolver가 쓰지 못한 Soft 등)면 빈 텍스트다.
Result<void> Importer::PathText(String& out) noexcept {
    out.Clear();
    if (path.Depth() == 0) {
        return Result<void>::Success();
    }

    auto formatted = FormatPropertyPath(state.lease, path.View(), out, &Context().KeyHook());
    if (!formatted) {
        if (formatted.Error() == ErrorCode::OutOfMemory) {
            return formatted;
        }

        out.Clear();
    }

    return Result<void>::Success();
}

Result<void> Importer::Report(TextDiagnosticCode code, JsonValue at, StringView message) noexcept {
    String text(state.allocator);
    if (auto made = PathText(text); !made) {
        return made;
    }

    return detail::Diagnose(state, code, TextSource::Json(at.Position()), text.View(), message);
}

// JSON 컨테이너 하나에 들어간다(나올 때 depth를 줄인다). 오류: maxJsonDepth를 넘으면
// CapacityExceeded.
Result<void> Importer::Enter() noexcept {
    if (depth >= Context().Options().maxJsonDepth) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    ++depth;
    return Result<void>::Success();
}

Result<void> Importer::Members(const SchemaScope& scope, uint32 flags, JsonValue object) noexcept {
    if (!object.IsObject()) {
        return Report(TextDiagnosticCode::InvalidValue, object, "expected an object");
    }

    if (auto entered = Enter(); !entered) {
        return entered;
    }

    Result<void> result = Result<void>::Success();
    const auto& fields = scope.Schema().fields;
    Array<bool> seen(state.allocator);
    const bool fits = object.Size() <= Context().Options().maxElements;
    if (!fits) {
        result = Report(TextDiagnosticCode::OutOfRange, object, "too many members");
    } else {
        result = seen.TryResize(fields.Size());
    }

    for (usize member = 0; result && fits && member < object.Size() && !Stopped(); ++member) {
        const StringView key = object.KeyAt(member);
        const JsonValue value = object.ValueAt(member);
        if (auto charged = Context().ChargeInput(key.ByteCount()); !charged) {
            result = Move(charged);
            break;
        }

        // 객체 키는 필드 도구 이름이고 대소문자를 무시한다(경로 규칙과 같다).
        usize found = IndexNone;
        for (usize index = 0; index < fields.Size(); ++index) {
            if (EqualsIgnoringAsciiCase(EffectiveToolName(fields[index]),
                                        Span<const char>(key.Data(), key.ByteCount()))) {
                found = index;
                break;
            }
        }

        if (found == IndexNone) {
            String text(state.allocator);
            result = PathText(text);
            if (result && path.Depth() != 0) {
                result = text.TryAppend(".");
            }

            if (result) {
                result = text.TryAppend(key);
            }

            if (result) {
                result = detail::Diagnose(state, TextDiagnosticCode::UnknownProperty,
                                          TextSource::Json(value.Position()), text.View(),
                                          "unknown property");
            }

            continue;
        }

        if (seen[found]) {
            if (path.Push(fields[found].id)) {
                result = Report(TextDiagnosticCode::DuplicateKey, value,
                                "the same property is given twice (keys ignore case)");
                path.Pop();
            }

            continue;
        }

        seen[found] = true;
        result = Field(scope, found, flags, value);
    }

    --depth;
    return result;
}

Result<void> Importer::Field(const SchemaScope& scope, usize index, uint32 flags,
                             JsonValue value) noexcept {
    const TextSlot slot = FieldSlot(scope, index, flags);
    if (!path.Push(slot.field->id)) {
        return Report(TextDiagnosticCode::Unsupported, value, "property path is too deep");
    }

    Result<void> result = Result<void>::Success();
    if (ClassifyTextInput(Context().Options().use, slot.flags) == TextFieldInput::Excluded) {
        result = Report(TextDiagnosticCode::ExcludedProperty, value,
                        "property is excluded from input for this use");
    } else if (detail::HasNoJsonForm(*slot.field)) {
        result = Report(TextDiagnosticCode::Unsupported, value, "property has no JSON form");
    } else {
        result = Whole(slot, value);
    }

    path.Pop();
    return result;
}

Result<void> Importer::Whole(const TextSlot& slot, JsonValue value) noexcept {
    const FieldSchema& field = *slot.field;
    switch (field.value.kind) {
    case ValueKind::Struct:
        return Members(SchemaScope{nullptr, slot.structView}, slot.flags, value);
    case ValueKind::Optional:
        if (value.IsNull()) {
            return Assign(AssignmentKind::Unset, slot, value, nullptr);
        }

        return field.element.kind == ValueKind::Struct ? StructAt(slot, value)
                                                       : ScalarAt(slot, value);
    case ValueKind::Array:
    case ValueKind::StaticArray:
    case ValueKind::Set:
        return Elements(slot, value);
    case ValueKind::Map:
        return Entries(slot, value);
    default:
        return ScalarAt(slot, value);
    }
}

// struct 자리(struct 원소·Map 값 struct·Optional<struct>)를 기본값으로 만든 뒤(`{}` 대입) 멤버를
// 병합한다. `{}` 대입이 진단을 남기면 멤버는 보지 않는다.
Result<void> Importer::StructAt(const TextSlot& slot, JsonValue value) noexcept {
    if (!value.IsObject()) {
        return Report(TextDiagnosticCode::InvalidValue, value, "expected an object");
    }

    TextValue reset(state.allocator);
    reset.shape = TextValueShape::StructDefault;
    const uint64 before = DiagnosticCount();
    if (auto assigned = Assign(AssignmentKind::Set, slot, value, &reset); !assigned) {
        return assigned;
    }

    if (DiagnosticCount() != before) {
        return Result<void>::Success();
    }

    return Members(SchemaScope{nullptr, slot.structView}, slot.flags, value);
}

Result<void> Importer::ScalarAt(const TextSlot& slot, JsonValue value) noexcept {
    String text(state.allocator);
    if (auto made = PathText(text); !made) {
        return made;
    }

    TextValue single(state.allocator);
    auto parsed = Scalar(SlotValueType(slot), value, text.View(), single.single);
    if (!parsed || !parsed.Value()) {
        state.failed = state.failed || (parsed && !parsed.Value());
        return parsed ? Result<void>::Success() : Result<void>::Failure(parsed.Error());
    }

    single.shape = TextValueShape::Single;
    return Assign(AssignmentKind::Set, slot, value, &single);
}

Result<void> Importer::Elements(const TextSlot& slot, JsonValue value) noexcept {
    const FieldSchema& field = *slot.field;
    if (!value.IsArray()) {
        return Report(TextDiagnosticCode::InvalidValue, value, "expected an array");
    }

    String text(state.allocator);
    if (auto made = PathText(text); !made) {
        return made;
    }

    if (auto entered = Enter(); !entered) {
        return entered;
    }

    Result<void> result = Result<void>::Success();
    const TextSource source = TextSource::Json(value.Position());
    auto fits = detail::CheckElementCount(Context(), field, value.Size(), source, text.View());
    if (!fits || !fits.Value()) {
        state.failed = state.failed || (fits && !fits.Value());
        --depth;
        return fits ? Result<void>::Success() : Result<void>::Failure(fits.Error());
    }

    // ObjectRef 정적 배열은 통째 값 단위가 없어(원소마다 참조 단위 — 리플렉션 B9i3c2) 원소마다
    // `a[i]=v`로 쓴다.
    if (field.value.kind == ValueKind::StaticArray &&
        field.element.kind == ValueKind::ObjectReference) {
        const TextSlot element = PartSlot(slot, TextSlotPart::Element);
        for (usize index = 0; result && index < value.Size() && !Stopped(); ++index) {
            path.SetIndex(static_cast<uint32>(index));
            result = ScalarAt(element, value.At(index));
            path.ClearSelector();
        }

        --depth;
        return result;
    }

    if (field.element.kind == ValueKind::Struct) {
        const TextSlot element = PartSlot(slot, TextSlotPart::Element);
        if (field.value.kind == ValueKind::Array) {
            // 원소를 담은 StructRefArray는 원소 추가가 아직 없다(대입 묶음은 지우기·비우기만 —
            // 리플렉션 B9i3c4).
            if (slot.structView.ContainsStrongReference() && value.Size() != 0) {
                result = Report(TextDiagnosticCode::Unsupported, value,
                                "elements of a reference struct array cannot be added yet");
                --depth;
                return result;
            }

            TextValue cleared(state.allocator);
            cleared.shape = TextValueShape::List;
            result = Assign(AssignmentKind::Set, slot, value, &cleared);
        }

        for (usize index = 0; result && index < value.Size() && !Stopped(); ++index) {
            path.SetIndex(static_cast<uint32>(index));
            result = StructAt(element, value.At(index));
            path.ClearSelector();
        }

        --depth;
        return result;
    }

    const bool set = field.value.kind == ValueKind::Set;
    const TextScalarType type = set ? SlotKeyType(slot) : SlotValueType(slot);
    TextValue list(state.allocator);
    detail::KeySet keys(state.allocator);
    bool ok = true;
    result = list.elements.TryReserve(value.Size());
    for (usize index = 0; result && index < value.Size() && !Stopped(); ++index) {
        const JsonValue item = value.At(index);
        FieldValue element(state.allocator);
        auto parsed = Scalar(type, item, text.View(), element);
        if (!parsed) {
            result = Result<void>::Failure(parsed.Error());
            break;
        }

        if (!parsed.Value()) {
            ok = false;
            continue;
        }

        if (set) {
            auto added = keys.Add(Context(), type, element);
            if (!added) {
                result = Result<void>::Failure(added.Error());
                break;
            }

            if (!added.Value()) {
                ok = false;
                result = detail::Diagnose(state, TextDiagnosticCode::DuplicateKey,
                                          TextSource::Json(item.Position()), text.View(),
                                          "duplicate set element");
                continue;
            }
        }

        result = list.elements.TryPushBack(Move(element));
    }

    --depth;
    if (!result) {
        return result;
    }

    if (!ok || list.elements.Size() != value.Size()) {
        state.failed = true;
        return Result<void>::Success();
    }

    list.shape = TextValueShape::List;
    return Assign(AssignmentKind::Set, slot, value, &list);
}

Result<void> Importer::Entries(const TextSlot& slot, JsonValue value) noexcept {
    const FieldSchema& field = *slot.field;
    if (!value.IsObject()) {
        return Report(TextDiagnosticCode::InvalidValue, value, "expected an object");
    }

    String text(state.allocator);
    if (auto made = PathText(text); !made) {
        return made;
    }

    if (auto entered = Enter(); !entered) {
        return entered;
    }

    Result<void> result = Result<void>::Success();
    auto fits = detail::CheckElementCount(Context(), field, value.Size(),
                                          TextSource::Json(value.Position()), text.View());
    if (!fits || !fits.Value()) {
        state.failed = state.failed || (fits && !fits.Value());
        --depth;
        return fits ? Result<void>::Success() : Result<void>::Failure(fits.Error());
    }

    const TextScalarType keyType = SlotKeyType(slot);
    const TextScalarType valueType = SlotValueType(slot);
    const TextSlot entrySlot = PartSlot(slot, TextSlotPart::MapValue);
    const bool structValue = field.element.kind == ValueKind::Struct;
    const bool merge = Context().Options().mapMerge == MapMergeMode::MergeByKey;
    // 통째 교체: struct 값 Map은 비운 뒤 항목마다 펼치고, 단일 값 Map은 항목 목록 대입 하나다.
    TextValue entries(state.allocator);
    if (structValue && !merge) {
        entries.shape = TextValueShape::Entries;
        result = Assign(AssignmentKind::Set, slot, value, &entries);
    }

    detail::KeySet keys(state.allocator);
    bool ok = true;
    for (usize index = 0; result && index < value.Size() && !Stopped(); ++index) {
        const JsonValue item = value.ValueAt(index);
        const TextSource source = TextSource::Json(item.Position());
        FieldValue key(state.allocator);
        auto parsed =
            ParseTextScalar(Context(), keyType, value.KeyAt(index), source, text.View(), key);
        if (!parsed) {
            result = Result<void>::Failure(parsed.Error());
            break;
        }

        if (!parsed.Value()) {
            ok = false;
            continue;
        }

        auto added = keys.Add(Context(), keyType, key);
        if (!added) {
            result = Result<void>::Failure(added.Error());
            break;
        }

        if (!added.Value()) {
            ok = false;
            result = detail::Diagnose(state, TextDiagnosticCode::DuplicateKey, source, text.View(),
                                      "duplicate map key");
            continue;
        }

        if (structValue || merge) {
            result = path.SetKey(state.options.names, state.allocator, key);
            if (!result) {
                break;
            }

            if (!structValue) {
                result = ScalarAt(entrySlot, item);
            } else if (merge) {
                // 있던 항목은 멤버 병합, 없던 항목은 기본값 뒤 멤버 병합이다.
                auto present = PathContains(state.target.Registry(), state.target.Access(),
                                            state.Root(), path.View());
                if (!present) {
                    result = Result<void>::Failure(present.Error());
                } else {
                    result = present.Value()
                                 ? Members(SchemaScope{nullptr, slot.structView}, slot.flags, item)
                                 : StructAt(entrySlot, item);
                }
            } else {
                result = StructAt(entrySlot, item);
            }

            path.ClearSelector();
            continue;
        }

        FieldValue entry(state.allocator);
        auto read = Scalar(valueType, item, text.View(), entry);
        if (!read) {
            result = Result<void>::Failure(read.Error());
            break;
        }

        if (!read.Value()) {
            ok = false;
            continue;
        }

        result = entries.elements.TryPushBack(Move(key));
        if (result) {
            result = entries.values.TryPushBack(Move(entry));
        }
    }

    --depth;
    if (!result) {
        return result;
    }

    if (!ok) {
        state.failed = true;
        return Result<void>::Success();
    }

    if (structValue || merge || Stopped()) {
        return Result<void>::Success();
    }

    entries.shape = TextValueShape::Entries;
    return Assign(AssignmentKind::Set, slot, value, &entries);
}

// JSON 값 하나를 type의 단일 값으로 읽는다(10.4절 JSON 열). false면 진단을 남겼다.
Result<bool> Importer::Scalar(const TextScalarType& type, JsonValue value, StringView pathText,
                              FieldValue& out) noexcept {
    const TextSource source = TextSource::Json(value.Position());
    const ValueKind kind = type.type.kind;
    StringView message;
    if (kind == ValueKind::Bool) {
        if (value.IsBool()) {
            out.kind = ValueKind::Bool;
            out.boolean = value.GetBool();
            out.present = true;
            return Result<bool>::Success(true);
        }

        message = "expected true or false";
    } else if (IsNumberKind(kind)) {
        const bool real = kind == ValueKind::Float32 || kind == ValueKind::Float64;
        if (value.IsNumber()) {
            return ParseTextScalar(Context(), type, value.RawNumber(), source, pathText, out);
        }

        if (real && value.IsString() && detail::IsNonFiniteText(value.GetString())) {
            return ParseTextScalar(Context(), type, value.GetString(), source, pathText, out);
        }

        message = real ? "expected a number (or \"nan\", \"inf\", \"-inf\")" : "expected a number";
    } else if (kind == ValueKind::Enum && type.enumSchema &&
               HasFlag(type.enumSchema->flags, EnumFlags::Flags)) {
        if (value.IsArray()) {
            return FlagNames(type, value, pathText, out);
        }

        message = "expected an array of enumerator names";
    } else if (IsNullableKind(kind)) {
        if (value.IsNull()) {
            return ParseTextScalar(Context(), type, "none", source, pathText, out);
        }

        if (value.IsString()) {
            return ParseTextScalar(Context(), type, value.GetString(), source, pathText, out);
        }

        message = "expected a string or null";
    } else {
        if (value.IsString()) {
            return ParseTextScalar(Context(), type, value.GetString(), source, pathText, out);
        }

        message = kind == ValueKind::Enum ? "expected an enumerator name" : "expected a string";
    }

    return detail::Report(Context(), TextDiagnosticCode::InvalidValue, source, pathText, message);
}

// Flags enum의 이름 배열을 읽는다(명령행 표기 `A+B`로 이어 같은 해석을 쓴다 — 빈 배열은 0).
Result<bool> Importer::FlagNames(const TextScalarType& type, JsonValue value, StringView pathText,
                                 FieldValue& out) noexcept {
    const TextSource source = TextSource::Json(value.Position());
    if (value.Size() > Context().Options().maxElements) {
        return detail::Report(Context(), TextDiagnosticCode::OutOfRange, source, pathText,
                              "too many elements");
    }

    if (auto entered = Enter(); !entered) {
        return Result<bool>::Failure(entered.Error());
    }

    StringBuilder<128> joined(state.allocator);
    Result<bool> result = Result<bool>::Success(true);
    for (usize index = 0; index < value.Size(); ++index) {
        const JsonValue name = value.At(index);
        const StringView text = name.IsString() ? name.GetString() : StringView();
        bool valid = !text.IsEmpty();
        for (usize at = 0; valid && at < text.ByteCount(); ++at) {
            valid = text.Data()[at] != '+' && text.Data()[at] != '|';
        }

        if (!valid) {
            result = detail::Report(Context(), TextDiagnosticCode::InvalidValue,
                                    TextSource::Json(name.Position()), pathText,
                                    "expected an enumerator name");
            break;
        }

        Result<void> appended = index == 0 ? Result<void>::Success() : joined.TryAppend("+");
        if (appended) {
            appended = joined.TryAppend(text);
        }

        if (!appended) {
            result = Result<bool>::Failure(appended.Error());
            break;
        }
    }

    --depth;
    if (!result || !result.Value()) {
        return result;
    }

    return ParseTextScalar(Context(), type, joined.View(), source, pathText, out);
}

Result<void> Importer::Assign(AssignmentKind kind, const TextSlot& slot, JsonValue at,
                              TextValue* value) noexcept {
    if (Stopped()) {
        return Result<void>::Success();
    }

    String text(state.allocator);
    if (auto made = PathText(text); !made) {
        return made;
    }

    const TextAssignment assignment{kind, text.View(), StringView(), 0};
    return detail::ProcessResolvedAssignment(state, assignment, TextSource::Json(at.Position()),
                                             path.View(), slot, value);
}

} // namespace

Result<PreparedAssignments> ImportJson(JsonValue root, const AssignmentTarget& target,
                                       const TextCodecOptions& options,
                                       TextDiagnostics& diagnostics) noexcept {
    using ImportResult = Result<PreparedAssignments>;
    if (!root.IsValid()) {
        return ImportResult::Failure(ErrorCode::InvalidArgument);
    }

    // 입력을 받지 않는 용도(Report)는 맨 앞에서 거부한다.
    if (auto excluded = TextInputExcludedFlags(options.use); !excluded) {
        return ImportResult::Failure(excluded.Error());
    }

    auto begun = detail::BeginAssignments(target, options, diagnostics);
    if (!begun) {
        return ImportResult::Failure(begun.Error());
    }

    detail::AssignmentState& state = *begun.Value();
    Importer importer{state, JsonPathBuilder(state.allocator)};
    const TypeLease& type = state.lease;
    const SchemaScope scope = type.Kind() == TypeKind::Struct ? SchemaScope{nullptr, type.Struct()}
                                                              : SchemaScope{&type, StructView{}};
    if (auto read = importer.Members(scope, 0, root); !read) {
        return ImportResult::Failure(read.Error());
    }

    if (state.failed) {
        return ImportResult::Failure(ErrorCode::InvalidArgument);
    }

    return ImportResult::Success(detail::AssignmentAccess::Wrap(Move(begun.Value())));
}

} // namespace rocks::reflection_text
