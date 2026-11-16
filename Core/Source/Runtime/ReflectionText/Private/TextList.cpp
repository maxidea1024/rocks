#include "TextInternal.h"

#include <Core/Containers/OrderedSet.h>
#include <Core/Guid.h>
#include <Core/Text/StringSplit.h>
#include <Object/ValueText.h>

// 자리 하나의 값 텍스트(단일 값·`{}`·쉼표 목록·Map `k:v` 목록)다(리플렉션 B9i3b — Docs/Design/
// ReflectionExtensions.ko.md 10.4절). 목록은 escape되지 않은 `,`로 나누고(Core StringSplit의 escape
// 규칙 — TextListEscapeCharacters), Map 항목은 첫 escape되지 않은 `:`에서 나눈 뒤 키·값의 escape를
// 푼다.
namespace rocks::reflection_text {

namespace {

constexpr StringView StructDefaultText = "{}";

// 목록 안 키 중복 판정이다. Name은 pool 비교 규칙(Name ==), 그 밖은 값의 정규 텍스트(String은 byte
// 그대로)로 본다.
class KeySet {
public:
    explicit KeySet(Allocator& allocator) noexcept : names_(allocator), texts_(allocator) {
    }

    // key를 더한다. 이미 있으면 false다.
    Result<bool> Add(TextCodecContext& context, const TextScalarType& type,
                     const FieldValue& key) noexcept {
        if (key.kind == ValueKind::Name) {
            auto added = names_.Add(key.name);
            if (!added) {
                return Result<bool>::Failure(added.Error());
            }

            return Result<bool>::Success(added.Value() == InsertOutcome::Inserted);
        }

        StringBuilder<128> text(context.GetAllocator());
        Result<void> made = Result<void>::Success();
        switch (key.kind) {
        case ValueKind::Utf8String:
            made = text.TryAppend(key.string.View());
            break;
        case ValueKind::SoftObjectReference:
            made = text.TryAppendFormat("{}\n{}", key.guid, key.string.View());
            break;
        case ValueKind::ClassReference:
            made = text.TryAppendFormat("{}", key.guid);
            break;
        default:
            made = FormatTextScalar(context, type, key, text);
            break;
        }

        if (!made) {
            return Result<bool>::Failure(made.Error());
        }

        String stored(context.GetAllocator());
        if (auto assigned = stored.TryAssign(text.View()); !assigned) {
            return Result<bool>::Failure(assigned.Error());
        }

        auto added = texts_.Add(Move(stored));
        if (!added) {
            return Result<bool>::Failure(added.Error());
        }

        return Result<bool>::Success(added.Value() == InsertOutcome::Inserted);
    }

private:
    OrderedSet<Name> names_;
    OrderedSet<String> texts_;
};

bool IsStructDefault(StringView text) noexcept {
    return text == StructDefaultText;
}

// 목록 텍스트의 원소 수다(빈 텍스트는 0 — escape되지 않은 `,` 수 + 1).
Result<usize> CountListItems(StringView text) noexcept {
    if (text.IsEmpty()) {
        return Result<usize>::Success(0);
    }

    usize count = 1;
    usize start = 0;
    while (true) {
        const StringView rest(text.Data() + start, text.ByteCount() - start);
        auto comma = TryFindUnescaped(rest, ",", TextListEscapeCharacters);
        if (!comma) {
            return Result<usize>::Failure(comma.Error());
        }

        if (comma.Value() == StringView::NotFound) {
            return Result<usize>::Success(count);
        }

        ++count;
        start += comma.Value() + 1;
    }
}

// 목록 텍스트의 index번째부터 다음 원소(escape를 풀지 않은 조각)를 piece에 두고 start를 다음 원소로
// 옮긴다.
Result<void> NextListItem(StringView text, usize& start, StringView& piece) noexcept {
    const StringView rest(text.Data() + start, text.ByteCount() - start);
    auto comma = TryFindUnescaped(rest, ",", TextListEscapeCharacters);
    if (!comma) {
        return Result<void>::Failure(comma.Error());
    }

    const usize length = comma.Value() == StringView::NotFound ? rest.ByteCount() : comma.Value();
    piece = StringView(rest.Data(), length);
    start += length + 1;
    return Result<void>::Success();
}

// 원소 조각의 escape를 풀어 읽는다(진단은 원소마다 남긴다).
Result<bool> ParseItem(TextCodecContext& context, const TextScalarType& type, StringView raw,
                       TextSource source, StringView pathText, FieldValue& out) noexcept {
    StringBuilder<128> text(context.GetAllocator());
    auto unescaped = TryAppendUnescaped(text, raw, TextListEscapeCharacters);
    if (!unescaped) {
        if (unescaped.Error() == ErrorCode::InvalidArgument) {
            return detail::Report(context, TextDiagnosticCode::InvalidValue, source, pathText,
                                  "not valid UTF-8");
        }

        return Result<bool>::Failure(unescaped.Error());
    }

    detail::ScalarFailure failure;
    auto parsed = detail::ParseScalarCore(context, type, text.View(), out, failure);
    if (!parsed || parsed.Value()) {
        return parsed;
    }

    return detail::Report(context, failure.code, source, pathText, failure.message);
}

// 원소 수 한도를 본다(options.maxElements → 필드 상한 → StaticArray는 정확히 N).
Result<bool> CheckCount(TextCodecContext& context, const FieldSchema& field, usize count,
                        TextSource source, StringView pathText) noexcept {
    if (count > context.Options().maxElements) {
        return detail::Report(context, TextDiagnosticCode::OutOfRange, source, pathText,
                              "too many elements");
    }

    if (field.value.kind == ValueKind::StaticArray) {
        if (count != field.maxElements) {
            return detail::Report(context, TextDiagnosticCode::InvalidValue, source, pathText,
                                  "fixed-size array needs exactly its element count");
        }
    } else if (count > field.maxElements) {
        return detail::Report(context, TextDiagnosticCode::OutOfRange, source, pathText,
                              "more elements than the field allows");
    }

    return Result<bool>::Success(true);
}

// Array·StaticArray·Set 목록을 읽는다.
Result<bool> ParseList(TextCodecContext& context, const TextSlot& slot, StringView text,
                       TextSource source, StringView pathText, TextValue& out) noexcept {
    auto count = CountListItems(text);
    if (!count) {
        return Result<bool>::Failure(count.Error());
    }

    auto fits = CheckCount(context, *slot.field, count.Value(), source, pathText);
    if (!fits || !fits.Value()) {
        return fits;
    }

    if (auto reserved = out.elements.TryReserve(count.Value()); !reserved) {
        return Result<bool>::Failure(reserved.Error());
    }

    const bool set = slot.field->value.kind == ValueKind::Set;
    const TextScalarType type = set ? SlotKeyType(slot) : SlotValueType(slot);
    KeySet keys(context.GetAllocator());
    bool ok = true;
    usize start = 0;
    for (usize index = 0; index < count.Value() && !context.Diagnostics().Full(); ++index) {
        StringView raw;
        if (auto next = NextListItem(text, start, raw); !next) {
            return Result<bool>::Failure(next.Error());
        }

        FieldValue element(context.GetAllocator());
        auto parsed = ParseItem(context, type, raw, source, pathText, element);
        if (!parsed) {
            return parsed;
        }

        if (!parsed.Value()) {
            ok = false;
            continue;
        }

        if (set) {
            auto added = keys.Add(context, type, element);
            if (!added) {
                return added;
            }

            if (!added.Value()) {
                ok = false;
                if (auto reported = detail::Report(context, TextDiagnosticCode::DuplicateKey,
                                                   source, pathText, "duplicate set element");
                    !reported) {
                    return reported;
                }

                continue;
            }
        }

        if (auto pushed = out.elements.TryPushBack(Move(element)); !pushed) {
            return Result<bool>::Failure(pushed.Error());
        }
    }

    // 진단 모음이 차서 멈췄으면 남은 원소를 읽지 않았다(실패로 본다).
    if (!ok || out.elements.Size() != count.Value()) {
        out.Reset();
        return Result<bool>::Success(false);
    }

    out.shape = TextValueShape::List;
    return Result<bool>::Success(true);
}

// Map `k:v` 목록을 읽는다.
Result<bool> ParseEntries(TextCodecContext& context, const TextSlot& slot, StringView text,
                          TextSource source, StringView pathText, TextValue& out) noexcept {
    auto count = CountListItems(text);
    if (!count) {
        return Result<bool>::Failure(count.Error());
    }

    auto fits = CheckCount(context, *slot.field, count.Value(), source, pathText);
    if (!fits || !fits.Value()) {
        return fits;
    }

    if (auto reserved = out.elements.TryReserve(count.Value()); !reserved) {
        return Result<bool>::Failure(reserved.Error());
    }

    if (auto reserved = out.values.TryReserve(count.Value()); !reserved) {
        return Result<bool>::Failure(reserved.Error());
    }

    const TextScalarType keyType = SlotKeyType(slot);
    const TextScalarType valueType = SlotValueType(slot);
    KeySet keys(context.GetAllocator());
    bool ok = true;
    usize start = 0;
    for (usize index = 0; index < count.Value() && !context.Diagnostics().Full(); ++index) {
        StringView raw;
        if (auto next = NextListItem(text, start, raw); !next) {
            return Result<bool>::Failure(next.Error());
        }

        auto colon = TryFindUnescaped(raw, ":", TextListEscapeCharacters);
        if (!colon) {
            return Result<bool>::Failure(colon.Error());
        }

        if (colon.Value() == StringView::NotFound) {
            ok = false;
            if (auto reported = detail::Report(context, TextDiagnosticCode::InvalidValue, source,
                                               pathText, "map entry needs key:value");
                !reported) {
                return reported;
            }

            continue;
        }

        FieldValue key(context.GetAllocator());
        FieldValue value(context.GetAllocator());
        const StringView keyText(raw.Data(), colon.Value());
        const StringView valueText(raw.Data() + colon.Value() + 1,
                                   raw.ByteCount() - colon.Value() - 1);
        auto keyParsed = ParseItem(context, keyType, keyText, source, pathText, key);
        if (!keyParsed) {
            return keyParsed;
        }

        auto valueParsed = ParseItem(context, valueType, valueText, source, pathText, value);
        if (!valueParsed) {
            return valueParsed;
        }

        if (!keyParsed.Value() || !valueParsed.Value()) {
            ok = false;
            continue;
        }

        auto added = keys.Add(context, keyType, key);
        if (!added) {
            return added;
        }

        if (!added.Value()) {
            ok = false;
            if (auto reported = detail::Report(context, TextDiagnosticCode::DuplicateKey, source,
                                               pathText, "duplicate map key");
                !reported) {
                return reported;
            }

            continue;
        }

        if (auto pushed = out.elements.TryPushBack(Move(key)); !pushed) {
            return Result<bool>::Failure(pushed.Error());
        }

        if (auto pushed = out.values.TryPushBack(Move(value)); !pushed) {
            return Result<bool>::Failure(pushed.Error());
        }
    }

    // 진단 모음이 차서 멈췄으면 남은 원소를 읽지 않았다(실패로 본다).
    if (!ok || out.elements.Size() != count.Value()) {
        out.Reset();
        return Result<bool>::Success(false);
    }

    out.shape = TextValueShape::Entries;
    return Result<bool>::Success(true);
}

Result<bool> ParseSingle(TextCodecContext& context, const TextScalarType& type, StringView text,
                         TextSource source, StringView pathText, TextValue& out) noexcept {
    detail::ScalarFailure failure;
    auto parsed = detail::ParseScalarCore(context, type, text, out.single, failure);
    if (!parsed) {
        return parsed;
    }

    if (!parsed.Value()) {
        out.Reset();
        return detail::Report(context, failure.code, source, pathText, failure.message);
    }

    out.shape = TextValueShape::Single;
    return Result<bool>::Success(true);
}

Result<bool> ParseStructDefault(TextCodecContext& context, StringView text, TextSource source,
                                StringView pathText, TextValue& out) noexcept {
    if (!IsStructDefault(text)) {
        return detail::Report(context, TextDiagnosticCode::InvalidValue, source, pathText,
                              "struct values take {} (set members by path)");
    }

    out.shape = TextValueShape::StructDefault;
    return Result<bool>::Success(true);
}

// 목록 원소 하나를 escape해 붙인다. key면 `:`도 escape하고, 아니면 원래 `\` 바로 뒤의 `:`만
// escape한다(그 `\`가 escape 쌍을 이루지 않게). 다음 구분자(원소 뒤 `,`·키 뒤 `:`)와 escape 쌍이
// 되는
// `\`로 끝나면 쓸 수 없다(Unsupported).
Result<void> AppendItem(StringView text, bool key, bool last, String& out) noexcept {
    const char* data = text.Data();
    const usize size = text.ByteCount();
    if (size != 0 && data[size - 1] == '\\' && (key || !last)) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    usize start = 0;
    for (usize index = 0; index < size; ++index) {
        const char c = data[index];
        const bool escape =
            c == ',' || c == ';' || (c == ':' && (key || (index != 0 && data[index - 1] == '\\')));
        if (!escape) {
            continue;
        }

        if (auto appended = out.TryAppend(StringView(data + start, index - start)); !appended) {
            return appended;
        }

        const char pair[2] = {'\\', c};
        if (auto appended = out.TryAppend(StringView(pair, 2)); !appended) {
            return appended;
        }

        start = index + 1;
    }

    return out.TryAppend(StringView(data + start, size - start));
}

bool IsStructSlot(const TextSlot& slot) noexcept {
    return SlotValueType(slot).type.kind == ValueKind::Struct;
}

} // namespace

Result<bool> ParseTextValue(TextCodecContext& context, const TextSlot& slot, StringView text,
                            TextSource source, StringView pathText, TextValue& out) noexcept {
    out.Reset();
    if (!slot.field) {
        return Result<bool>::Failure(ErrorCode::InvalidArgument);
    }

    if (auto started = context.Start(); !started) {
        return Result<bool>::Failure(started.Error());
    }

    if (auto charged = context.ChargeInput(text.ByteCount()); !charged) {
        return Result<bool>::Failure(charged.Error());
    }

    const FieldSchema& field = *slot.field;
    switch (slot.part) {
    case TextSlotPart::SetElement:
        return detail::Report(context, TextDiagnosticCode::Unsupported, source, pathText,
                              "a set element has no value");
    case TextSlotPart::Element:
    case TextSlotPart::MapValue:
        if (IsStructSlot(slot)) {
            return ParseStructDefault(context, text, source, pathText, out);
        }

        return ParseSingle(context, SlotValueType(slot), text, source, pathText, out);
    case TextSlotPart::Field:
        break;
    }

    switch (field.value.kind) {
    case ValueKind::Struct:
        return ParseStructDefault(context, text, source, pathText, out);
    case ValueKind::Optional:
        if (field.element.kind == ValueKind::Struct) {
            return ParseStructDefault(context, text, source, pathText, out);
        }

        return ParseSingle(context, SlotValueType(slot), text, source, pathText, out);
    case ValueKind::Array:
    case ValueKind::StaticArray:
    case ValueKind::Set:
        if (field.element.kind == ValueKind::Struct) {
            if (field.value.kind == ValueKind::Array && text.IsEmpty()) {
                out.shape = TextValueShape::List;
                return Result<bool>::Success(true);
            }

            return detail::Report(
                context, TextDiagnosticCode::InvalidValue, source, pathText,
                "struct element arrays take an empty value; set elements by path");
        }

        return ParseList(context, slot, text, source, pathText, out);
    case ValueKind::Map:
        if (field.element.kind == ValueKind::Struct) {
            if (text.IsEmpty()) {
                out.shape = TextValueShape::Entries;
                return Result<bool>::Success(true);
            }

            return detail::Report(context, TextDiagnosticCode::InvalidValue, source, pathText,
                                  "struct value maps take an empty value; set entries by path");
        }

        return ParseEntries(context, slot, text, source, pathText, out);
    default:
        return ParseSingle(context, SlotValueType(slot), text, source, pathText, out);
    }
}

Result<void> FormatTextValue(TextCodecContext& context, const TextSlot& slot,
                             const TextValue& value, String& out) noexcept {
    if (!slot.field) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const ValueKind kind = slot.field->value.kind;
    const bool whole = slot.part == TextSlotPart::Field;
    const bool list = whole && (kind == ValueKind::Array || kind == ValueKind::StaticArray ||
                                kind == ValueKind::Set);
    const bool entries = whole && kind == ValueKind::Map;
    const bool structured = IsStructSlot(slot) && !list && !entries;
    switch (value.shape) {
    case TextValueShape::Single: {
        if (list || entries || structured || slot.part == TextSlotPart::SetElement) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        StringBuilder<128> text(context.GetAllocator());
        if (auto formatted = FormatTextScalar(context, SlotValueType(slot), value.single, text);
            !formatted) {
            return formatted;
        }

        return out.TryAppend(text.View());
    }
    case TextValueShape::StructDefault:
        if (!structured) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        return out.TryAppend(StructDefaultText);
    case TextValueShape::List: {
        if (!list) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        const TextScalarType type =
            kind == ValueKind::Set ? SlotKeyType(slot) : SlotValueType(slot);
        const usize count = value.elements.Size();
        for (usize index = 0; index < count; ++index) {
            StringBuilder<128> text(context.GetAllocator());
            if (auto formatted = FormatTextScalar(context, type, value.elements[index], text);
                !formatted) {
                return formatted;
            }

            // 원소 하나뿐인 빈 문자열은 빈 목록과 같은 텍스트라 쓸 수 없다.
            if (count == 1 && text.IsEmpty()) {
                return Result<void>::Failure(ErrorCode::Unsupported);
            }

            if (index != 0) {
                if (auto comma = out.TryAppend(","); !comma) {
                    return comma;
                }
            }

            if (auto appended = AppendItem(text.View(), false, index + 1 == count, out);
                !appended) {
                return appended;
            }
        }

        return Result<void>::Success();
    }
    case TextValueShape::Entries: {
        if (!entries || value.elements.Size() != value.values.Size()) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        const TextScalarType keyType = SlotKeyType(slot);
        const TextScalarType valueType = SlotValueType(slot);
        const usize count = value.elements.Size();
        for (usize index = 0; index < count; ++index) {
            StringBuilder<128> keyText(context.GetAllocator());
            StringBuilder<128> valueText(context.GetAllocator());
            if (auto formatted = FormatTextScalar(context, keyType, value.elements[index], keyText);
                !formatted) {
                return formatted;
            }

            if (auto formatted =
                    FormatTextScalar(context, valueType, value.values[index], valueText);
                !formatted) {
                return formatted;
            }

            if (index != 0) {
                if (auto comma = out.TryAppend(","); !comma) {
                    return comma;
                }
            }

            if (auto appended = AppendItem(keyText.View(), true, false, out); !appended) {
                return appended;
            }

            if (auto colon = out.TryAppend(":"); !colon) {
                return colon;
            }

            if (auto appended = AppendItem(valueText.View(), false, index + 1 == count, out);
                !appended) {
                return appended;
            }
        }

        return Result<void>::Success();
    }
    case TextValueShape::Empty:
        break;
    }

    return Result<void>::Failure(ErrorCode::InvalidArgument);
}

} // namespace rocks::reflection_text
