#include "DecimalConversion.h"
#include "NumberTextInternal.h"

#include <Core/Text/NumberText.h>
#include <Platform/Memory.h>
#include <Platform/PlatformMath.h>

namespace rocks {
namespace {

namespace decimal = detail::decimal;

bool IsDigit(char character) noexcept {
    return character >= '0' && character <= '9';
}

char LowerAscii(char character) noexcept {
    return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a')
                                                : character;
}

bool EqualsIgnoreAsciiCase(const char* text, usize bytes, const char* word) noexcept {
    if (bytes != platform::CStringLength(word)) {
        return false;
    }

    for (usize index = 0; index < bytes; ++index) {
        if (LowerAscii(text[index]) != word[index]) {
            return false;
        }
    }

    return true;
}

// 10진 실수 [+|-](digits[.[digits]]|.digits)([e|E][+|-]digits)를 읽어 가장 가까운 값으로 반올림한
// IEEE 비트(부호 포함, single이면 하위 32비트가 float 비트)를 돌려준다. float로 바로
// 반올림하고(double을 거쳐 두 번 반올림하지 않는다) 값을 부동소수점 변환 명령에 통과시키지 않으므로
// FTZ·DAZ 환경에서도 비정규 수가 그대로다. 입력 길이 제한은 없다: 앞 MaxSignificantDigits개 유효
// 숫자를 보관하고 그 뒤는 0이 아닌지만 기록한다.
Result<uint64> ParseFloatingPoint(StringView view, bool single) noexcept {
    const char* text = view.Data();
    const usize bytes = view.ByteCount();
    if (bytes == 0) {
        return Result<uint64>::Failure(ErrorCode::InvalidArgument);
    }

    const decimal::BinaryFormat format = single ? decimal::FloatFormat : decimal::DoubleFormat;
    const uint64 signBit = uint64{1} << (format.mantissaBits + format.exponentBits);
    const uint64 infinityBits = static_cast<uint64>((1 << format.exponentBits) - 1)
                                << format.mantissaBits;
    usize index = 0;
    bool negative = false;
    if (text[0] == '+' || text[0] == '-') {
        negative = text[0] == '-';
        index = 1;
    }

    // 특수 값: [+|-](inf|infinity|nan), 대소문자 무관.
    if (EqualsIgnoreAsciiCase(text + index, bytes - index, "inf") ||
        EqualsIgnoreAsciiCase(text + index, bytes - index, "infinity")) {
        return Result<uint64>::Success(infinityBits | (negative ? signBit : 0));
    }

    if (EqualsIgnoreAsciiCase(text + index, bytes - index, "nan")) {
        // quiet NaN: 지수 전부 1, 가수 최상위 비트.
        const uint64 quiet = infinityBits | (uint64{1} << (format.mantissaBits - 1));
        return Result<uint64>::Success(quiet | (negative ? signBit : 0));
    }

    decimal::DecimalNumber number;
    auto append = [&number](uint8 digit) noexcept {
        if (number.count < decimal::MaxSignificantDigits) {
            number.digits[number.count++] = digit;
        } else if (digit != 0) {
            number.truncated = true;
        }
    };

    // 정수부: 첫 유효 숫자부터 소수점 앞 자리 수만큼 pointPosition이 커진다.
    usize integerDigits = 0;
    while (index < bytes && IsDigit(text[index])) {
        const uint8 digit = static_cast<uint8>(text[index] - '0');
        if (number.count != 0 || digit != 0) {
            append(digit);
            ++number.pointPosition;
        }

        ++integerDigits;
        ++index;
    }

    // 소수부: 첫 유효 숫자 앞의 0은 pointPosition을 줄인다.
    usize fractionDigits = 0;
    if (index < bytes && text[index] == '.') {
        ++index;
        while (index < bytes && IsDigit(text[index])) {
            const uint8 digit = static_cast<uint8>(text[index] - '0');
            if (number.count == 0 && digit == 0) {
                --number.pointPosition;
            } else {
                append(digit);
            }

            ++fractionDigits;
            ++index;
        }
    }

    if (integerDigits + fractionDigits == 0) {
        return Result<uint64>::Failure(ErrorCode::InvalidArgument);
    }

    // 지수: 10^17 이상이 되면 더 읽지 않는다. 자릿수에서 오는 소수점 이동은 입력 byte 수를 넘지
    // 않으므로 포화한 지수를 상쇄하려면 입력이 10^17 byte를 넘어야 한다(pointPosition은 int64라
    // 넘치지 않는다). 10^9로 두면 1 GiB 입력이 상쇄할 수 있었다(교차검증에서 재현).
    if (index < bytes && (text[index] == 'e' || text[index] == 'E')) {
        ++index;
        bool negativeExponent = false;
        if (index < bytes && (text[index] == '+' || text[index] == '-')) {
            negativeExponent = text[index] == '-';
            ++index;
        }

        int64 exponent = 0;
        usize exponentDigits = 0;
        while (index < bytes && IsDigit(text[index])) {
            if (exponent < 100000000000000000ll) {
                exponent = exponent * 10 + (text[index] - '0');
            }

            ++exponentDigits;
            ++index;
        }

        if (exponentDigits == 0) {
            return Result<uint64>::Failure(ErrorCode::InvalidArgument);
        }

        if (number.count != 0) {
            number.pointPosition += negativeExponent ? -exponent : exponent;
        }
    }

    if (index != bytes) {
        return Result<uint64>::Failure(ErrorCode::InvalidArgument);
    }

    if (number.count == 0) {
        return Result<uint64>::Success(negative ? signBit : 0);
    }

    const uint64 bits = decimal::ToBinary(number, format);
    if (bits == infinityBits) {
        return Result<uint64>::Failure(ErrorCode::Overflow);
    }

    return Result<uint64>::Success(bits | (negative ? signBit : 0));
}

int DigitValue(char character) noexcept {
    if (character >= '0' && character <= '9') {
        return character - '0';
    }

    const char lower = LowerAscii(character);
    if (lower >= 'a' && lower <= 'z') {
        return lower - 'a' + 10;
    }

    return 99;
}

// 부호를 뺀 자릿수를 limit 이하의 값으로 읽는다. 크기 초과는 Overflow, 문법 오류는 InvalidArgument.
Result<uint64> ParseMagnitude(const char* text, usize bytes, uint32 base, uint64 limit) noexcept {
    if (bytes == 0) {
        return Result<uint64>::Failure(ErrorCode::InvalidArgument);
    }

    uint64 value = 0;
    bool overflow = false;
    for (usize index = 0; index < bytes; ++index) {
        const int digit = DigitValue(text[index]);
        if (digit >= static_cast<int>(base)) {
            return Result<uint64>::Failure(ErrorCode::InvalidArgument);
        }

        // 넘친 뒤에도 끝까지 문법을 검사한다("999…9x"는 Overflow가 아니라 InvalidArgument다).
        if (!overflow) {
            if (value > (limit - static_cast<uint64>(digit)) / base) {
                overflow = true;
            } else {
                value = value * base + static_cast<uint64>(digit);
            }
        }
    }

    return overflow ? Result<uint64>::Failure(ErrorCode::Overflow) : Result<uint64>::Success(value);
}

Result<int64> ParseSigned(StringView view, uint32 base, int64 minimum, int64 maximum) noexcept {
    if (base < 2 || base > 36) {
        return Result<int64>::Failure(ErrorCode::InvalidArgument);
    }

    const char* text = view.Data();
    usize bytes = view.ByteCount();
    bool negative = false;
    if (bytes != 0 && (text[0] == '+' || text[0] == '-')) {
        negative = text[0] == '-';
        ++text;
        --bytes;
    }

    // 음수 쪽 한도는 |minimum|이다(-(minimum + 1) + 1로 overflow 없이 구한다).
    const uint64 limit =
        negative ? static_cast<uint64>(-(minimum + 1)) + 1u : static_cast<uint64>(maximum);
    auto magnitude = ParseMagnitude(text, bytes, base, limit);
    if (!magnitude) {
        return Result<int64>::Failure(magnitude.Error());
    }

    const uint64 value = magnitude.Value();
    if (!negative) {
        return Result<int64>::Success(static_cast<int64>(value));
    }

    // -(2^63)도 표현한다.
    return Result<int64>::Success(value == 0 ? 0 : -static_cast<int64>(value - 1u) - 1);
}

Result<uint64> ParseUnsigned(StringView view, uint32 base, uint64 maximum) noexcept {
    if (base < 2 || base > 36) {
        return Result<uint64>::Failure(ErrorCode::InvalidArgument);
    }

    const char* text = view.Data();
    usize bytes = view.ByteCount();
    if (bytes != 0 && text[0] == '+') {
        ++text;
        --bytes;
    }

    return ParseMagnitude(text, bytes, base, maximum);
}

// ---- 표기 ----

// capacity 안에서만 쓰고 전체 길이를 센다. 끝나면 NUL을 붙인다(넘쳤으면 잘린 내용).
class TextWriter {
public:
    TextWriter(char* buffer, usize capacity) noexcept : buffer_(buffer), capacity_(capacity) {
    }

    void Put(char character) noexcept {
        if (length_ + 1 < capacity_) {
            buffer_[length_] = character;
        }

        ++length_;
    }

    void PutRepeated(char character, int64 count) noexcept {
        for (int64 index = 0; index < count; ++index) {
            Put(character);
        }
    }

    // 지수를 부호와 최소 두 자리로 쓴다(printf 방식: e+05, e-300).
    void PutExponent(char marker, int32 exponent) noexcept {
        Put(marker);
        Put(exponent < 0 ? '-' : '+');
        const uint32 magnitude = static_cast<uint32>(exponent < 0 ? -exponent : exponent);
        char digits[10];
        int count = 0;
        uint32 rest = magnitude;
        do {
            digits[count++] = static_cast<char>('0' + rest % 10);
            rest /= 10;
        } while (rest != 0);

        if (count < 2) {
            Put('0');
        }

        while (count > 0) {
            Put(digits[--count]);
        }
    }

    usize Finish() noexcept {
        if (capacity_ != 0) {
            buffer_[length_ < capacity_ ? length_ : capacity_ - 1] = '\0';
        }

        return length_;
    }

    usize Length() const noexcept {
        return length_;
    }

private:
    char* buffer_;
    usize capacity_;
    usize length_ = 0;
};

// 정확한 전개를 소수점 아래 precision자리로 반올림해 고정 소수점으로 쓴다.
void WriteFixed(decimal::ExactDecimal value, int32 precision, TextWriter& out) noexcept {
    decimal::RoundExact(value, static_cast<int64>(value.pointPosition) + precision);
    const int64 point = value.count == 0 ? 0 : value.pointPosition;
    if (point <= 0) {
        out.Put('0');
    } else {
        for (int64 index = 0; index < point; ++index) {
            out.Put(index < static_cast<int64>(value.count) ? value.digits[index] : '0');
        }
    }

    if (precision > 0) {
        out.Put('.');
        for (int64 offset = 0; offset < precision; ++offset) {
            const int64 index = point + offset;
            out.Put(value.count != 0 && index >= 0 && index < static_cast<int64>(value.count)
                        ? value.digits[index]
                        : '0');
        }
    }
}

// 정확한 전개를 유효 숫자 precision + 1자리로 반올림해 d.ddd e±XX로 쓴다.
void WriteScientific(decimal::ExactDecimal value, int32 precision, bool upper,
                     TextWriter& out) noexcept {
    decimal::RoundExact(value, static_cast<int64>(precision) + 1);
    out.Put(value.count == 0 ? '0' : value.digits[0]);
    if (precision > 0) {
        out.Put('.');
        for (int32 index = 1; index <= precision; ++index) {
            out.Put(static_cast<usize>(index) < value.count ? value.digits[index] : '0');
        }
    }

    out.PutExponent(upper ? 'E' : 'e', value.count == 0 ? 0 : value.pointPosition - 1);
}

// printf %g: 유효 숫자 P(0이면 1)로 반올림했을 때의 지수 X가 −4 ≤ X < P면 고정 소수점(소수 P − 1 −
// X자리), 아니면 과학 표기(소수 P − 1자리). 그 뒤 소수부 끝의 0과 남는 소수점을 지운다('#' 없음).
// 정밀도 상한(StringBuilder의 MaxFloatingPrecision과 같다). %g의 지우기 전 결과는 1080자 이하다.
constexpr int MaxPrecision = 1074;

usize WriteGeneral(const decimal::ExactDecimal& value, int32 precision, bool upper, char* buffer,
                   usize capacity) noexcept {
    const int32 significant = precision == 0 ? 1 : precision;
    decimal::ExactDecimal rounded = value;
    decimal::RoundExact(rounded, significant);
    const int32 exponent = rounded.count == 0 ? 0 : rounded.pointPosition - 1;
    // 지우기 전 결과를 내부 buffer에 만들고, 지운 결과의 길이를 돌려준다(capacity가 부족해도 필요한
    // 길이).
    char scratch[1100];
    TextWriter out(scratch, sizeof(scratch));
    if (exponent >= -4 && exponent < significant) {
        WriteFixed(value, significant - 1 - exponent, out);
    } else {
        WriteScientific(value, significant - 1, upper, out);
    }

    usize length = out.Finish();
    if (length >= sizeof(scratch)) {
        return MaxSize; // 정밀도 상한 안에서는 일어나지 않는다
    }

    // 소수점이 있으면 가수 부분 끝의 0과 소수점을 지우고 지수 부분을 앞으로 당긴다.
    usize mantissaEnd = 0;
    bool hasPoint = false;
    while (mantissaEnd < length && scratch[mantissaEnd] != 'e' && scratch[mantissaEnd] != 'E') {
        hasPoint = hasPoint || scratch[mantissaEnd] == '.';
        ++mantissaEnd;
    }

    if (hasPoint) {
        usize trimmed = mantissaEnd;
        while (scratch[trimmed - 1] == '0') {
            --trimmed;
        }

        if (scratch[trimmed - 1] == '.') {
            --trimmed;
        }

        const usize exponentLength = length - mantissaEnd;
        for (usize index = 0; index < exponentLength; ++index) {
            scratch[trimmed + index] = scratch[mantissaEnd + index];
        }

        length = trimmed + exponentLength;
    }

    TextWriter result(buffer, capacity);
    for (usize index = 0; index < length; ++index) {
        result.Put(scratch[index]);
    }

    return result.Finish();
}

} // namespace

Result<int64> TryParseInt64(StringView text, uint32 base) noexcept {
    return ParseSigned(text, base, -0x7fffffffffffffffll - 1, 0x7fffffffffffffffll);
}

Result<uint64> TryParseUInt64(StringView text, uint32 base) noexcept {
    return ParseUnsigned(text, base, ~0ull);
}

Result<int32> TryParseInt32(StringView text, uint32 base) noexcept {
    auto parsed = ParseSigned(text, base, -0x7fffffffll - 1, 0x7fffffffll);
    return parsed ? Result<int32>::Success(static_cast<int32>(parsed.Value()))
                  : Result<int32>::Failure(parsed.Error());
}

Result<uint32> TryParseUInt32(StringView text, uint32 base) noexcept {
    auto parsed = ParseUnsigned(text, base, 0xffffffffull);
    return parsed ? Result<uint32>::Success(static_cast<uint32>(parsed.Value()))
                  : Result<uint32>::Failure(parsed.Error());
}

Result<double> TryParseDouble(StringView text) noexcept {
    auto parsed = ParseFloatingPoint(text, false);
    return parsed ? Result<double>::Success(platform::BitCast<double>(parsed.Value()))
                  : Result<double>::Failure(parsed.Error());
}

Result<float> TryParseFloat(StringView text) noexcept {
    auto parsed = ParseFloatingPoint(text, true);
    return parsed ? Result<float>::Success(
                        platform::BitCast<float>(static_cast<uint32>(parsed.Value())))
                  : Result<float>::Failure(parsed.Error());
}

namespace detail {

usize FormatFloatingPoint(uint64 doubleBits, char type, int precision, char* buffer,
                          usize capacity) noexcept {
    if (precision < 0 || precision > MaxPrecision) {
        return MaxSize;
    }

    decimal::ExactDecimal exact;
    decimal::ExpandExact(doubleBits, exact);
    switch (type) {
    case 'f': {
        TextWriter out(buffer, capacity);
        WriteFixed(exact, precision, out);
        return out.Finish();
    }
    case 'e':
    case 'E': {
        TextWriter out(buffer, capacity);
        WriteScientific(exact, precision, type == 'E', out);
        return out.Finish();
    }
    case 'g':
    case 'G':
        return WriteGeneral(exact, precision, type == 'G', buffer, capacity);
    default:
        return MaxSize;
    }
}

usize FormatShortestFloatingPoint(uint64 bits, bool single, char* buffer, usize capacity) noexcept {
    if (capacity < 32) {
        return MaxSize;
    }

    // 0 판정도 비트로 한다(DAZ 환경의 비정규 수를 0으로 보지 않는다).
    if (bits == 0) {
        buffer[0] = '0';
        buffer[1] = '\0';
        return 1;
    }

    // Ryu 최단 표기: significand·10^exponent. 끝의 0은 유효 숫자가 아니다.
    const decimal::ShortestDecimal shortest =
        decimal::Shortest(bits, single ? decimal::FloatFormat : decimal::DoubleFormat);
    uint64 significand = shortest.significand;
    int32 decimalExponent = shortest.exponent;
    while (significand % 10 == 0) {
        significand /= 10;
        ++decimalExponent;
    }

    char digits[24];
    int count = 0;
    char reversed[24];
    while (significand != 0) {
        reversed[count++] = static_cast<char>('0' + significand % 10);
        significand /= 10;
    }

    for (int index = 0; index < count; ++index) {
        digits[index] = reversed[count - 1 - index];
    }

    // 첫 유효 숫자의 10진 지수가 −4 이상 16 미만이면 고정 소수점, 아니면 과학 표기로 조립한다. 찾은
    // 유효 숫자만 쓰고 이진 값을 다시 전개하지 않는다(그래서 1e11f도 "100000000000"이다).
    const int32 exponent = decimalExponent + count - 1;
    TextWriter out(buffer, capacity);
    if (exponent < -4 || exponent >= 16) {
        out.Put(digits[0]);
        if (count > 1) {
            out.Put('.');
            for (int index = 1; index < count; ++index) {
                out.Put(digits[index]);
            }
        }

        out.PutExponent('e', exponent);
    } else if (exponent < 0) {
        // 0.000ddd
        out.Put('0');
        out.Put('.');
        out.PutRepeated('0', -exponent - 1);
        for (int index = 0; index < count; ++index) {
            out.Put(digits[index]);
        }
    } else {
        // 정수부 exponent + 1자리(모자라면 0으로 채운다), 남는 숫자는 소수부다.
        const int integerDigits = exponent + 1;
        for (int index = 0; index < integerDigits; ++index) {
            out.Put(index < count ? digits[index] : '0');
        }

        if (count > integerDigits) {
            out.Put('.');
            for (int index = integerDigits; index < count; ++index) {
                out.Put(digits[index]);
            }
        }
    }

    const usize length = out.Finish();
    return length >= capacity ? MaxSize : length;
}

} // namespace detail

} // namespace rocks
