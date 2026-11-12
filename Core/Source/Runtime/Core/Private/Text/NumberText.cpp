#include "NumberTextInternal.h"

#include <Core/Text/NumberText.h>
#include <Platform/Memory.h>
#include <Platform/PlatformMath.h>

#include <errno.h>
#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

namespace rocks {
namespace {

// strtod·strtof에 넘길 사본의 최대 크기다. 입력 1023 byte + locale 소수점(보통 1 byte) + NUL.
constexpr usize ParseBufferBytes = 1024 + 16;

// 현재 C locale의 소수점 문자열이다. 대부분 "."이며 일부 locale은 ","다.
const char* LocaleDecimalPoint() noexcept {
    const lconv* conventions = localeconv();
    if (conventions == nullptr || conventions->decimal_point == nullptr ||
        conventions->decimal_point[0] == '\0') {
        return ".";
    }

    return conventions->decimal_point;
}

// printf 결과에 들어간 locale 소수점을 '.'로 바꾼다. 소수점이 여러 byte면 길이가 줄어드므로 새
// 길이를 반환한다.
usize NormalizeDecimalPoint(char* buffer, usize length) noexcept {
    const char* point = LocaleDecimalPoint();
    if (point[0] == '.' && point[1] == '\0') {
        return length;
    }

    const usize pointLength = platform::CStringLength(point);
    for (usize index = 0; index + pointLength <= length; ++index) {
        bool match = true;
        for (usize offset = 0; offset < pointLength && match; ++offset) {
            match = buffer[index + offset] == point[offset];
        }

        if (!match) {
            continue;
        }

        buffer[index] = '.';
        // 나머지(끝의 NUL 포함)를 앞으로 당긴다.
        for (usize from = index + pointLength, to = index + 1; from <= length; ++from, ++to) {
            buffer[to] = buffer[from];
        }

        return length - pointLength + 1;
    }

    return length;
}

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

// 엄격한 10진 실수 문법인지 본다: [+|-](digits[.digits]|.digits)([e|E][+|-]digits).
// inf·nan은 호출자가 먼저 처리한다.
bool IsDecimalFloatingPoint(const char* text, usize bytes) noexcept {
    usize index = 0;
    if (index < bytes && (text[index] == '+' || text[index] == '-')) {
        ++index;
    }

    usize integerDigits = 0;
    while (index < bytes && IsDigit(text[index])) {
        ++index;
        ++integerDigits;
    }

    usize fractionDigits = 0;
    if (index < bytes && text[index] == '.') {
        ++index;
        while (index < bytes && IsDigit(text[index])) {
            ++index;
            ++fractionDigits;
        }
    }

    if (integerDigits + fractionDigits == 0) {
        return false;
    }

    if (index < bytes && (text[index] == 'e' || text[index] == 'E')) {
        ++index;
        if (index < bytes && (text[index] == '+' || text[index] == '-')) {
            ++index;
        }

        usize exponentDigits = 0;
        while (index < bytes && IsDigit(text[index])) {
            ++index;
            ++exponentDigits;
        }

        if (exponentDigits == 0) {
            return false;
        }
    }

    return index == bytes;
}

// 문법을 검사한 뒤 strtod/strtof로 읽는다. single이면 float로 읽는다(double을 거쳐 두 번
// 반올림하지 않는다).
Result<double> ParseFloatingPoint(StringView view, bool single) noexcept {
    const char* text = view.Data();
    const usize bytes = view.ByteCount();
    if (bytes == 0 || bytes > 1023) {
        return Result<double>::Failure(ErrorCode::InvalidArgument);
    }

    // 특수 값: [+|-](inf|infinity|nan), 대소문자 무관.
    usize start = 0;
    bool negative = false;
    if (text[0] == '+' || text[0] == '-') {
        negative = text[0] == '-';
        start = 1;
    }

    if (EqualsIgnoreAsciiCase(text + start, bytes - start, "inf") ||
        EqualsIgnoreAsciiCase(text + start, bytes - start, "infinity")) {
        const double infinity = platform::PlatformMath::Infinity<double>();
        return Result<double>::Success(negative ? -infinity : infinity);
    }

    if (EqualsIgnoreAsciiCase(text + start, bytes - start, "nan")) {
        const double nan = platform::PlatformMath::QuietNaN<double>();
        return Result<double>::Success(negative ? -nan : nan);
    }

    if (!IsDecimalFloatingPoint(text, bytes)) {
        return Result<double>::Failure(ErrorCode::InvalidArgument);
    }

    // C runtime은 현재 locale의 소수점을 기대하므로 '.'를 그것으로 바꾼 NUL 종료 사본을 만든다.
    char buffer[ParseBufferBytes];
    const char* point = LocaleDecimalPoint();
    const usize pointLength = platform::CStringLength(point);
    usize written = 0;
    for (usize index = 0; index < bytes; ++index) {
        const usize needed = text[index] == '.' ? pointLength : 1;
        if (written + needed >= sizeof(buffer)) {
            return Result<double>::Failure(ErrorCode::InvalidArgument);
        }

        if (text[index] == '.') {
            for (usize offset = 0; offset < pointLength; ++offset) {
                buffer[written++] = point[offset];
            }
        } else {
            buffer[written++] = text[index];
        }
    }

    buffer[written] = '\0';

    char* end = nullptr;
    errno = 0;
    const double value = single ? static_cast<double>(strtof(buffer, &end)) : strtod(buffer, &end);
    if (end != buffer + written) {
        return Result<double>::Failure(ErrorCode::InvalidArgument);
    }

    // ERANGE는 overflow(±HUGE)와 underflow(0·비정규) 모두에서 나온다. underflow는 값으로 받는다.
    if (errno == ERANGE && isinf(value)) {
        return Result<double>::Failure(ErrorCode::Overflow);
    }

    return Result<double>::Success(value);
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

// ---- 최단 왕복 표기 ----

// 유효 숫자열 digits(앞자리 0 없음)와 10진 지수로 나타낸 값 d.ddd × 10^exponent다.
struct DecimalDigits {
    char digits[24];
    int count = 0;
    int exponent = 0;
};

// "%.{p-1}e" 결과("d.ddde±XX" 또는 "de±XX")를 숫자열과 지수로 나눈다.
bool SplitScientific(const char* text, usize length, DecimalDigits& out) noexcept {
    out.count = 0;
    usize index = 0;
    while (index < length && text[index] != 'e' && text[index] != 'E') {
        if (IsDigit(text[index])) {
            if (out.count >= static_cast<int>(sizeof(out.digits))) {
                return false;
            }

            out.digits[out.count++] = text[index];
        }

        ++index;
    }

    if (index >= length || out.count == 0) {
        return false;
    }

    ++index;
    bool negative = false;
    if (index < length && (text[index] == '+' || text[index] == '-')) {
        negative = text[index++] == '-';
    }

    int exponent = 0;
    while (index < length && IsDigit(text[index])) {
        exponent = exponent * 10 + (text[index++] - '0');
    }

    out.exponent = negative ? -exponent : exponent;
    return true;
}

// 과학 표기 문자열 "d.ddde±X"를 만든다.
usize ComposeScientific(const DecimalDigits& value, char* buffer, usize capacity) noexcept {
    const int count = value.count == 1
                          ? snprintf(buffer, capacity, "%ce%+d", value.digits[0], value.exponent)
                          : snprintf(buffer, capacity, "%c.%.*se%+d", value.digits[0],
                                     value.count - 1, value.digits + 1, value.exponent);
    return count < 0 ? MaxSize : static_cast<usize>(count);
}

bool RoundTrips(const DecimalDigits& value, double expected, bool single) noexcept {
    char text[48];
    const usize length = ComposeScientific(value, text, sizeof(text));
    if (length == MaxSize || length >= sizeof(text)) {
        return false;
    }

    auto parsed = ParseFloatingPoint(StringView(text, length), single);
    return parsed && parsed.Value() == expected;
}

// 유효 숫자 precision개로 올바르게 반올림한 표기다.
bool RoundedDigits(double value, int precision, DecimalDigits& out) noexcept {
    char text[48];
    const usize length = detail::FormatFloatingPoint(value, 'e', precision - 1, text, sizeof(text));
    return length != MaxSize && length < sizeof(text) && SplitScientific(text, length, out);
}

// 마지막 자리에 delta(+1 또는 -1)를 더한다. 자리올림·자리내림으로 자릿수가 바뀌면 지수를 고친다.
// 0에서 빼는 경우는 count 0을 돌려준다.
DecimalDigits AdjustLastDigit(const DecimalDigits& value, int delta) noexcept {
    DecimalDigits result = value;
    int index = result.count - 1;
    if (delta > 0) {
        while (index >= 0 && result.digits[index] == '9') {
            result.digits[index--] = '0';
        }

        if (index >= 0) {
            ++result.digits[index];
        } else {
            // 999 + 1 = 1000: 숫자열 "1"과 지수 + 1.
            result.digits[0] = '1';
            result.count = 1;
            ++result.exponent;
        }

        return result;
    }

    while (index >= 0 && result.digits[index] == '0') {
        result.digits[index--] = '9';
    }

    if (index < 0) {
        result.count = 0;
        return result;
    }

    --result.digits[index];
    if (result.digits[0] == '0') {
        // 1000 - 1 = 0999: 앞의 0을 지우고 지수 - 1.
        for (int shift = 0; shift + 1 < result.count; ++shift) {
            result.digits[shift] = result.digits[shift + 1];
        }

        --result.count;
        --result.exponent;
    }

    return result;
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
    return ParseFloatingPoint(text, false);
}

Result<float> TryParseFloat(StringView text) noexcept {
    auto parsed = ParseFloatingPoint(text, true);
    return parsed ? Result<float>::Success(static_cast<float>(parsed.Value()))
                  : Result<float>::Failure(parsed.Error());
}

namespace detail {

usize FormatFloatingPoint(double value, char type, int precision, char* buffer,
                          usize capacity) noexcept {
    // -Wformat-nonliteral을 피하려고 형식 문자열을 상수로 고른다.
    int count = -1;
    switch (type) {
    case 'f':
        count = snprintf(buffer, capacity, "%.*f", precision, value);
        break;
    case 'e':
        count = snprintf(buffer, capacity, "%.*e", precision, value);
        break;
    case 'E':
        count = snprintf(buffer, capacity, "%.*E", precision, value);
        break;
    case 'g':
        count = snprintf(buffer, capacity, "%.*g", precision, value);
        break;
    case 'G':
        count = snprintf(buffer, capacity, "%.*G", precision, value);
        break;
    default:
        return MaxSize;
    }

    if (count < 0) {
        return MaxSize;
    }

    const usize length = static_cast<usize>(count);
    if (length >= capacity) {
        return length; // 잘렸다. 호출자가 더 큰 buffer로 다시 부른다.
    }

    return NormalizeDecimalPoint(buffer, length);
}

usize FormatShortestFloatingPoint(double value, bool single, char* buffer,
                                  usize capacity) noexcept {
    if (capacity < 32) {
        return MaxSize;
    }

    if (value == 0.0) {
        buffer[0] = '0';
        buffer[1] = '\0';
        return 1;
    }

    // 1) 올바르게 반올림한 p자리 표기가 왕복하는 가장 작은 p를 이진 탐색한다. p자리가 왕복하면 더
    // 긴
    //    표기도 왕복하므로 단조다. 최대 자릿수(float 9, double 17)는 항상 왕복한다.
    const int maximum = single ? 9 : 17;
    int low = 1;
    int high = maximum;
    while (low < high) {
        const int middle = (low + high) / 2;
        DecimalDigits candidate;
        if (!RoundedDigits(value, middle, candidate)) {
            return MaxSize;
        }

        if (RoundTrips(candidate, value, single)) {
            high = middle;
        } else {
            low = middle + 1;
        }
    }

    DecimalDigits best;
    if (!RoundedDigits(value, high, best)) {
        return MaxSize;
    }

    // 2) 2의 거듭제곱 경계에서는 반올림한 값이 아니라 그 이웃(마지막 자리 ±1)이 한 자리 짧게 왕복할
    //    수 있다. 한 자리 짧은 반올림 값의 두 이웃을 검사한다.
    if (high > 1) {
        DecimalDigits shorter;
        if (RoundedDigits(value, high - 1, shorter)) {
            const DecimalDigits up = AdjustLastDigit(shorter, 1);
            const DecimalDigits down = AdjustLastDigit(shorter, -1);
            if (RoundTrips(up, value, single)) {
                best = up;
            } else if (down.count != 0 && RoundTrips(down, value, single)) {
                best = down;
            }
        }
    }

    // 끝의 0은 유효 숫자가 아니다.
    while (best.count > 1 && best.digits[best.count - 1] == '0') {
        --best.count;
    }

    // 3) 지수가 -4 이상 16 미만이면 고정 소수점, 아니면 과학 표기로 조립한다. 찾은 유효 숫자만 쓰고
    //    이진 값을 다시 전개하지 않는다(그래서 1e11f도 "100000000000"이다).
    const int exponent = best.exponent;
    if (exponent < -4 || exponent >= 16) {
        usize length = ComposeScientific(best, buffer, capacity);
        if (length == MaxSize || length + 1 >= capacity) {
            return MaxSize;
        }

        // printf처럼 지수를 최소 두 자리로 쓴다("1e+20", "1e-05").
        usize mark = 0;
        while (buffer[mark] != 'e') {
            ++mark;
        }

        if (length - mark == 3) {
            buffer[length + 1] = '\0';
            buffer[length] = buffer[length - 1];
            buffer[length - 1] = '0';
            ++length;
        }

        return length;
    }

    if (static_cast<usize>(best.count) + 24 >= capacity) {
        return MaxSize;
    }

    usize length = 0;
    if (exponent < 0) {
        // 0.000ddd
        buffer[length++] = '0';
        buffer[length++] = '.';
        for (int zero = 0; zero < -exponent - 1; ++zero) {
            buffer[length++] = '0';
        }

        for (int index = 0; index < best.count; ++index) {
            buffer[length++] = best.digits[index];
        }
    } else {
        // 정수부 exponent + 1자리(모자라면 0으로 채운다), 남는 숫자는 소수부다.
        const int integerDigits = exponent + 1;
        for (int index = 0; index < integerDigits; ++index) {
            buffer[length++] = index < best.count ? best.digits[index] : '0';
        }

        if (best.count > integerDigits) {
            buffer[length++] = '.';
            for (int index = integerDigits; index < best.count; ++index) {
                buffer[length++] = best.digits[index];
            }
        }
    }

    buffer[length] = '\0';
    return length;
}

} // namespace detail

} // namespace rocks
