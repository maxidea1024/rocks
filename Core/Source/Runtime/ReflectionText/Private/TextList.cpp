#include "TextInternal.h"

#include <Core/Containers/OrderedSet.h>
#include <Core/Guid.h>
#include <Core/Encoding/Base64.h>
#include <Core/Text/StringSplit.h>
#include <Object/FieldValueWire.h>
#include <Object/ValueText.h>

// 자리 하나의 값 텍스트(단일 값·`{}`·쉼표 목록·Map `k:v` 목록)다(리플렉션 B9i3b — Docs/Design/
// ReflectionExtensions.ko.md 10.4절). 목록은 escape되지 않은 `,`로 나누고(Core StringSplit의 escape
// 규칙 — TextListEscapeCharacters), Map 항목은 첫 escape되지 않은 `:`에서 나눈 뒤 키·값의 escape를
// 푼다.
namespace rocks::reflection_text {

namespace {

constexpr StringView StructDefaultText = "{}";

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

// Array·StaticArray·Set 목록을 읽는다.
Result<bool> ParseList(TextCodecContext& context, const TextSlot& slot, StringView text,
                       TextSource source, StringView pathText, TextValue& out) noexcept {
    auto count = CountListItems(text);
    if (!count) {
        return Result<bool>::Failure(count.Error());
    }

    auto fits = detail::CheckElementCount(context, *slot.field, count.Value(), source, pathText);
    if (!fits || !fits.Value()) {
        return fits;
    }

    if (auto reserved = out.elements.TryReserve(count.Value()); !reserved) {
        return Result<bool>::Failure(reserved.Error());
    }

    const bool set = slot.field->value.kind == ValueKind::Set;
    const TextScalarType type = set ? SlotKeyType(slot) : SlotValueType(slot);
    detail::KeySet keys(context.GetAllocator());
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

    auto fits = detail::CheckElementCount(context, *slot.field, count.Value(), source, pathText);
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
    detail::KeySet keys(context.GetAllocator());
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

// InstancedStruct 값 자리(필드·원소·Map 값 — 리플렉션 B9j)인지다.
bool IsInstancedSlot(const TextSlot& slot) noexcept {
    return SlotValueType(slot).type.kind == ValueKind::InstancedStruct;
}

// InstancedStruct 자리의 값 텍스트다(리플렉션 B9j — Docs/Design/ReflectionExtensions.ko.md 10.4절):
// `none`(ASCII 대소문자 무시)은 비우기, 그 밖은 타입 도구 이름(TypeRegistry::AcquireByToolName —
// 대소문자 무시)이어야 하고 struct여야 한다(아니면 InvalidValue 진단). 담기 조건·깊이 합은 형
// 바꾸기 경로 연산이 본다.
Result<bool> ParseInstancedType(TextCodecContext& context, StringView text, TextSource source,
                                StringView pathText, TextValue& out) noexcept {
    out.shape = TextValueShape::InstancedType;
    out.single.kind = ValueKind::InstancedStruct;
    out.single.guid = Guid{};
    if (EqualsIgnoringAsciiCase(Span<const char>(text.Data(), text.ByteCount()),
                                Span<const char>("none", 4))) {
        return Result<bool>::Success(true);
    }

    auto lease = context.Types().AcquireByToolName(Span<const char>(text.Data(), text.ByteCount()));
    if (!lease) {
        if (lease.Error() != ErrorCode::NotFound && lease.Error() != ErrorCode::InvalidArgument) {
            return Result<bool>::Failure(lease.Error());
        }

        out.Reset();
        return detail::Report(context, TextDiagnosticCode::InvalidValue, source, pathText,
                              "unknown struct type tool name");
    }

    if (lease.Value().Kind() != TypeKind::Struct) {
        out.Reset();
        return detail::Report(context, TextDiagnosticCode::InvalidValue, source, pathText,
                              "type is not a reflected struct");
    }

    out.single.guid = lease.Value().Id();
    return Result<bool>::Success(true);
}

} // namespace

namespace detail {

Result<bool> KeySet::Add(TextCodecContext& context, const TextScalarType& type,
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

Result<bool> CheckElementCount(TextCodecContext& context, const FieldSchema& field, usize count,
                               TextSource source, StringView pathText) noexcept {
    if (count > context.Options().maxElements) {
        return Report(context, TextDiagnosticCode::OutOfRange, source, pathText,
                      "too many elements");
    }

    if (field.value.kind == ValueKind::StaticArray) {
        if (count != field.maxElements) {
            return Report(context, TextDiagnosticCode::InvalidValue, source, pathText,
                          "fixed-size array needs exactly its element count");
        }
    } else if (count > field.maxElements) {
        return Report(context, TextDiagnosticCode::OutOfRange, source, pathText,
                      "more elements than the field allows");
    }

    return Result<bool>::Success(true);
}

} // namespace detail

namespace detail {

Result<bool> ParseByteList(TextCodecContext& context, const FieldSchema& field, StringView text,
                           TextSource source, StringView pathText, TextValue& out) noexcept {
    // Base64 4글자가 3 byte다 — 풀기 전에 원소 수 상한을 본다(패딩은 줄일 뿐이다).
    const usize capacity = text.ByteCount() / 4 * 3;
    Array<uint8> bytes(context.GetAllocator());
    if (auto sized = bytes.TryResize(capacity); !sized) {
        return Result<bool>::Failure(sized.Error());
    }

    auto decoded = TryBase64Decode(text, bytes.View());
    if (!decoded) {
        if (decoded.Error() != ErrorCode::InvalidArgument) {
            return Result<bool>::Failure(decoded.Error());
        }

        return Report(context, TextDiagnosticCode::InvalidValue, source, pathText,
                      "expected Base64 bytes (RFC 4648, padded, no whitespace)");
    }

    const usize count = decoded.Value();
    auto fits = CheckElementCount(context, field, count, source, pathText);
    if (!fits || !fits.Value()) {
        return fits;
    }

    if (auto reserved = out.elements.TryReserve(count); !reserved) {
        return Result<bool>::Failure(reserved.Error());
    }

    for (usize index = 0; index < count; ++index) {
        FieldValue element(context.GetAllocator());
        element.kind = ValueKind::UInt8;
        element.unsignedInteger = bytes[index];
        if (auto pushed = out.elements.TryPushBack(Move(element)); !pushed) {
            return Result<bool>::Failure(pushed.Error());
        }
    }

    out.shape = TextValueShape::List;
    return Result<bool>::Success(true);
}

Result<void> AppendByteText(Allocator& allocator, Span<const uint8> bytes, String& out) noexcept {
    auto size = Base64EncodedSize(bytes.Size());
    if (!size) {
        return Result<void>::Failure(size.Error());
    }

    Array<char> text(allocator);
    if (auto sized = text.TryResize(size.Value()); !sized) {
        return sized;
    }

    auto written = TryBase64Encode(bytes, text.View());
    if (!written) {
        return Result<void>::Failure(written.Error());
    }

    return out.TryAppend(StringView(text.Data(), written.Value()));
}

} // namespace detail

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

        if (IsInstancedSlot(slot)) {
            return ParseInstancedType(context, text, source, pathText, out);
        }

        return ParseSingle(context, SlotValueType(slot), text, source, pathText, out);
    case TextSlotPart::Field:
        break;
    }

    switch (field.value.kind) {
    case ValueKind::Struct:
        return ParseStructDefault(context, text, source, pathText, out);
    case ValueKind::InstancedStruct:
        return ParseInstancedType(context, text, source, pathText, out);
    case ValueKind::Optional:
        if (field.element.kind == ValueKind::Struct) {
            return ParseStructDefault(context, text, source, pathText, out);
        }

        return ParseSingle(context, SlotValueType(slot), text, source, pathText, out);
    case ValueKind::Array:
    case ValueKind::StaticArray:
    case ValueKind::Set:
        if (field.element.kind == ValueKind::Struct ||
            field.element.kind == ValueKind::InstancedStruct) {
            if (field.value.kind == ValueKind::Array && text.IsEmpty()) {
                out.shape = TextValueShape::List;
                return Result<bool>::Success(true);
            }

            return detail::Report(
                context, TextDiagnosticCode::InvalidValue, source, pathText,
                "struct element arrays take an empty value; set elements by path");
        }

        // 바이트 배열은 Base64 문자열 하나다(Docs/Design/CurveData.ko.md D18 — 원소는 경로
        // `a[i]=v`로).
        if (::rocks::detail::IsByteArrayField(field)) {
            return detail::ParseByteList(context, field, text, source, pathText, out);
        }

        return ParseList(context, slot, text, source, pathText, out);
    case ValueKind::Map:
        if (field.element.kind == ValueKind::Struct ||
            field.element.kind == ValueKind::InstancedStruct) {
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
    const bool instanced = IsInstancedSlot(slot) && !list && !entries;
    switch (value.shape) {
    case TextValueShape::InstancedType: {
        if (!instanced) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        if (value.single.guid == Guid{}) {
            return out.TryAppend("none");
        }

        auto lease = context.Types().AcquireValueType(value.single.guid, TypeKind::Struct);
        if (!lease) {
            return Result<void>::Failure(lease.Error());
        }

        const Span<const char> name = lease.Value().Schema().toolName;
        if (name.Size() == 0) {
            return Result<void>::Failure(ErrorCode::Unsupported);
        }

        return out.TryAppend(StringView(name.Data(), name.Size()));
    }
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

        if (::rocks::detail::IsByteArrayField(*slot.field)) {
            Array<uint8> bytes(context.GetAllocator());
            if (auto reserved = bytes.TryReserve(value.elements.Size()); !reserved) {
                return reserved;
            }

            for (const FieldValue& element : value.elements) {
                if (element.kind != ValueKind::UInt8 || element.unsignedInteger > 255u) {
                    return Result<void>::Failure(ErrorCode::InvalidArgument);
                }

                if (auto pushed = bytes.TryPushBack(static_cast<uint8>(element.unsignedInteger));
                    !pushed) {
                    return pushed;
                }
            }

            return detail::AppendByteText(context.GetAllocator(), bytes.View(), out);
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
