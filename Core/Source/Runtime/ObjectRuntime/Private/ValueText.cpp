#include <Core/Text/NumberText.h>
#include <Object/AssetId.h>
#include <Object/ValueText.h>

// 단일 값 텍스트 표기다(리플렉션 B9i3b — Docs/Design/ReflectionExtensions.ko.md 1.5·10.4절). 문자열
// 경로 key와 ReflectionText 값이 함께 쓴다(Object/ValueText.h).
namespace rocks {

namespace {

bool IsDigit(char c) noexcept {
    return c >= '0' && c <= '9';
}

// 정수 텍스트가 정규 10진인지다: 숫자 하나 이상, 앞자리 0은 `0` 하나만, 부호는 부호 있는 종류의 `-`
// 만이다(`+`·`-0` 거부 — 표기가 하나가 되게, 출력 FormatDecimalText와 같다).
bool IsCanonicalDecimal(StringView text, bool signedKind) noexcept {
    const char* data = text.Data();
    usize size = text.ByteCount();
    usize start = 0;
    if (signedKind && size != 0 && data[0] == '-') {
        start = 1;
    }

    if (start == size) {
        return false;
    }

    for (usize index = start; index < size; ++index) {
        if (!IsDigit(data[index])) {
            return false;
        }
    }

    if (data[start] == '0') {
        return size - start == 1 && start == 0;
    }

    return true;
}

// Flags enum 텍스트(`A+B`·`A|B`, 빈 값은 0)를 값으로 읽는다. 빈 이름 조각·모르는 이름이면 false다.
bool ParseFlagsText(const EnumSchema& schema, StringView text, int64& value) noexcept {
    value = 0;
    const char* data = text.Data();
    const usize size = text.ByteCount();
    if (size == 0) {
        return true;
    }

    usize start = 0;
    for (usize position = 0; position <= size; ++position) {
        if (position != size && data[position] != '+' && data[position] != '|') {
            continue;
        }

        if (position == start) {
            return false;
        }

        const EnumeratorSchema* found =
            FindEnumerator(schema, Span<const char>(data + start, position - start));
        if (!found) {
            return false;
        }

        value |= found->value;
        start = position + 1;
    }

    return true;
}

constexpr char HexDigits[] = "0123456789abcdef";

} // namespace

namespace detail {

Span<const char> FormatDecimalText(uint64 value, bool negative, char* out,
                                   usize capacity) noexcept {
    usize count = 0;
    do {
        out[capacity - 1 - count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);

    if (negative) {
        out[capacity - 1 - count++] = '-';
    }

    return Span<const char>(out + capacity - count, count);
}

Span<const char> FormatGuidText(Guid value, char* out) noexcept {
    usize count = 0;
    for (usize nibble = 0; nibble < 32; ++nibble) {
        if (nibble == 8 || nibble == 12 || nibble == 16 || nibble == 20) {
            out[count++] = '-';
        }

        const uint64 word = nibble < 16 ? value.high : value.low;
        const uint32 shift = static_cast<uint32>(60 - 4 * (nibble % 16));
        out[count++] = HexDigits[(word >> shift) & 0xf];
    }

    return Span<const char>(out, count);
}

const EnumeratorSchema* FindEnumeratorByValue(const EnumSchema& schema, int64 value) noexcept {
    for (usize index = 0; index < schema.values.Size(); ++index) {
        if (schema.values[index].value == value) {
            return &schema.values[index];
        }
    }

    return nullptr;
}

} // namespace detail

Result<PathKeyValue> ParseScalarText(const ValueType& type, const EnumSchema* enumSchema,
                                     StringView text) noexcept {
    const auto invalid = []() noexcept {
        return Result<PathKeyValue>::Failure(ErrorCode::InvalidArgument);
    };

    const Span<const char> span(text.Data(), text.ByteCount());
    PathKeyValue value;
    value.kind = type.kind;
    switch (type.kind) {
    case ValueKind::Bool: {
        const auto is = [&](const char* word, usize bytes) noexcept {
            return EqualsIgnoringAsciiCase(span, Span<const char>(word, bytes));
        };

        if (is("true", 4) || is("on", 2)) {
            value.boolean = true;
        } else if (!is("false", 5) && !is("off", 3)) {
            return invalid();
        }

        break;
    }
    case ValueKind::UInt8:
    case ValueKind::UInt16:
    case ValueKind::UInt32:
    case ValueKind::UInt64: {
        if (!IsCanonicalDecimal(text, false)) {
            return invalid();
        }

        auto parsed = TryParseUInt64(text);
        if (!parsed) {
            return Result<PathKeyValue>::Failure(parsed.Error());
        }

        value.unsignedInteger = parsed.Value();
        break;
    }
    case ValueKind::Int8:
    case ValueKind::Int16:
    case ValueKind::Int32:
    case ValueKind::Int64: {
        if (!IsCanonicalDecimal(text, true)) {
            return invalid();
        }

        auto parsed = TryParseInt64(text);
        if (!parsed) {
            return Result<PathKeyValue>::Failure(parsed.Error());
        }

        value.signedInteger = parsed.Value();
        break;
    }
    case ValueKind::Enum: {
        if (!enumSchema) {
            return invalid();
        }

        if (HasFlag(enumSchema->flags, EnumFlags::Flags)) {
            if (!ParseFlagsText(*enumSchema, text, value.signedInteger)) {
                return invalid();
            }

            break;
        }

        const EnumeratorSchema* found = FindEnumerator(*enumSchema, span);
        if (!found) {
            return invalid();
        }

        value.signedInteger = found->value;
        break;
    }
    case ValueKind::Guid:
    case ValueKind::AssetId: {
        // AssetId 표기 규칙(36자 소문자 하나뿐 — AssetId::TryParse)이고 Guid도 같다(10.4절).
        auto parsed = AssetId::TryParse(text);
        if (!parsed) {
            return invalid();
        }

        value.guid = parsed.Value().value;
        break;
    }
    case ValueKind::Digest256: {
        for (usize index = 0; index < text.ByteCount(); ++index) {
            if (text.Data()[index] >= 'A' && text.Data()[index] <= 'F') {
                return invalid();
            }
        }

        auto parsed = Sha256Digest::ParseHex(text);
        if (!parsed) {
            return invalid();
        }

        value.digest = parsed.Value();
        break;
    }
    case ValueKind::Utf8String:
    case ValueKind::Name:
        value.text = text;
        break;
    default:
        return Result<PathKeyValue>::Failure(ErrorCode::Unsupported);
    }

    return Result<PathKeyValue>::Success(value);
}

Result<void> AppendScalarText(const ValueType& type, const EnumSchema* enumSchema,
                              const PathKeyValue& value, StringBuilderBase& out) noexcept {
    char buffer[Sha256Digest::HexBytes + 1];
    switch (type.kind) {
    case ValueKind::Bool:
        return out.TryAppend(value.boolean ? "true" : "false");
    case ValueKind::UInt8:
    case ValueKind::UInt16:
    case ValueKind::UInt32:
    case ValueKind::UInt64: {
        const Span<const char> digits =
            detail::FormatDecimalText(value.unsignedInteger, false, buffer, sizeof(buffer));
        return out.TryAppend(StringView(digits.Data(), digits.Size()));
    }
    case ValueKind::Int8:
    case ValueKind::Int16:
    case ValueKind::Int32:
    case ValueKind::Int64: {
        // 최솟값의 크기도 uint64로 정확히 나타낸다.
        const bool negative = value.signedInteger < 0;
        const uint64 magnitude = negative ? uint64{0} - static_cast<uint64>(value.signedInteger)
                                          : static_cast<uint64>(value.signedInteger);
        const Span<const char> digits =
            detail::FormatDecimalText(magnitude, negative, buffer, sizeof(buffer));
        return out.TryAppend(StringView(digits.Data(), digits.Size()));
    }
    case ValueKind::Enum: {
        if (!enumSchema) {
            return Result<void>::Failure(ErrorCode::Unsupported);
        }

        if (HasFlag(enumSchema->flags, EnumFlags::Flags)) {
            usize bytes = 0;
            if (!detail::FormatFlagsText<StringBuilderBase>(*enumSchema, value.signedInteger,
                                                            nullptr, bytes)) {
                return Result<void>::Failure(ErrorCode::Unsupported);
            }

            if (!detail::FormatFlagsText(*enumSchema, value.signedInteger, &out, bytes)) {
                return Result<void>::Failure(ErrorCode::OutOfMemory);
            }

            return Result<void>::Success();
        }

        const EnumeratorSchema* found =
            detail::FindEnumeratorByValue(*enumSchema, value.signedInteger);
        if (!found) {
            return Result<void>::Failure(ErrorCode::Unsupported);
        }

        const Span<const char> name = EffectiveToolName(*found);
        return out.TryAppend(StringView(name.Data(), name.Size()));
    }
    case ValueKind::Guid:
    case ValueKind::AssetId: {
        char guid[detail::GuidTextBytes];
        const Span<const char> text = detail::FormatGuidText(value.guid, guid);
        return out.TryAppend(StringView(text.Data(), text.Size()));
    }
    case ValueKind::Digest256:
        value.digest.ToHex(buffer);
        return out.TryAppend(StringView(buffer, Sha256Digest::HexBytes));
    case ValueKind::Utf8String:
    case ValueKind::Name:
        return out.TryAppend(value.text);
    default:
        return Result<void>::Failure(ErrorCode::Unsupported);
    }
}

} // namespace rocks
