#include <Core/Guid.h>
#include <Core/Optional.h>
#include <Core/Text/NumberText.h>
#include <Object/AssetId.h>
#include <Object/PropertyPath.h>

// 문자열 경로(ParsePropertyPath·FormatPropertyPath)다. 도구 이름 조회는 대소문자 무시이고 원소
// index는 정규 10진이다(Docs/Design/ReflectionFoundations.ko.md 7.1절). Map·Set의 key는 키 종류의
// 텍스트이고 필요할 때만 큰따옴표로 감싼다. 따옴표 안 escape는 UE ParseQuotedString의 escape 가운데
// `\\`·`\"`·`\n`·`\r`·`\t`다(`\'`·8진은 받지 않는다 — 리플렉션 B9f3, Docs/Design/
// ReflectionExtensions.ko.md 1.5절).
namespace rocks {

namespace {

// 키 하나의 텍스트 표기(따옴표·escape 전)를 담는 지역 버퍼 byte다(String·Name·Flags enum·hook 키는
// 이 버퍼를 쓰지 않는다 — 가장 긴 것은 Digest256 64자).
constexpr usize KeyTextBytes = 72;

bool IsIdentifierStart(char c) noexcept {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

bool IsIdentifierPart(char c) noexcept {
    return IsIdentifierStart(c) || (c >= '0' && c <= '9');
}

bool IsDigit(char c) noexcept {
    return c >= '0' && c <= '9';
}

// ASCII 제어 문자(0x00–0x1F, 0x7F)다.
bool IsControl(char c) noexcept {
    const uint8 byte = static_cast<uint8>(c);
    return byte < 0x20 || byte == 0x7f;
}

// key 텍스트에 따옴표가 필요한 글자다(`]`·`,`·`=`·`"`·`\`·공백·제어 문자 — 1.5절). 따옴표 없는
// key는 이 글자를 담을 수 없다.
bool NeedsKeyQuote(char c) noexcept {
    return c == ']' || c == ',' || c == '=' || c == '"' || c == '\\' || c == ' ' || IsControl(c);
}

// 따옴표 안에서 escape로 쓰는 글자의 escape 문자다(없으면 0): `\`·`"`·개행·CR·탭.
char EscapeOf(char c) noexcept {
    switch (c) {
    case '\\':
        return '\\';
    case '"':
        return '"';
    case '\n':
        return 'n';
    case '\r':
        return 'r';
    case '\t':
        return 't';
    default:
        return 0;
    }
}

// escape 문자(`\` 다음 글자)가 뜻하는 글자다(모르는 escape면 0).
char Unescape(char c) noexcept {
    switch (c) {
    case '\\':
        return '\\';
    case '"':
        return '"';
    case 'n':
        return '\n';
    case 'r':
        return '\r';
    case 't':
        return '\t';
    default:
        return 0;
    }
}

// 경로가 struct 값으로 끝나는 단계인지다(다음 단계 `.`를 받을 수 있다 — index 없는 배열·key 없는
// Map은 아니다). Optional<값 struct> 필드는 그 내부 struct로(리플렉션 B9e — set 여부는 경로 연산이
// 본다), Map<K, 값 struct>의 key 단계는 그 값 struct로 들어간다(리플렉션 B9f3).
bool IsStructStep(const FieldSchema& field, PathSelector selector) noexcept {
    switch (selector) {
    case PathSelector::Index:
        return field.value.kind == ValueKind::Array && field.element.kind == ValueKind::Struct;
    case PathSelector::Key:
        return field.value.kind == ValueKind::Map && field.element.kind == ValueKind::Struct;
    case PathSelector::None:
        return field.value.kind == ValueKind::Struct ||
               (field.value.kind == ValueKind::Optional && field.element.kind == ValueKind::Struct);
    }

    return false;
}

bool IsKeyedField(const FieldSchema& field) noexcept {
    return field.value.kind == ValueKind::Map || field.value.kind == ValueKind::Set;
}

// Map·Set 필드의 키 형식이다(Map은 key, Set은 원소).
const ValueType& KeyTypeOf(const FieldSchema& field) noexcept {
    return field.value.kind == ValueKind::Map ? field.key : field.element;
}

usize DecimalDigits(uint64 value) noexcept {
    usize digits = 1;
    while (value >= 10) {
        value /= 10;
        ++digits;
    }

    return digits;
}

// value의 10진 표기를 out 끝(오른쪽 정렬)에 쓰고 쓴 범위를 돌려준다(out은 21자 이상).
Span<const char> FormatDecimal(uint64 value, bool negative, char* out, usize capacity) noexcept {
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

// 정수 key 텍스트가 정규 10진인지다: 숫자 하나 이상, 앞자리 0은 `0` 하나만, 부호는 부호 있는
// 종류의 `-`만이다(`+`·`-0` 거부 — 표기가 하나가 되게, 출력 FormatDecimal과 같다).
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

constexpr char HexDigits[] = "0123456789abcdef";

// Guid의 36자 소문자 표기(RFC 9562 — Core Guid "{}"·AssetId 표기와 같다, high가 앞 16자리)다.
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

// Flags enum key 텍스트(`A+B`·`A|B`, 빈 값은 0)를 값으로 읽는다. 빈 이름 조각·모르는 이름이면
// false다.
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

// Flags enum 값의 정규 텍스트(선언 순 `A+B`, 0은 빈 값)를 out(없으면 길이만)에 쓰고 byte 수를
// 돌려준다. 이름 있는 열거자 조합으로 나타낼 수 없는 비트가 남으면 false다.
bool FormatFlagsText(const EnumSchema& schema, int64 value, String* out, usize& bytes) noexcept {
    bytes = 0;
    int64 remaining = value;
    bool first = true;
    for (usize index = 0; index < schema.values.Size() && remaining != 0; ++index) {
        const EnumeratorSchema& enumerator = schema.values[index];
        if (enumerator.value == 0 || (enumerator.value & ~value) != 0 ||
            (enumerator.value & remaining) == 0) {
            continue;
        }

        const Span<const char> name = EffectiveToolName(enumerator);
        if (out && ((!first && !out->TryAppend("+")) ||
                    !out->TryAppend(StringView(name.Data(), name.Size())))) {
            return false;
        }

        bytes += name.Size() + (first ? 0 : 1);
        first = false;
        remaining &= ~enumerator.value;
    }

    return remaining == 0;
}

// key 텍스트를 키 종류의 값으로 읽는다(ParsePropertyPath — text는 escape를 푼 원문).
// Soft·ClassRef는 hook이 읽고(없으면 Unsupported), 그 밖의 해석 실패는 InvalidArgument다.
Result<PathKeyValue> ParseKeyText(const ValueType& keyType, const EnumSchema* keyEnum,
                                  StringView text, PathKeyTextHook* hook) noexcept {
    const auto invalid = []() noexcept {
        return Result<PathKeyValue>::Failure(ErrorCode::InvalidArgument);
    };

    const Span<const char> span(text.Data(), text.ByteCount());
    PathKeyValue value;
    value.kind = keyType.kind;
    switch (keyType.kind) {
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
            return invalid();
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
            return invalid();
        }

        value.signedInteger = parsed.Value();
        break;
    }
    case ValueKind::Enum: {
        if (!keyEnum) {
            return invalid();
        }

        if (HasFlag(keyEnum->flags, EnumFlags::Flags)) {
            if (!ParseFlagsText(*keyEnum, text, value.signedInteger)) {
                return invalid();
            }

            break;
        }

        const EnumeratorSchema* found = FindEnumerator(*keyEnum, span);
        if (!found) {
            return invalid();
        }

        value.signedInteger = found->value;
        break;
    }
    case ValueKind::Guid:
    case ValueKind::AssetId: {
        // AssetId 표기 규칙(36자 소문자 하나뿐 — AssetId::TryParse)이고 Guid 키도 같다(10.4절).
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
    case ValueKind::SoftObjectReference:
    case ValueKind::ClassReference: {
        if (!hook) {
            // resolver(논리 경로)·TypeRegistry(타입 도구 이름)가 필요하다 — ReflectionText가 hook을
            // 준다.
            return Result<PathKeyValue>::Failure(ErrorCode::Unsupported);
        }

        auto resolved = hook->ParseKeyText(keyType, text);
        if (!resolved) {
            return resolved;
        }

        if (resolved.Value().kind != keyType.kind) {
            return Result<PathKeyValue>::Failure(ErrorCode::TypeMismatch);
        }

        return resolved;
    }
    default:
        return invalid();
    }

    return Result<PathKeyValue>::Success(value);
}

// key 하나의 형식화 결과다: 따옴표·escape 전 텍스트(text — 지역 버퍼·key wire·schema·hook 문자열을
// 빌린다) 또는 Flags enum 값(flags — 이름 조각을 쓸 때 다시 만든다), 따옴표 여부, 괄호를 뺀 표기
// byte 수.
struct KeyPiece {
    Span<const char> text;
    const EnumSchema* flagsEnum = nullptr;
    int64 flags = 0;
    bool quote = false;
    usize bytes = 0;
};

// text의 따옴표 여부와 escape를 더한 표기 byte 수(괄호 제외)를 piece에 쓴다. 개행·CR·탭이 아닌 제어
// 문자가 있으면 이 문법으로 쓸 수 없다(false).
bool MeasureKeyText(KeyPiece& piece) noexcept {
    const Span<const char> text = piece.text;
    usize escapes = 0;
    piece.quote = false;
    for (usize index = 0; index < text.Size(); ++index) {
        const char c = text[index];
        if (IsControl(c) && EscapeOf(c) == 0) {
            return false;
        }

        piece.quote = piece.quote || NeedsKeyQuote(c);
        escapes += EscapeOf(c) != 0 ? 1 : 0;
    }

    piece.bytes = text.Size() + (piece.quote ? 2 + escapes : 0);
    return true;
}

// key의 형식화 결과를 piece에 만든다(FormatPropertyPath). String·Name은 key wire를 빌리고(Name
// None은 `None`), Flags enum은 이름 조각, Soft·ClassRef는 hook 문자열(hookText — out의 allocator),
// 그 밖은 buffer에 쓴다. hook 없는 Soft·ClassRef·이름 있는 열거자(조합)가 아닌 enum 값·쓸 수 없는
// 제어 문자는 Unsupported, 키 종류가 필드와 다르면 TypeMismatch, 정규형이 아닌 wire는
// InvalidArgument다.
Result<void> FormatKeyText(const ValueType& keyType, const EnumSchema* keyEnum, const PathKey& key,
                           PathKeyTextHook* hook, Allocator& allocator, Optional<String>& hookText,
                           char (&buffer)[KeyTextBytes], KeyPiece& piece) noexcept {
    if (key.kind != keyType.kind) {
        return Result<void>::Failure(ErrorCode::TypeMismatch);
    }

    auto read = ReadPathKey(key);
    if (!read) {
        return Result<void>::Failure(read.Error());
    }

    const PathKeyValue& value = read.Value();
    switch (value.kind) {
    case ValueKind::Bool:
        piece.text = value.boolean ? Span<const char>("true", 4) : Span<const char>("false", 5);
        break;
    case ValueKind::UInt8:
    case ValueKind::UInt16:
    case ValueKind::UInt32:
    case ValueKind::UInt64:
        piece.text = FormatDecimal(value.unsignedInteger, false, buffer, KeyTextBytes);
        break;
    case ValueKind::Int8:
    case ValueKind::Int16:
    case ValueKind::Int32:
    case ValueKind::Int64: {
        // 최솟값의 크기도 uint64로 정확히 나타낸다.
        const bool negative = value.signedInteger < 0;
        const uint64 magnitude = negative ? uint64{0} - static_cast<uint64>(value.signedInteger)
                                          : static_cast<uint64>(value.signedInteger);
        piece.text = FormatDecimal(magnitude, negative, buffer, KeyTextBytes);
        break;
    }
    case ValueKind::Enum: {
        if (!keyEnum) {
            return Result<void>::Failure(ErrorCode::Unsupported);
        }

        if (HasFlag(keyEnum->flags, EnumFlags::Flags)) {
            // 이름 조각은 식별자 글자뿐이라 따옴표가 필요 없다.
            if (!FormatFlagsText(*keyEnum, value.signedInteger, nullptr, piece.bytes)) {
                return Result<void>::Failure(ErrorCode::Unsupported);
            }

            piece.flagsEnum = keyEnum;
            piece.flags = value.signedInteger;
            piece.quote = false;
            return Result<void>::Success();
        }

        const EnumeratorSchema* found = nullptr;
        for (usize index = 0; index < keyEnum->values.Size(); ++index) {
            if (keyEnum->values[index].value == value.signedInteger) {
                found = &keyEnum->values[index];
                break;
            }
        }

        if (!found) {
            return Result<void>::Failure(ErrorCode::Unsupported);
        }

        piece.text = EffectiveToolName(*found);
        break;
    }
    case ValueKind::Guid:
    case ValueKind::AssetId:
        piece.text = FormatGuidText(value.guid, buffer);
        break;
    case ValueKind::Digest256: {
        char hex[Sha256Digest::HexBytes + 1];
        value.digest.ToHex(hex);
        for (usize index = 0; index < Sha256Digest::HexBytes; ++index) {
            buffer[index] = hex[index];
        }

        piece.text = Span<const char>(buffer, Sha256Digest::HexBytes);
        break;
    }
    case ValueKind::Utf8String:
        piece.text = Span<const char>(value.text.Data(), value.text.ByteCount());
        break;
    case ValueKind::Name:
        // None의 wire는 빈 문자열이고 표기는 `None`이다(10.4절 출력).
        piece.text = value.text.ByteCount() == 0
                         ? Span<const char>("None", 4)
                         : Span<const char>(value.text.Data(), value.text.ByteCount());
        break;
    case ValueKind::SoftObjectReference:
    case ValueKind::ClassReference: {
        if (!hook) {
            return Result<void>::Failure(ErrorCode::Unsupported);
        }

        hookText.Emplace(allocator);
        if (auto formatted = hook->FormatKeyText(keyType, value, *hookText); !formatted) {
            return formatted;
        }

        piece.text = Span<const char>(hookText->View().Data(), hookText->View().ByteCount());
        break;
    }
    default:
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    if (!MeasureKeyText(piece)) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    return Result<void>::Success();
}

// piece를 `[...]`로 out에 붙인다(따옴표 안에서 `\`·`"`·개행·CR·탭은 escape).
Result<void> AppendKeyPiece(const KeyPiece& piece, String& out) noexcept {
    const auto failed = []() noexcept { return Result<void>::Failure(ErrorCode::OutOfMemory); };
    if (!out.TryAppend(piece.quote ? "[\"" : "[")) {
        return failed();
    }

    if (piece.flagsEnum) {
        usize bytes = 0;
        if (!FormatFlagsText(*piece.flagsEnum, piece.flags, &out, bytes)) {
            return failed();
        }
    } else {
        const Span<const char> text = piece.text;
        usize start = 0;
        for (usize at = 0; at <= text.Size(); ++at) {
            const char escape = at == text.Size() || !piece.quote ? 0 : EscapeOf(text[at]);
            if (at != text.Size() && escape == 0) {
                continue;
            }

            if (!out.TryAppend(StringView(text.Data() + start, at - start))) {
                return failed();
            }

            if (escape != 0) {
                const char pair[2] = {'\\', escape};
                if (!out.TryAppend(StringView(pair, 2))) {
                    return failed();
                }
            }

            start = at + 1;
        }
    }

    return out.TryAppend(piece.quote ? "\"]" : "]");
}

} // namespace

namespace detail {

// ParsePropertyPath의 본체다(PropertyPathBuffer의 key 칸을 채우는 유일한 곳 — friend).
struct PropertyPathParser {
    static Result<void> Parse(const TypeLease& type, StringView text, PropertyPathBuffer& out,
                              PropertyPathFailure* failure, PathKeyTextHook* hook) noexcept;

    // [position — `[` 다음]부터 key 텍스트를 parsed의 여백 뒤 byte에 풀고 키 값으로 읽어 정규형
    // wire로 담는다. 성공하면 position은 `]` 다음이다.
    static Result<void> ParseKey(const FieldSchema& field, const EnumSchema* keyEnum,
                                 const char* data, usize size, usize& position,
                                 PropertyPathBuffer& parsed, PathStep& step, PathKeyTextHook* hook,
                                 PropertyPathError& reason, usize& offset) noexcept;
};

Result<void> PropertyPathParser::ParseKey(const FieldSchema& field, const EnumSchema* keyEnum,
                                          const char* data, usize size, usize& position,
                                          PropertyPathBuffer& parsed, PathStep& step,
                                          PathKeyTextHook* hook, PropertyPathError& reason,
                                          usize& offset) noexcept {
    const usize keyStart = position - 1;
    const auto fail = [&](ErrorCode code, PropertyPathError why, usize at) noexcept {
        reason = why;
        offset = at;
        return Result<void>::Failure(code);
    };

    // 텍스트는 이 key의 wire 자리 뒤 여백(PathKeyTextPrefixBytes) 너머에 푼다. wire는 그 자리에서
    // 앞으로 쓰므로(정규형 prefix ≤ 여백) 따로 텍스트 버퍼가 없다.
    if (MaxPropertyPathKeyBytes - parsed.keyByteCount_ < PathKeyTextPrefixBytes) {
        return fail(ErrorCode::InvalidArgument, PropertyPathError::InvalidKey, keyStart);
    }

    char* scratch =
        reinterpret_cast<char*>(parsed.keyBytes_ + parsed.keyByteCount_ + PathKeyTextPrefixBytes);
    const usize capacity = MaxPropertyPathKeyBytes - parsed.keyByteCount_ - PathKeyTextPrefixBytes;
    usize length = 0;
    const auto append = [&](char c) noexcept {
        if (length == capacity) {
            return false;
        }

        scratch[length++] = c;
        return true;
    };

    if (position < size && data[position] == '"') {
        ++position;
        bool closed = false;
        while (position < size) {
            const char c = data[position];
            if (c == '"') {
                ++position;
                closed = true;
                break;
            }

            if (IsControl(c)) {
                return fail(ErrorCode::InvalidArgument, PropertyPathError::Syntax, position);
            }

            char literal = c;
            if (c == '\\') {
                literal = position + 1 < size ? Unescape(data[position + 1]) : 0;
                if (literal == 0) {
                    return fail(ErrorCode::InvalidArgument, PropertyPathError::Syntax, position);
                }

                ++position;
            }

            if (!append(literal)) {
                return fail(ErrorCode::InvalidArgument, PropertyPathError::InvalidKey, keyStart);
            }

            ++position;
        }

        if (!closed || position >= size || data[position] != ']') {
            return fail(ErrorCode::InvalidArgument, PropertyPathError::Syntax, position);
        }
    } else {
        while (position < size && data[position] != ']') {
            if (NeedsKeyQuote(data[position])) {
                return fail(ErrorCode::InvalidArgument, PropertyPathError::Syntax, position);
            }

            if (!append(data[position])) {
                return fail(ErrorCode::InvalidArgument, PropertyPathError::InvalidKey, keyStart);
            }

            ++position;
        }

        if (position >= size) {
            return fail(ErrorCode::InvalidArgument, PropertyPathError::Syntax, position);
        }
    }

    ++position;
    auto value = ParseKeyText(KeyTypeOf(field), keyEnum, StringView(scratch, length), hook);
    if (!value) {
        return fail(value.Error(), PropertyPathError::InvalidKey, keyStart);
    }

    // 정규형 wire를 이 key 자리에 쓴다(String·Name 텍스트는 여백 뒤에서 앞으로 옮겨진다 — 앞으로의
    // byte 복사라 겹쳐도 안전하다).
    auto written = detail::WritePathKeyWire(
        value.Value(), Span<uint8>(parsed.keyBytes_ + parsed.keyByteCount_,
                                   MaxPropertyPathKeyBytes - parsed.keyByteCount_));
    if (!written) {
        return fail(ErrorCode::InvalidArgument, PropertyPathError::InvalidKey, keyStart);
    }

    parsed.keys_[parsed.keyCount_] =
        PathKey{value.Value().kind,
                Span<const uint8>(parsed.keyBytes_ + parsed.keyByteCount_, written.Value())};
    parsed.keyByteCount_ += written.Value();
    step.index = static_cast<uint32>(parsed.keyCount_++);
    step.selector = PathSelector::Key;
    return Result<void>::Success();
}

Result<void> PropertyPathParser::Parse(const TypeLease& type, StringView text,
                                       PropertyPathBuffer& out, PropertyPathFailure* failure,
                                       PathKeyTextHook* hook) noexcept {
    const auto fail = [failure](ErrorCode code, PropertyPathError reason, usize step,
                                usize offset) noexcept {
        if (failure) {
            *failure = {reason, step, offset};
        }

        return Result<void>::Failure(code);
    };

    if (failure) {
        *failure = {};
    }

    if (!type.IsValid() || type.Kind() == TypeKind::Enum) {
        return fail(ErrorCode::InvalidArgument, PropertyPathError::None, 0, 0);
    }

    const char* data = text.Data();
    const usize size = text.ByteCount();
    PropertyPathBuffer parsed;
    StructView scope;
    usize position = 0;
    while (true) {
        // 이름(ASCII 식별자)을 읽어 지금 범위(클래스·struct lease 스키마 또는 struct view)에서
        // 찾는다.
        const usize nameStart = position;
        if (position >= size || !IsIdentifierStart(data[position])) {
            return fail(ErrorCode::InvalidArgument, PropertyPathError::Syntax, parsed.count,
                        position);
        }

        while (position < size && IsIdentifierPart(data[position])) {
            ++position;
        }

        const Span<const char> name(data + nameStart, position - nameStart);
        const Span<const FieldSchema> fields =
            parsed.count == 0 ? type.Schema().fields : scope.Schema().fields;
        usize found = TypeLease::NoField;
        for (usize index = 0; index < fields.Size(); ++index) {
            // Opaque 필드는 경로로 닿을 수 없으므로 찾지 않는다.
            if (fields[index].value.kind != ValueKind::Opaque &&
                EqualsIgnoringAsciiCase(EffectiveToolName(fields[index]), name)) {
                found = index;
                break;
            }
        }

        if (found == TypeLease::NoField) {
            return fail(ErrorCode::NotFound, PropertyPathError::UnknownProperty, parsed.count,
                        nameStart);
        }

        const FieldSchema& field = fields[found];
        const StructView target =
            parsed.count == 0 ? type.FieldStruct(found) : scope.FieldStruct(found);
        PathStep& step = parsed.steps[parsed.count];
        step = PathStep{field.id, 0, PathSelector::None};
        const usize stepIndex = parsed.count++;
        if (position < size && data[position] == '[' && IsKeyedField(field)) {
            // key(리플렉션 B9f3).
            const EnumSchema* keyEnum =
                field.value.kind == ValueKind::Map
                    ? (stepIndex == 0 ? type.FieldKeyEnum(found) : scope.FieldKeyEnum(found))
                    : (stepIndex == 0 ? type.FieldEnum(found) : scope.FieldEnum(found));
            ++position;
            PropertyPathError reason = PropertyPathError::None;
            usize offset = 0;
            if (auto key = ParseKey(field, keyEnum, data, size, position, parsed, step, hook,
                                    reason, offset);
                !key) {
                return fail(key.Error(), reason, stepIndex, offset);
            }

            // Map 항목·Set 원소는 배열이 아니다(중첩 컨테이너 없음).
            if (position < size && data[position] == '[') {
                return fail(ErrorCode::TypeMismatch, PropertyPathError::NotArray, stepIndex,
                            position);
            }
        } else if (position < size && data[position] == '[') {
            if (field.value.kind != ValueKind::Array) {
                return fail(ErrorCode::TypeMismatch, PropertyPathError::NotArray, stepIndex,
                            position);
            }

            // 정규 10진 index: `0` 하나 또는 0이 아닌 숫자로 시작, uint32 범위.
            ++position;
            const usize digitsStart = position;
            uint64 value = 0;
            while (position < size && IsDigit(data[position])) {
                value = value * 10 + static_cast<uint64>(data[position] - '0');
                if (value > NumericLimits<uint32>::Max()) {
                    return fail(ErrorCode::InvalidArgument, PropertyPathError::Syntax, stepIndex,
                                digitsStart);
                }

                ++position;
            }

            if (position == digitsStart ||
                (data[digitsStart] == '0' && position - digitsStart > 1)) {
                return fail(ErrorCode::InvalidArgument, PropertyPathError::Syntax, stepIndex,
                            digitsStart);
            }

            if (position >= size || data[position] != ']') {
                return fail(ErrorCode::InvalidArgument, PropertyPathError::Syntax, stepIndex,
                            position);
            }

            ++position;
            step.index = static_cast<uint32>(value);
            step.selector = PathSelector::Index;
            // 배열 원소는 배열일 수 없다(중첩 배열 없음).
            if (position < size && data[position] == '[') {
                return fail(ErrorCode::TypeMismatch, PropertyPathError::NotArray, stepIndex,
                            position);
            }
        }

        if (position == size) {
            break;
        }

        if (data[position] != '.') {
            return fail(ErrorCode::InvalidArgument, PropertyPathError::Syntax, parsed.count,
                        position);
        }

        if (parsed.count == MaxPropertyPathSteps) {
            return fail(ErrorCode::InvalidArgument, PropertyPathError::TooDeep, parsed.count,
                        position);
        }

        if (!IsStructStep(field, step.selector) || target.IsEmpty()) {
            return fail(ErrorCode::TypeMismatch, PropertyPathError::NotStruct, stepIndex, position);
        }

        ++position;
        scope = target;
    }

    out = parsed;
    return Result<void>::Success();
}

} // namespace detail

Result<void> ParsePropertyPath(const TypeLease& type, StringView text, PropertyPathBuffer& out,
                               PropertyPathFailure* failure, PathKeyTextHook* hook) noexcept {
    return detail::PropertyPathParser::Parse(type, text, out, failure, hook);
}

Result<void> FormatPropertyPath(const TypeLease& type, PropertyPath path, String& out,
                                PathKeyTextHook* hook) noexcept {
    const Span<const PathStep> steps = path.steps;
    if (!type.IsValid() || type.Kind() == TypeKind::Enum || steps.Size() == 0 ||
        steps.Size() > MaxPropertyPathSteps) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    Span<const char> names[MaxPropertyPathSteps];
    KeyPiece pieces[MaxPropertyPathSteps];
    Optional<String> hookTexts[MaxPropertyPathSteps];
    char keyBuffers[MaxPropertyPathSteps][KeyTextBytes];
    usize bytes = 0;
    StructView scope;
    for (usize index = 0; index < steps.Size(); ++index) {
        const PathStep& step = steps[index];
        if (!IsCanonicalPathStep(step, path.keys.Size())) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        const usize position =
            index == 0 ? type.FieldIndex(step.fieldId) : scope.FieldIndex(step.fieldId);
        if (position == TypeLease::NoField) {
            return Result<void>::Failure(ErrorCode::NotFound);
        }

        const FieldSchema& field =
            index == 0 ? type.Schema().fields[position] : scope.Schema().fields[position];
        if (field.value.kind == ValueKind::Opaque) {
            return Result<void>::Failure(ErrorCode::Unsupported);
        }

        if ((step.selector == PathSelector::Index && field.value.kind != ValueKind::Array) ||
            (step.selector == PathSelector::Key && !IsKeyedField(field))) {
            return Result<void>::Failure(ErrorCode::TypeMismatch);
        }

        const StructView target =
            index == 0 ? type.FieldStruct(position) : scope.FieldStruct(position);
        if (index + 1 < steps.Size() && (!IsStructStep(field, step.selector) || target.IsEmpty())) {
            return Result<void>::Failure(ErrorCode::TypeMismatch);
        }

        names[index] = EffectiveToolName(field);
        bytes += names[index].Size() + (index != 0 ? 1 : 0);
        if (step.selector == PathSelector::Index) {
            bytes += 2 + DecimalDigits(step.index);
        } else if (step.selector == PathSelector::Key) {
            const EnumSchema* keyEnum =
                field.value.kind == ValueKind::Map
                    ? (index == 0 ? type.FieldKeyEnum(position) : scope.FieldKeyEnum(position))
                    : (index == 0 ? type.FieldEnum(position) : scope.FieldEnum(position));
            if (auto formatted = FormatKeyText(KeyTypeOf(field), keyEnum, path.keys[step.index],
                                               hook, out.GetAllocator(), hookTexts[index],
                                               keyBuffers[index], pieces[index]);
                !formatted) {
                return formatted;
            }

            bytes += 2 + pieces[index].bytes;
        }

        scope = target;
    }

    String result(out.GetAllocator());
    if (auto reserved = result.TryReserve(bytes); !reserved) {
        return reserved;
    }

    for (usize index = 0; index < steps.Size(); ++index) {
        if (index != 0 && !result.TryAppend(".")) {
            return Result<void>::Failure(ErrorCode::OutOfMemory);
        }

        if (!result.TryAppend(StringView(names[index].Data(), names[index].Size()))) {
            return Result<void>::Failure(ErrorCode::OutOfMemory);
        }

        if (steps[index].selector == PathSelector::Index) {
            char digits[KeyTextBytes];
            const Span<const char> text =
                FormatDecimal(steps[index].index, false, digits, KeyTextBytes);
            if (!result.TryAppend("[") || !result.TryAppend(StringView(text.Data(), text.Size())) ||
                !result.TryAppend("]")) {
                return Result<void>::Failure(ErrorCode::OutOfMemory);
            }
        } else if (steps[index].selector == PathSelector::Key) {
            if (auto appended = AppendKeyPiece(pieces[index], result); !appended) {
                return appended;
            }
        }
    }

    out.Swap(result);
    return Result<void>::Success();
}

} // namespace rocks
