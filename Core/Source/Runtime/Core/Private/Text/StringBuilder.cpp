#include "DecimalConversion.h"
#include "NumberTextInternal.h"

#include <Core/Text/StringBuilder.h>
#include <Platform/Fatal.h>
#include <Platform/Memory.h>

namespace rocks {

// ---- StringBuilderBase ----

StringBuilderBase::StringBuilderBase(Allocator& allocator, char* inlineBuffer,
                                     usize inlineCapacity) noexcept
    : allocator_(&allocator), inline_(inlineBuffer), data_(inlineBuffer),
      capacity_(inlineCapacity) {
    data_[0] = '\0';
}

StringBuilderBase::~StringBuilderBase() noexcept {
    if (data_ != inline_ && !allocator_->Deallocate(data_)) {
        platform::Fatal("StringBuilder storage deallocation was refused by its allocator");
    }
}

void StringBuilderBase::Clear() noexcept {
    size_ = 0;
    data_[0] = '\0';
    error_ = ErrorCode::None;
}

Result<void> StringBuilderBase::TryReserve(usize byteCapacity) noexcept {
    if (byteCapacity <= capacity_) {
        return Result<void>::Success();
    }

    return Grow(byteCapacity);
}

// 새 저장소(기존 용량의 2배 이상, 최소 64 byte)를 받아 내용을 옮기고 옛 저장소를 반환한다.
Result<void> StringBuilderBase::Grow(usize required) noexcept {
    if (required >= MaxSize) {
        return Result<void>::Failure(ErrorCode::Overflow);
    }

    usize capacity = capacity_ > (MaxSize - 1) / 2 ? MaxSize - 1 : capacity_ * 2;
    if (capacity < required) {
        capacity = required;
    }

    if (capacity < 64) {
        capacity = 64;
    }

    auto allocation = allocator_->Allocate(capacity + 1, 1);
    if (!allocation) {
        return Result<void>::Failure(allocation.Error());
    }

    char* replacement = static_cast<char*>(allocation.Value());
    if (replacement == nullptr) {
        platform::Fatal("Allocator returned null for a nonzero allocation");
    }

    for (usize index = 0; index <= size_; ++index) {
        replacement[index] = data_[index];
    }

    if (data_ != inline_ && !allocator_->Deallocate(data_)) {
        platform::Fatal("StringBuilder storage deallocation was refused by its allocator");
    }

    data_ = replacement;
    capacity_ = capacity;
    return Result<void>::Success();
}

bool StringBuilderBase::Overlaps(const char* address, usize bytes) const noexcept {
    if (address == nullptr || bytes == 0) {
        return false;
    }

    // 서로 다른 객체의 포인터 대소 비교는 정의되지 않으므로 정수 주소로 비교한다.
    const auto begin = reinterpret_cast<usize>(address);
    const auto storage = reinterpret_cast<usize>(data_);
    return begin < storage + capacity_ + 1 && storage < begin + bytes;
}

Result<void> StringBuilderBase::AppendRaw(const char* bytes, usize count) noexcept {
    if (count == 0) {
        return Result<void>::Success();
    }

    if (count > MaxSize - 1 - size_) {
        return Result<void>::Failure(ErrorCode::Overflow);
    }

    const usize required = size_ + count;
    if (required > capacity_) {
        // 원본이 이 builder의 내용이면 성장 뒤 같은 위치에서 읽는다.
        const bool aliased = Overlaps(bytes, count);
        const usize offset = aliased ? static_cast<usize>(reinterpret_cast<usize>(bytes) -
                                                          reinterpret_cast<usize>(data_))
                                     : 0;
        auto grown = Grow(required);
        if (!grown) {
            return grown;
        }

        if (aliased) {
            bytes = data_ + offset;
        }
    }

    for (usize index = 0; index < count; ++index) {
        data_[size_ + index] = bytes[index];
    }

    size_ = required;
    data_[size_] = '\0';
    return Result<void>::Success();
}

void StringBuilderBase::Truncate(usize size) noexcept {
    if (size < size_) {
        size_ = size;
        data_[size_] = '\0';
    }
}

Result<void> StringBuilderBase::TryAppend(StringView text) noexcept {
    auto valid = text.ValidateUtf8();
    if (!valid) {
        return valid;
    }

    return AppendRaw(text.Data(), text.ByteCount());
}

Result<void> StringBuilderBase::TryAppendAscii(char character) noexcept {
    if (static_cast<unsigned char>(character) >= 0x80) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return AppendRaw(&character, 1);
}

Result<void> StringBuilderBase::TryAppendCodePoint(uint32 codePoint) noexcept {
    char encoded[4];
    usize count = 0;
    if (codePoint < 0x80) {
        encoded[count++] = static_cast<char>(codePoint);
    } else if (codePoint < 0x800) {
        encoded[count++] = static_cast<char>(0xc0 | (codePoint >> 6));
        encoded[count++] = static_cast<char>(0x80 | (codePoint & 0x3f));
    } else if (codePoint >= 0xd800 && codePoint <= 0xdfff) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    } else if (codePoint < 0x10000) {
        encoded[count++] = static_cast<char>(0xe0 | (codePoint >> 12));
        encoded[count++] = static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f));
        encoded[count++] = static_cast<char>(0x80 | (codePoint & 0x3f));
    } else if (codePoint <= 0x10ffff) {
        encoded[count++] = static_cast<char>(0xf0 | (codePoint >> 18));
        encoded[count++] = static_cast<char>(0x80 | ((codePoint >> 12) & 0x3f));
        encoded[count++] = static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f));
        encoded[count++] = static_cast<char>(0x80 | (codePoint & 0x3f));
    } else {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return AppendRaw(encoded, count);
}

Result<void> StringBuilderBase::TryAppendRepeated(char character, usize count) noexcept {
    if (static_cast<unsigned char>(character) >= 0x80) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (count > MaxSize - 1 - size_) {
        return Result<void>::Failure(ErrorCode::Overflow);
    }

    auto reserved = TryReserve(size_ + count);
    if (!reserved) {
        return reserved;
    }

    for (usize index = 0; index < count; ++index) {
        data_[size_ + index] = character;
    }

    size_ += count;
    data_[size_] = '\0';
    return Result<void>::Success();
}

Result<void> StringBuilderBase::TryAppendJoin(Span<const StringView> parts,
                                              StringView separator) noexcept {
    // 1) 모든 조각을 검증하고 전체 길이를 구한다. 여기까지는 아무것도 바꾸지 않는다.
    auto valid = separator.ValidateUtf8();
    if (!valid) {
        return valid;
    }

    usize total = 0;
    bool aliased = Overlaps(separator.Data(), separator.ByteCount());
    for (usize index = 0; index < parts.Size(); ++index) {
        valid = parts[index].ValidateUtf8();
        if (!valid) {
            return valid;
        }

        const usize piece = parts[index].ByteCount() + (index == 0 ? 0 : separator.ByteCount());
        if (piece > MaxSize - 1 - size_ - total) {
            return Result<void>::Failure(ErrorCode::Overflow);
        }

        total += piece;
        aliased = aliased || Overlaps(parts[index].Data(), parts[index].ByteCount());
    }

    // 2) 조각이 이 builder를 가리키면 성장 중 원본이 사라지므로 임시 builder에 모은 뒤 붙인다.
    if (aliased) {
        StringBuilder<256> joined(*allocator_);
        auto reserved = joined.TryReserve(total);
        if (!reserved) {
            return reserved;
        }

        for (usize index = 0; index < parts.Size(); ++index) {
            if (index != 0) {
                (void)joined.AppendRaw(separator.Data(), separator.ByteCount());
            }

            (void)joined.AppendRaw(parts[index].Data(), parts[index].ByteCount());
        }

        return AppendRaw(joined.data_, joined.size_);
    }

    // 3) 별칭이 없으면 한 번 확보한 뒤 바로 쓴다.
    auto reserved = TryReserve(size_ + total);
    if (!reserved) {
        return reserved;
    }

    for (usize index = 0; index < parts.Size(); ++index) {
        if (index != 0) {
            (void)AppendRaw(separator.Data(), separator.ByteCount());
        }

        (void)AppendRaw(parts[index].Data(), parts[index].ByteCount());
    }

    return Result<void>::Success();
}

Result<String> StringBuilderBase::TryToString(Allocator& allocator) const noexcept {
    String result(allocator);
    auto assigned = result.TryAssign(View());
    if (!assigned) {
        return Result<String>::Failure(assigned.Error());
    }

    return Result<String>::Success(Move(result));
}

// ---- 서식 ----

namespace detail {
namespace {

constexpr usize NoPrecision = MaxSize;
constexpr usize MaxWidth = 65535;

// double의 정확한 10진 전개는 소수점 아래 최대 1074자리다. 그보다 큰 정밀도는 받지 않으며, 이 상한
// 덕분에 실수 표기는 고정 크기 stack buffer에 들어간다(heap을 쓰지 않는다).
constexpr usize MaxFloatingPrecision = 1074;

// "%.1074f"의 최대 길이(정수부 309자리 + '.' + 1074자리)보다 크게 잡는다.
constexpr usize FloatingBufferBytes = 1408;

struct Spec {
    char fill = ' ';
    char align = 0;
    char sign = 0;
    bool alternate = false;
    bool zero = false;
    usize width = 0;
    usize precision = NoPrecision;
    char type = 0;
};

Result<void> Fail() noexcept {
    return Result<void>::Failure(ErrorCode::InvalidArgument);
}

bool IsAlign(char character) noexcept {
    return character == '<' || character == '>' || character == '^';
}

// 10진 숫자열을 읽는다. MaxWidth를 넘거나 숫자가 없으면 false다.
bool ParseNumber(const char* text, usize bytes, usize& index, usize& value) noexcept {
    const usize start = index;
    value = 0;
    while (index < bytes && text[index] >= '0' && text[index] <= '9') {
        value = value * 10 + static_cast<usize>(text[index] - '0');
        if (value > MaxWidth) {
            return false;
        }

        ++index;
    }

    return index != start;
}

// [[fill]align][sign][#][0][width][.precision][type]
Result<void> ParseSpec(StringView text, Spec& spec) noexcept {
    const char* data = text.Data();
    const usize bytes = text.ByteCount();
    usize index = 0;
    if (bytes >= 2 && IsAlign(data[1])) {
        const auto fill = static_cast<unsigned char>(data[0]);
        if (fill < 0x20 || fill >= 0x7f) {
            return Fail();
        }

        spec.fill = data[0];
        spec.align = data[1];
        index = 2;
    } else if (bytes >= 1 && IsAlign(data[0])) {
        spec.align = data[0];
        index = 1;
    }

    if (index < bytes && (data[index] == '+' || data[index] == '-' || data[index] == ' ')) {
        spec.sign = data[index++];
    }

    if (index < bytes && data[index] == '#') {
        spec.alternate = true;
        ++index;
    }

    if (index < bytes && data[index] == '0') {
        spec.zero = true;
        ++index;
    }

    usize number = 0;
    if (index < bytes && data[index] >= '1' && data[index] <= '9') {
        if (!ParseNumber(data, bytes, index, number)) {
            return Fail();
        }

        spec.width = number;
    }

    if (index < bytes && data[index] == '.') {
        ++index;
        if (!ParseNumber(data, bytes, index, number)) {
            return Fail();
        }

        spec.precision = number;
    }

    if (index < bytes) {
        spec.type = data[index++];
    }

    return index == bytes ? Result<void>::Success() : Fail();
}

// body(표시 폭 width 문자)를 spec의 폭·정렬·채움 문자로 붙인다.
Result<void> AppendAligned(StringBuilderBase& out, StringView body, usize width, const Spec& spec,
                           char defaultAlign) noexcept {
    if (spec.width <= width) {
        return out.TryAppend(body);
    }

    const usize padding = spec.width - width;
    const char align = spec.align != 0 ? spec.align : defaultAlign;
    const usize before = align == '<' ? 0 : align == '>' ? padding : padding / 2;
    auto result = out.TryAppendRepeated(spec.fill, before);
    if (result) {
        result = out.TryAppend(body);
    }

    if (result) {
        result = out.TryAppendRepeated(spec.fill, padding - before);
    }

    return result;
}

// 부호·접두사·숫자로 이루어진 ASCII 숫자 표기를 붙인다. '0' 채움은 부호·접두사 뒤에 넣는다.
Result<void> AppendNumeric(StringBuilderBase& out, char signCharacter, StringView prefix,
                           StringView digits, const Spec& spec, bool allowZero) noexcept {
    char head[4];
    usize headBytes = 0;
    if (signCharacter != 0) {
        head[headBytes++] = signCharacter;
    }

    for (usize index = 0; index < prefix.ByteCount(); ++index) {
        head[headBytes++] = prefix.Data()[index];
    }

    const usize total = headBytes + digits.ByteCount();
    const usize padding = spec.width > total ? spec.width - total : 0;
    if (spec.zero && allowZero && spec.align == 0) {
        auto result = out.TryAppend(StringView(head, headBytes));
        if (result) {
            result = out.TryAppendRepeated('0', padding);
        }

        if (result) {
            result = out.TryAppend(digits);
        }

        return result;
    }

    const char align = spec.align != 0 ? spec.align : '>';
    const usize before = align == '<' ? 0 : align == '>' ? padding : padding / 2;
    auto result = out.TryAppendRepeated(spec.fill, before);
    if (result) {
        result = out.TryAppend(StringView(head, headBytes));
    }

    if (result) {
        result = out.TryAppend(digits);
    }

    if (result) {
        result = out.TryAppendRepeated(spec.fill, padding - before);
    }

    return result;
}

char SignFor(bool negative, const Spec& spec) noexcept {
    if (negative) {
        return '-';
    }

    return spec.sign == '+' ? '+' : spec.sign == ' ' ? ' ' : 0;
}

Result<void> AppendInteger(StringBuilderBase& out, uint64 magnitude, bool negative,
                           const Spec& spec) noexcept {
    if (spec.precision != NoPrecision) {
        return Fail();
    }

    uint32 base = 10;
    const char* prefix = "";
    bool upper = false;
    switch (spec.type) {
    case 0:
    case 'd':
        break;
    case 'x':
        base = 16;
        prefix = "0x";
        break;
    case 'X':
        base = 16;
        prefix = "0X";
        upper = true;
        break;
    case 'b':
        base = 2;
        prefix = "0b";
        break;
    case 'B':
        base = 2;
        prefix = "0B";
        break;
    case 'o':
        base = 8;
        prefix = "0";
        break;
    default:
        return Fail();
    }

    char digits[72];
    usize cursor = sizeof(digits);
    do {
        const auto digit = static_cast<uint32>(magnitude % base);
        digits[--cursor] =
            static_cast<char>(digit < 10 ? '0' + digit : (upper ? 'A' : 'a') + digit - 10);
        magnitude /= base;
    } while (magnitude != 0);

    // C++ 서식처럼 8진 접두사 "0"은 값이 0이면 붙이지 않는다.
    const bool zeroOctal = base == 8 && cursor == sizeof(digits) - 1 && digits[cursor] == '0';
    const StringView prefixView = spec.alternate && !zeroOctal ? StringView(prefix) : StringView();
    return AppendNumeric(out, SignFor(negative, spec), prefixView,
                         StringView(digits + cursor, sizeof(digits) - cursor), spec, true);
}

Result<void> AppendSigned(StringBuilderBase& out, int64 value, const Spec& spec) noexcept {
    const bool negative = value < 0;
    // INT64_MIN도 표현하도록 부호 없는 크기로 바꾼다.
    const uint64 magnitude =
        negative ? static_cast<uint64>(-(value + 1)) + 1u : static_cast<uint64>(value);
    return AppendInteger(out, magnitude, negative, spec);
}

Result<void> AppendText(StringBuilderBase& out, StringView text, const Spec& spec) noexcept {
    if ((spec.type != 0 && spec.type != 's') || spec.sign != 0 || spec.alternate || spec.zero) {
        return Fail();
    }

    if (!text.ValidateUtf8()) {
        return Fail();
    }

    StringView body = text;
    if (spec.precision != NoPrecision) {
        auto truncated = text.TryLeftCodePoints(spec.precision);
        if (!truncated) {
            return Result<void>::Failure(truncated.Error());
        }

        body = truncated.Value();
    }

    if (spec.width == 0) {
        return out.TryAppend(body);
    }

    auto count = body.TryCountCodePoints();
    if (!count) {
        return Result<void>::Failure(count.Error());
    }

    return AppendAligned(out, body, count.Value(), spec, '<');
}

// bits는 single이면 float 비트, 아니면 double 비트다. 값을 부동소수점 변환 명령에 통과시키지
// 않으므로(정수 연산만) FTZ·DAZ 환경에서도 비정규 수가 그대로 표기된다.
Result<void> AppendFloating(StringBuilderBase& out, uint64 bits, bool single,
                            const Spec& spec) noexcept {
    char type = spec.type;
    const bool upper = type == 'F' || type == 'E' || type == 'G';
    if (type != 0 && type != 'f' && type != 'F' && type != 'e' && type != 'E' && type != 'g' &&
        type != 'G') {
        return Fail();
    }

    if (spec.alternate) {
        return Fail();
    }

    if (spec.precision != NoPrecision && spec.precision > MaxFloatingPrecision) {
        return Fail();
    }

    const int mantissaBits = single ? 23 : 52;
    const int exponentBits = single ? 8 : 11;
    const uint64 signBit = uint64{1} << (mantissaBits + exponentBits);
    const uint64 exponentMask = ((uint64{1} << exponentBits) - 1) << mantissaBits;
    const bool negative = (bits & signBit) != 0;
    if ((bits & exponentMask) == exponentMask) {
        const bool nan = (bits & ((uint64{1} << mantissaBits) - 1)) != 0;
        const char* word = nan ? (upper ? "NAN" : "nan") : (upper ? "INF" : "inf");
        return AppendNumeric(out, SignFor(negative, spec), StringView(), StringView(word), spec,
                             false);
    }

    // 부호는 따로 붙이므로 크기만 표기한다(-0.0도 '-'가 붙는다).
    const uint64 magnitude = bits & ~signBit;
    char buffer[FloatingBufferBytes];
    usize length = 0;
    if (type == 0 && spec.precision == NoPrecision) {
        length = FormatShortestFloatingPoint(magnitude, single, buffer, sizeof(buffer));
    } else {
        if (type == 0) {
            type = 'g';
        } else if (type == 'F') {
            type = 'f';
        }

        const int precision = spec.precision == NoPrecision ? 6 : static_cast<int>(spec.precision);
        // float는 정확한 double 비트로 넓힌다(정수 연산).
        const uint64 doubleBits =
            single ? detail::decimal::FloatBitsToDoubleBits(static_cast<uint32>(magnitude))
                   : magnitude;
        length = FormatFloatingPoint(doubleBits, type, precision, buffer, sizeof(buffer));
    }

    if (length == MaxSize || length >= sizeof(buffer)) {
        return Fail();
    }

    return AppendNumeric(out, SignFor(negative, spec), StringView(), StringView(buffer, length),
                         spec, true);
}

Result<void> AppendArgument(StringBuilderBase& out, const FormatArg& arg, StringView specText,
                            bool hasSpec) noexcept {
    if (arg.kind == FormatArg::Kind::Custom) {
        return arg.custom.format(out, arg.custom.object, specText);
    }

    Spec spec;
    if (hasSpec) {
        auto parsed = ParseSpec(specText, spec);
        if (!parsed) {
            return parsed;
        }
    }

    switch (arg.kind) {
    case FormatArg::Kind::Bool:
        if (spec.type == 0 || spec.type == 's') {
            // C++ 표준 format처럼 bool·char의 정밀도는 받지 않는다.
            if (spec.precision != NoPrecision) {
                return Fail();
            }

            return AppendText(out, arg.boolean ? "true" : "false", spec);
        }

        return AppendInteger(out, arg.boolean ? 1u : 0u, false, spec);
    case FormatArg::Kind::Char: {
        const auto code = static_cast<unsigned char>(arg.character);
        if (spec.type == 0 || spec.type == 'c') {
            if (code >= 0x80 || spec.precision != NoPrecision) {
                return Fail(); // UTF-8 byte 하나는 문자가 아니다
            }

            Spec text = spec;
            text.type = 0;
            return AppendText(out, StringView(&arg.character, 1), text);
        }

        // 정수 형식이면 플랫폼의 char 부호와 무관하게 부호 없는 값(0–255)으로 쓴다.
        return AppendInteger(out, code, false, spec);
    }
    case FormatArg::Kind::Signed:
        return AppendSigned(out, arg.signedValue, spec);
    case FormatArg::Kind::Unsigned:
        return AppendInteger(out, arg.unsignedValue, false, spec);
    case FormatArg::Kind::Float:
        return AppendFloating(out, platform::BitCast<uint32>(arg.single), true, spec);
    case FormatArg::Kind::Double:
        return AppendFloating(out, platform::BitCast<uint64>(arg.real), false, spec);
    case FormatArg::Kind::Text:
        return AppendText(out, StringView(arg.text.data, arg.text.bytes), spec);
    case FormatArg::Kind::NullText:
        return Fail();
    case FormatArg::Kind::Pointer: {
        if ((spec.type != 0 && spec.type != 'p') || spec.sign != 0 || spec.alternate) {
            return Fail();
        }

        Spec hex = spec;
        hex.type = 'x';
        hex.alternate = true;
        return AppendInteger(out, static_cast<uint64>(reinterpret_cast<usize>(arg.pointer)), false,
                             hex);
    }
    case FormatArg::Kind::Custom:
        break;
    }

    return Fail();
}

// format을 해석해 target에 붙인다. 실패하면 target에 일부가 남을 수 있으며 되돌리는 것은 호출자다.
Result<void> Render(StringBuilderBase& target, StringView format,
                    Span<const FormatArg> args) noexcept {
    const char* text = format.Data();
    const usize bytes = format.ByteCount();
    usize next = 0;
    bool automatic = false;
    bool manual = false;
    usize index = 0;
    while (index < bytes) {
        const char character = text[index];

        // 일반 문자열 구간. 중괄호는 ASCII이므로 이 구간도 유효한 UTF-8이다.
        if (character != '{' && character != '}') {
            usize end = index;
            while (end < bytes && text[end] != '{' && text[end] != '}') {
                ++end;
            }

            auto appended = target.TryAppend(StringView(text + index, end - index));
            if (!appended) {
                return appended;
            }

            index = end;
            continue;
        }

        // "}}"는 '}' 문자다. 짝 없는 '}'는 오류다.
        if (character == '}') {
            if (index + 1 >= bytes || text[index + 1] != '}') {
                return Fail();
            }

            auto appended = target.TryAppendAscii('}');
            if (!appended) {
                return appended;
            }

            index += 2;
            continue;
        }

        // "{{"는 '{' 문자다.
        if (index + 1 < bytes && text[index + 1] == '{') {
            auto appended = target.TryAppendAscii('{');
            if (!appended) {
                return appended;
            }

            index += 2;
            continue;
        }

        // 치환 필드: '{' [arg-id] [':' spec] '}'
        ++index;
        usize argument = 0;
        if (index < bytes && text[index] >= '0' && text[index] <= '9') {
            if (automatic) {
                return Fail();
            }

            manual = true;
            if (text[index] == '0' && index + 1 < bytes && text[index + 1] >= '0' &&
                text[index + 1] <= '9') {
                return Fail(); // 앞자리 0은 받지 않는다
            }

            if (!ParseNumber(text, bytes, index, argument)) {
                return Fail();
            }
        } else {
            if (manual) {
                return Fail();
            }

            automatic = true;
            argument = next++;
        }

        bool hasSpec = false;
        usize specStart = index;
        if (index < bytes && text[index] == ':') {
            hasSpec = true;
            specStart = ++index;
            while (index < bytes && text[index] != '}' && text[index] != '{') {
                ++index;
            }
        }

        if (index >= bytes || text[index] != '}' || argument >= args.Size()) {
            return Fail();
        }

        auto appended = AppendArgument(target, args[argument],
                                       StringView(text + specStart, index - specStart), hasSpec);
        if (!appended) {
            return appended;
        }

        ++index;
    }

    return Result<void>::Success();
}

} // namespace

Result<void> FormatTo(StringBuilderBase& out, StringView format,
                      Span<const FormatArg> args) noexcept {
    auto valid = format.ValidateUtf8();
    if (!valid) {
        return valid;
    }

    // format이나 문자열 인자가 out의 내용을 가리키면, out이 자라는 동안 원본이 사라지므로 임시
    // builder에 쓴 뒤 한 번에 붙인다.
    bool aliased = out.Overlaps(format.Data(), format.ByteCount());
    for (usize index = 0; index < args.Size() && !aliased; ++index) {
        if (args[index].kind == FormatArg::Kind::Text) {
            aliased = out.Overlaps(args[index].text.data, args[index].text.bytes);
        }
    }

    if (aliased) {
        StringBuilder<256> scratch(out.GetAllocator());
        auto rendered = Render(scratch, format, args);
        if (!rendered) {
            return rendered;
        }

        return out.AppendRaw(scratch.data_, scratch.size_);
    }

    // 보통은 out에 바로 쓰고, 실패하면 호출 전 길이로 되돌린다(추가 할당 없음).
    const usize start = out.size_;
    auto rendered = Render(out, format, args);
    if (!rendered) {
        out.Truncate(start);
    }

    return rendered;
}

} // namespace detail

} // namespace rocks
