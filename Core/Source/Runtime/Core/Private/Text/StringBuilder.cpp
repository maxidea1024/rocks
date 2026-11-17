#include "DecimalConversion.h"
#include "NumberTextInternal.h"

#include <Core/Text/StringBuilder.h>
#include <Platform/Fatal.h>
#include <Platform/Memory.h>

namespace rocks {

// ---- StringBuilderBase ----

StringBuilderBase::StringBuilderBase(Allocator& allocator, char* inlineBuffer, usize inlineCapacity,
                                     bool truncating) noexcept
    : allocator_(&allocator), inline_(inlineBuffer), data_(inlineBuffer), capacity_(inlineCapacity),
      truncating_(truncating) {
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
    truncated_ = false;
}

Result<void> StringBuilderBase::TryReserve(usize byteCapacity) noexcept {
    // 잘림 모드는 할당하지 않는다. 확보는 늘 성공하고 넘치는 내용은 다음 붙이기가
    // 자른다(Formatter가 TryReserve를 불러도 실패하지 않게).
    if (byteCapacity <= capacity_ || truncating_) {
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

void StringBuilderBase::AppendTruncated(const char* bytes, usize count) noexcept {
    // 남은 용량에 들어가는 앞부분을 UTF-8 문자 경계에서 고른다.
    const usize fit = StringView(bytes, count).LeftBytesAtCodePoint(capacity_ - size_).ByteCount();
    if (fit < count) {
        truncated_ = true;
    }

    // 저장소가 움직이지 않으므로 원본이 이 builder의 앞 내용이어도 겹치지 않는다.
    for (usize index = 0; index < fit; ++index) {
        data_[size_ + index] = bytes[index];
    }

    size_ += fit;
    data_[size_] = '\0';
}

Result<void> StringBuilderBase::AppendRaw(const char* bytes, usize count) noexcept {
    if (count == 0) {
        return Result<void>::Success();
    }

    if (truncating_) {
        if (!truncated_) {
            AppendTruncated(bytes, count);
        }

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

    if (truncating_) {
        // 이미 잘렸으면 버리고, 아니면 남은 용량만큼만 쓴다.
        usize fit = truncated_ ? 0 : capacity_ - size_;
        if (fit > count) {
            fit = count;
        }

        for (usize index = 0; index < fit; ++index) {
            data_[size_ + index] = character;
        }

        size_ += fit;
        data_[size_] = '\0';
        truncated_ = truncated_ || fit < count;
        return Result<void>::Success();
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

    // 잘림 모드는 저장소가 움직이지 않으므로 별칭이어도 바로 쓰고, 넘치면 AppendRaw가 자른다.
    if (truncating_) {
        for (usize index = 0; index < parts.Size(); ++index) {
            if (index != 0) {
                (void)AppendRaw(separator.Data(), separator.ByteCount());
            }

            (void)AppendRaw(parts[index].Data(), parts[index].ByteCount());
        }

        return Result<void>::Success();
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

using Spec = FormatSpec;

constexpr usize NoPrecision = FormatNoPrecision;

// "%.1074f"의 최대 길이(정수부 309자리 + '.' + 1074자리)보다 크게 잡는다.
constexpr usize FloatingBufferBytes = 1408;

static_assert(FormatMaxFloatingPrecision + 310 < FloatingBufferBytes,
              "floating buffer must hold the longest fixed-point expansion");

Result<void> Fail() noexcept {
    return Result<void>::Failure(ErrorCode::InvalidArgument);
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

// spec은 IsValidIntegerFormatSpec을 통과한 값이다.
Result<void> AppendInteger(StringBuilderBase& out, uint64 magnitude, bool negative,
                           const Spec& spec) noexcept {
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

// spec은 IsValidTextFormatSpec을 통과한 값이다. 값(text)의 UTF-8만 여기서 본다.
Result<void> AppendText(StringBuilderBase& out, StringView text, const Spec& spec) noexcept {
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
// 않으므로(정수 연산만) FTZ·DAZ 환경에서도 비정규 수가 그대로 표기된다. spec은
// IsValidBuiltinFormatSpec(Float·Double)을 통과한 값이다.
Result<void> AppendFloating(StringBuilderBase& out, uint64 bits, bool single,
                            const Spec& spec) noexcept {
    char type = spec.type;
    const bool upper = type == 'F' || type == 'E' || type == 'G';

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

    if (length == FormatFailed || length >= sizeof(buffer)) {
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

    // spec 문법과 종류별 허용 조합은 로그 템플릿의 컴파일 타임 검증과 같은 함수로 본다.
    Spec spec;
    if (hasSpec && !ParseFormatSpec(specText, spec)) {
        return Fail();
    }

    if (!IsValidBuiltinFormatSpec(arg.kind, spec)) {
        return Fail();
    }

    switch (arg.kind) {
    case FormatArg::Kind::Bool:
        if (spec.type == 0 || spec.type == 's') {
            return AppendText(out, arg.boolean ? "true" : "false", spec);
        }

        return AppendInteger(out, arg.boolean ? 1u : 0u, false, spec);
    case FormatArg::Kind::Char: {
        const auto code = static_cast<unsigned char>(arg.character);
        if (spec.type == 0 || spec.type == 'c') {
            if (code >= 0x80) {
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
// 문법은 로그 템플릿과 같은 FormatParser가 해석한다(이름 자리는 받지 않는다).
Result<void> Render(StringBuilderBase& target, StringView format,
                    Span<const FormatArg> args) noexcept {
    FormatParser parser(format, false);
    FormatToken token;
    while (parser.Next(token)) {
        if (token.kind == FormatTokenKind::Literal) {
            auto appended = target.TryAppend(token.text);
            if (!appended) {
                return appended;
            }

            continue;
        }

        if (token.argument >= args.Size()) {
            return Fail();
        }

        auto appended = AppendArgument(target, args[token.argument], token.spec, token.hasSpec);
        if (!appended) {
            return appended;
        }
    }

    return parser.Error() == FormatParseError::None ? Result<void>::Success() : Fail();
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

    // 잘림 모드는 저장소가 움직이지 않으므로 별칭이어도 바로 쓴다(임시 builder는 할당할 수 있다).
    if (aliased && !out.truncating_) {
        StringBuilder<256> scratch(out.GetAllocator());
        auto rendered = Render(scratch, format, args);
        if (!rendered) {
            return rendered;
        }

        return out.AppendRaw(scratch.data_, scratch.size_);
    }

    // 보통은 out에 바로 쓰고, 실패하면 호출 전 길이와 잘림 표시로 되돌린다(추가 할당 없음).
    const usize start = out.size_;
    const bool truncated = out.truncated_;
    auto rendered = Render(out, format, args);
    if (!rendered) {
        out.Truncate(start);
        out.truncated_ = truncated;
    }

    return rendered;
}

Result<void> AppendFormatArgument(StringBuilderBase& out, const FormatArg& arg, StringView spec,
                                  bool hasSpec) noexcept {
    return AppendArgument(out, arg, spec, hasSpec);
}

namespace {

// 거절한 할당 요청 수다(스레드별). 시험이 무할당을 확인하는 용도다.
thread_local uint64 refusedAllocations = 0;

} // namespace

Result<void*> RefusingAllocator::Allocate(usize, usize, uint32) noexcept {
    ++refusedAllocations;
    return Result<void*>::Failure(ErrorCode::OutOfMemory);
}

Result<void> RefusingAllocator::Deallocate(void* address) noexcept {
    // 준 적이 없으므로 null이 아닌 주소는 받을 수 없다.
    return address == nullptr ? Result<void>::Success()
                              : Result<void>::Failure(ErrorCode::InvalidArgument);
}

uint64 FixedStringBuilderAllocationAttempts() noexcept {
    return refusedAllocations;
}

RefusingAllocator& FixedStringBuilderAllocator() noexcept {
    static RefusingAllocator allocator;
    return allocator;
}

} // namespace detail

namespace {

char* FixedBufferData(Span<char> buffer) noexcept {
    if (buffer.Size() == 0) {
        platform::Fatal("FixedStringBuilder requires at least one byte for the terminating NUL");
    }

    return buffer.Data();
}

} // namespace

FixedStringBuilder::FixedStringBuilder(Span<char> buffer) noexcept
    : StringBuilderBase(detail::FixedStringBuilderAllocator(), FixedBufferData(buffer),
                        buffer.Size() == 0 ? 0 : buffer.Size() - 1, true) {
}

} // namespace rocks
