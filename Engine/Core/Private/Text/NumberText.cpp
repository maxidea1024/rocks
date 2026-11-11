#include "NumberTextInternal.h"

#include <Core/Text/NumberText.h>

#include <errno.h>
#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

namespace rocks {
namespace {

// C locale의 소수점 문자열이다. 대부분 "."이며 일부 locale은 ","다.
const char* LocaleDecimalPoint() noexcept {
    const lconv* conventions = localeconv();
    return conventions != nullptr && conventions->decimal_point != nullptr &&
                   conventions->decimal_point[0] != '\0'
               ? conventions->decimal_point
               : ".";
}

usize CStringLength(const char* text) noexcept {
    usize length = 0;
    while (text[length] != '\0') {
        ++length;
    }
    return length;
}

// printf 결과의 locale 소수점을 '.'로 바꾼다. 길이가 줄 수 있으므로 새 길이를 반환한다.
usize NormalizeDecimalPoint(char* buffer, usize length) noexcept {
    const char* point = LocaleDecimalPoint();
    if (point[0] == '.' && point[1] == '\0') {
        return length;
    }
    const usize pointLength = CStringLength(point);
    for (usize index = 0; index + pointLength <= length; ++index) {
        bool match = true;
        for (usize offset = 0; offset < pointLength && match; ++offset) {
            match = buffer[index + offset] == point[offset];
        }
        if (match) {
            buffer[index] = '.';
            for (usize from = index + pointLength, to = index + 1; from <= length; ++from, ++to) {
                buffer[to] = buffer[from]; // NUL까지 당긴다
            }
            return length - pointLength + 1;
        }
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
    const usize length = CStringLength(word);
    if (bytes != length) {
        return false;
    }
    for (usize index = 0; index < bytes; ++index) {
        if (LowerAscii(text[index]) != word[index]) {
            return false;
        }
    }
    return true;
}

// 엄격한 10진 실수 문법인지 본다. 특수 값(inf·nan)은 따로 처리한다.
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

// 문법 검사를 마친 실수를 strtod/strtof로 읽는다. single이면 float로 읽는다.
Result<double> ParseFloatingPoint(StringView view, bool single) noexcept {
    const char* text = view.Data();
    usize bytes = view.ByteCount();
    if (bytes == 0 || bytes > 1023) {
        return Result<double>::Failure(ErrorCode::InvalidArgument);
    }
    usize start = 0;
    bool negative = false;
    if (text[0] == '+' || text[0] == '-') {
        negative = text[0] == '-';
        start = 1;
    }
    if (EqualsIgnoreAsciiCase(text + start, bytes - start, "inf") ||
        EqualsIgnoreAsciiCase(text + start, bytes - start, "infinity")) {
        return Result<double>::Success(negative ? -HUGE_VAL : HUGE_VAL);
    }
    if (EqualsIgnoreAsciiCase(text + start, bytes - start, "nan")) {
        const double nan = static_cast<double>(NAN);
        return Result<double>::Success(negative ? -nan : nan);
    }
    if (!IsDecimalFloatingPoint(text, bytes)) {
        return Result<double>::Failure(ErrorCode::InvalidArgument);
    }
    // '.'를 현재 locale의 소수점으로 바꾼 NUL 종료 사본을 만든다.
    char buffer[1024 + 8];
    const char* point = LocaleDecimalPoint();
    const usize pointLength = CStringLength(point);
    usize written = 0;
    for (usize index = 0; index < bytes; ++index) {
        if (text[index] == '.') {
            if (written + pointLength >= sizeof(buffer)) {
                return Result<double>::Failure(ErrorCode::InvalidArgument);
            }
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
    double value = 0.0;
    if (single) {
        value = static_cast<double>(strtof(buffer, &end));
    } else {
        value = strtod(buffer, &end);
    }
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

// 부호를 뺀 자릿수를 읽는다. 크기 초과는 Overflow, 문법 오류는 InvalidArgument다.
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
        // 넘친 뒤에도 끝까지 문법을 검사한다.
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
    // 1) 다시 읽어 같은 값이 되는 가장 적은 유효 자릿수를 과학 표기로 찾는다(최대 정밀도는 항상
    //    왕복한다).
    const int maximum = single ? 9 : 17;
    int digits = maximum;
    usize length = 0;
    for (int precision = 1; precision <= maximum; ++precision) {
        length = FormatFloatingPoint(value, 'e', precision - 1, buffer, capacity);
        if (length == MaxSize || length >= capacity) {
            return MaxSize;
        }
        auto parsed = ParseFloatingPoint(StringView(buffer, length), single);
        if (parsed && parsed.Value() == value) {
            digits = precision;
            break;
        }
    }
    // 2) 첫 유효 숫자의 10진 지수가 -4 이상 16 미만이면 같은 자릿수의 고정 소수점으로 쓴다
    //    (Python repr·fmt와 같은 규칙). 그 밖은 과학 표기를 그대로 쓴다.
    int exponent = 0;
    usize cursor = 0;
    while (cursor < length && buffer[cursor] != 'e') {
        ++cursor;
    }
    bool negativeExponent = false;
    if (++cursor < length && (buffer[cursor] == '-' || buffer[cursor] == '+')) {
        negativeExponent = buffer[cursor++] == '-';
    }
    while (cursor < length && buffer[cursor] >= '0' && buffer[cursor] <= '9') {
        exponent = exponent * 10 + (buffer[cursor++] - '0');
    }
    if (negativeExponent) {
        exponent = -exponent;
    }
    if (exponent < -4 || exponent >= 16) {
        return length;
    }
    const int decimals = digits - 1 - exponent > 0 ? digits - 1 - exponent : 0;
    length = FormatFloatingPoint(value, 'f', decimals, buffer, capacity);
    return length == MaxSize || length >= capacity ? MaxSize : length;
}

} // namespace detail

} // namespace rocks
