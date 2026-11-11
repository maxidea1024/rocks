#include "NumberTextInternal.h"

#include <Core/Text/StringBuilder.h>
#include <Platform/Fatal.h>

#include <math.h>

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
        platform::Fatal("StringBuilder storage was not owned by its allocator");
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

// 새 저장소로 옮긴다. 원본이 이 builder를 가리킬 수 있는 append는 Grow 전에 원본 위치를 보정한다.
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
        platform::Fatal("StringBuilder storage was not owned by its allocator");
    }
    data_ = replacement;
    capacity_ = capacity;
    return Result<void>::Success();
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
        // 원본이 이 builder의 내용이면 성장 뒤의 같은 위치에서 읽는다.
        const auto address = reinterpret_cast<usize>(bytes);
        const auto begin = reinterpret_cast<usize>(data_);
        const bool aliased = address >= begin && address < begin + size_;
        const usize offset = aliased ? static_cast<usize>(address - begin) : 0;
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
    auto valid = separator.ValidateUtf8();
    if (!valid) {
        return valid;
    }
    usize total = 0;
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
    }
    // 조각이 이 builder를 가리킬 수 있으므로 별도 builder에 모은 뒤 한 번에 붙인다.
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

bool IsAlign(char character) noexcept {
    return character == '<' || character == '>' || character == '^';
}

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
            return Result<void>::Failure(ErrorCode::InvalidArgument);
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
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
        spec.width = number;
    }
    if (index < bytes && data[index] == '.') {
        ++index;
        if (!ParseNumber(data, bytes, index, number)) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
        spec.precision = number;
    }
    if (index < bytes) {
        spec.type = data[index++];
    }
    return index == bytes ? Result<void>::Success()
                          : Result<void>::Failure(ErrorCode::InvalidArgument);
}

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

Result<void> AppendFloating(StringBuilderBase& out, double value, bool single,
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
    const bool negative = signbit(value) != 0;
    if (isnan(value) || isinf(value)) {
        const char* word = isnan(value) ? (upper ? "NAN" : "nan") : (upper ? "INF" : "inf");
        return AppendNumeric(out, SignFor(negative, spec), StringView(), StringView(word), spec,
                             false);
    }
    const double magnitude = negative ? -value : value;
    char small[128];
    char* buffer = small;
    usize capacity = sizeof(small);
    usize length = 0;
    if (type == 0 && spec.precision == NoPrecision) {
        length = FormatShortestFloatingPoint(magnitude, single, buffer, capacity);
    } else {
        if (type == 0) {
            type = 'g';
        } else if (type == 'F') {
            type = 'f';
        }
        const int precision = spec.precision == NoPrecision ? 6 : static_cast<int>(spec.precision);
        length = FormatFloatingPoint(magnitude, type, precision, buffer, capacity);
        if (length != MaxSize && length >= capacity) {
            // 큰 지수의 고정 소수점이나 큰 정밀도는 heap에 다시 쓴다.
            capacity = length + 1;
            auto allocation = out.GetAllocator().Allocate(capacity, 1);
            if (!allocation) {
                return Result<void>::Failure(allocation.Error());
            }
            buffer = static_cast<char*>(allocation.Value());
            length = FormatFloatingPoint(magnitude, type, precision, buffer, capacity);
        }
    }
    Result<void> result = Fail();
    if (length != MaxSize && length < capacity) {
        result = AppendNumeric(out, SignFor(negative, spec), StringView(),
                               StringView(buffer, length), spec, true);
    }
    if (buffer != small && !out.GetAllocator().Deallocate(buffer)) {
        platform::Fatal("Format scratch storage was not owned by its allocator");
    }
    return result;
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
        return AppendInteger(out, code, false, spec);
    }
    case FormatArg::Kind::Signed:
        return AppendSigned(out, arg.signedValue, spec);
    case FormatArg::Kind::Unsigned:
        return AppendInteger(out, arg.unsignedValue, false, spec);
    case FormatArg::Kind::Float:
        return AppendFloating(out, static_cast<double>(arg.single), true, spec);
    case FormatArg::Kind::Double:
        return AppendFloating(out, arg.real, false, spec);
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

} // namespace

Result<void> FormatTo(StringBuilderBase& out, StringView format,
                      Span<const FormatArg> args) noexcept {
    auto valid = format.ValidateUtf8();
    if (!valid) {
        return valid;
    }
    // 인자나 format이 out의 내용을 가리킬 수 있고 실패 시 되돌려야 하므로 별도 builder에 쓴다.
    StringBuilder<256> scratch(out.GetAllocator());
    const char* text = format.Data();
    const usize bytes = format.ByteCount();
    usize next = 0;
    bool automatic = false;
    bool manual = false;
    usize index = 0;
    while (index < bytes) {
        const char character = text[index];
        if (character != '{' && character != '}') {
            usize end = index;
            while (end < bytes && text[end] != '{' && text[end] != '}') {
                ++end;
            }
            auto appended = scratch.AppendRaw(text + index, end - index);
            if (!appended) {
                return appended;
            }
            index = end;
            continue;
        }
        if (character == '}') {
            if (index + 1 >= bytes || text[index + 1] != '}') {
                return Fail();
            }
            auto appended = scratch.AppendRaw("}", 1);
            if (!appended) {
                return appended;
            }
            index += 2;
            continue;
        }
        if (index + 1 < bytes && text[index + 1] == '{') {
            auto appended = scratch.AppendRaw("{", 1);
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
        if (index >= bytes || text[index] != '}') {
            return Fail();
        }
        if (argument >= args.Size()) {
            return Fail();
        }
        auto appended = AppendArgument(scratch, args[argument],
                                       StringView(text + specStart, index - specStart), hasSpec);
        if (!appended) {
            return appended;
        }
        ++index;
    }
    if (scratch.HasFailed()) {
        return scratch.Status();
    }
    return out.AppendRaw(scratch.data_, scratch.size_);
}

} // namespace detail

} // namespace rocks
