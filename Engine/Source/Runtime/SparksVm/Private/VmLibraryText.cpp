#include <Core/Math/MathUtility.h>
#include <Core/Text/NumberText.h>
#include <Core/Text/StringBuilder.h>
#include <Platform/Memory.h>

#include "VmArithmetic.h"
#include "VmLibrary.h"

// 문자열·Format·Parse·Log·Assert 언어 함수다(Docs/Design/SparksVm.ko.md 6a절, SL 부록 B). 문자열은
// UTF-8 byte 열이고 위치·길이는 byte 단위다(`CharCount`만 code point).

namespace rocks::sparks::detail {

namespace {

using T = VmLibraryContext;

// `Format.Fixed`의 소수 자리 상한과 `Format.Padded`의 폭 상한이다.
constexpr int64 VmFixedDecimalsMax = 9;
constexpr int64 VmPaddedWidthMax = 32;

// 서식 inline 크기다.
constexpr usize VmTextInlineBytes = 128;

bool VmContinuation(char c) noexcept {
    return (static_cast<uint8>(c) & 0xC0) == 0x80;
}

bool VmAsciiSpace(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

StringView Text(const T& call, uint32 operand) noexcept {
    return VmStringOf(call.Buffer(operand));
}

// 새 문자열을 Buf 창 0번에 둔다(수신자 읽기가 끝난 뒤에 부른다).
VmFaultKind PutString(T& call, StringView text) noexcept {
    VmFaultKind fault = VmFaultKind::None;
    VmBuffer* buffer = VmLibrary::NewString(call, text, fault);
    if (buffer == nullptr) {
        return fault;
    }

    VmLibrary::Put(call, 0, buffer);
    return VmFaultKind::None;
}

// Scalar 선택 값(있음 표지 + 값 하나)을 둔다.
void PutOptional(T& call, bool present, uint64 value) noexcept {
    call.s[0] = present ? 1 : 0;
    call.s[1] = present ? value : 0;
}

usize Find(StringView text, StringView needle) noexcept {
    if (needle.ByteCount() > text.ByteCount()) {
        return NumericLimits<usize>::Max();
    }

    for (usize at = 0; at + needle.ByteCount() <= text.ByteCount(); ++at) {
        if (StringView(text.Data() + at, needle.ByteCount()) == needle) {
            return at;
        }
    }

    return NumericLimits<usize>::Max();
}

VmFaultKind StringLength(T& call) noexcept {
    call.s[0] = Text(call, 0).ByteCount();
    return VmFaultKind::None;
}

VmFaultKind StringCharCount(T& call) noexcept {
    const StringView text = Text(call, 0);
    uint64 count = 0;
    for (usize at = 0; at < text.ByteCount(); ++at) {
        count += VmContinuation(text.Data()[at]) ? 0 : 1;
    }

    call.s[0] = count;
    return VmFaultKind::None;
}

VmFaultKind StringContains(T& call) noexcept {
    call.s[0] = Find(Text(call, 0), Text(call, 1)) != NumericLimits<usize>::Max() ? 1 : 0;
    return VmFaultKind::None;
}

VmFaultKind StringStartsWith(T& call) noexcept {
    call.s[0] = Text(call, 0).StartsWith(Text(call, 1)) ? 1 : 0;
    return VmFaultKind::None;
}

VmFaultKind StringEndsWith(T& call) noexcept {
    call.s[0] = Text(call, 0).EndsWith(Text(call, 1)) ? 1 : 0;
    return VmFaultKind::None;
}

VmFaultKind StringFind(T& call) noexcept {
    const usize at = Find(Text(call, 0), Text(call, 1));
    PutOptional(call, at != NumericLimits<usize>::Max(), at);
    return VmFaultKind::None;
}

VmFaultKind StringSlice(T& call) noexcept {
    // [start, end) byte 범위. 범위 밖이나 code point 경계가 아니면 IndexOutOfRange(부록 B).
    const StringView text = Text(call, 0);
    const int64 start = static_cast<int64>(call.Scalars(1)[0]);
    const int64 end = static_cast<int64>(call.Scalars(2)[0]);
    const int64 length = static_cast<int64>(text.ByteCount());
    if (start < 0 || end < start || end > length ||
        (start < length && VmContinuation(text.Data()[start])) ||
        (end < length && VmContinuation(text.Data()[end]))) {
        return VmFaultKind::IndexOutOfRange;
    }

    return PutString(call, StringView(text.Data() + start, static_cast<usize>(end - start)));
}

VmFaultKind StringTrim(T& call) noexcept {
    const StringView text = Text(call, 0);
    usize begin = 0;
    usize end = text.ByteCount();
    while (begin < end && VmAsciiSpace(text.Data()[begin])) {
        ++begin;
    }

    while (end > begin && VmAsciiSpace(text.Data()[end - 1])) {
        --end;
    }

    return PutString(call, StringView(text.Data() + begin, end - begin));
}

VmFaultKind StringCase(T& call, bool upper) noexcept {
    const StringView text = Text(call, 0);
    VmFaultKind fault = VmFaultKind::None;
    VmBuffer* buffer = VmLibrary::NewString(call, text, fault);
    if (buffer == nullptr) {
        return fault;
    }

    char* bytes = buffer->Bytes();
    for (usize at = 0; at < text.ByteCount(); ++at) {
        const char c = bytes[at];
        if (upper && c >= 'a' && c <= 'z') {
            bytes[at] = static_cast<char>(c - 'a' + 'A');
        } else if (!upper && c >= 'A' && c <= 'Z') {
            bytes[at] = static_cast<char>(c - 'A' + 'a');
        }
    }

    VmLibrary::Put(call, 0, buffer);
    return VmFaultKind::None;
}

VmFaultKind StringUpper(T& call) noexcept {
    return StringCase(call, true);
}

VmFaultKind StringLower(T& call) noexcept {
    return StringCase(call, false);
}

VmFaultKind StringSplit(T& call) noexcept {
    // 빈 구분자는 code point마다 나눈다(Go strings.Split과 같다). 빈 조각도 남긴다.
    const StringView text = Text(call, 0);
    const StringView separator = Text(call, 1);
    uint64 pieces = 0;
    if (separator.IsEmpty()) {
        // 채우는 규칙과 같게 센다: 첫 byte는 늘 조각을 시작한다(continuation byte로 시작하는 host
        // 문자열도 조각 하나 — 리뷰 A4).
        pieces = text.IsEmpty() ? 0 : 1;
        for (usize at = 1; at < text.ByteCount(); ++at) {
            pieces += VmContinuation(text.Data()[at]) ? 0 : 1;
        }
    } else {
        pieces = 1;
        for (usize at = 0; at + separator.ByteCount() <= text.ByteCount();) {
            if (StringView(text.Data() + at, separator.ByteCount()) == separator) {
                ++pieces;
                at += separator.ByteCount();
            } else {
                ++at;
            }
        }
    }

    VmFaultKind fault = VmFaultKind::None;
    VmBuffer* array = VmLibrary::Allocate(call, VmBufferKind::Array, pieces, sizeof(uint64), fault);
    if (array == nullptr) {
        return fault;
    }

    array->valueBank = static_cast<uint8>(BytecodeBank::Buf);
    array->valueSlots = 1;
    array->count = 0; // 채운 만큼만 센다(중간 실패 때 놓을 수 있게)
    uint64* slots = reinterpret_cast<uint64*>(array->Bytes());
    auto add = [&](StringView piece) noexcept {
        VmBuffer* item = VmLibrary::NewString(call, piece, fault);
        if (item == nullptr) {
            return false;
        }

        slots[array->count++] = static_cast<uint64>(reinterpret_cast<usize>(item));
        return true;
    };
    bool ok = true;
    if (separator.IsEmpty()) {
        usize begin = 0;
        for (usize at = 1; ok && at <= text.ByteCount(); ++at) {
            if (at == text.ByteCount() || !VmContinuation(text.Data()[at])) {
                ok = add(StringView(text.Data() + begin, at - begin));
                begin = at;
            }
        }
    } else {
        usize begin = 0;
        usize at = 0;
        while (ok && at + separator.ByteCount() <= text.ByteCount()) {
            if (StringView(text.Data() + at, separator.ByteCount()) == separator) {
                ok = add(StringView(text.Data() + begin, at - begin));
                at += separator.ByteCount();
                begin = at;
            } else {
                ++at;
            }
        }

        ok = ok && add(StringView(text.Data() + begin, text.ByteCount() - begin));
    }

    if (!ok) {
        VmLibrary::Release(call, array);
        return fault;
    }

    VmLibrary::Put(call, 0, array);
    return VmFaultKind::None;
}

// ---- Format·Parse ----

VmFaultKind FormatFixed(T& call) noexcept {
    const double value = call.Real(0);
    const int64 decimals = call.Integer(1);
    if (decimals < 0 || decimals > VmFixedDecimalsMax) {
        return VmFaultKind::AssertFailed; // 부록 B: decimals는 0–9
    }

    StringBuilder<VmTextInlineBytes> spec(VmLibrary::AllocatorOf(call));
    spec.AppendFormat("{{:.{}f}}", decimals);
    StringBuilder<VmTextInlineBytes> text(VmLibrary::AllocatorOf(call));
    text.AppendFormat(spec.View(), value);
    if (spec.HasFailed() || text.HasFailed()) {
        call.error = ErrorCode::OutOfMemory;
        return VmFaultKind::None;
    }

    return PutString(call, text.View());
}

VmFaultKind FormatPadded(T& call) noexcept {
    // 앞을 0으로 채워 폭(부호 포함)을 맞춘다. 폭은 0–32, 밖이면 AssertFailed.
    const int64 value = call.Integer(0);
    const int64 width = call.Integer(1);
    if (width < 0 || width > VmPaddedWidthMax) {
        return VmFaultKind::AssertFailed;
    }

    char digits[VmTextInlineBytes] = {};
    usize count = 0;
    uint64 magnitude = value < 0 ? uint64(0) - uint64(value) : uint64(value);
    do {
        digits[count++] = static_cast<char>('0' + magnitude % 10);
        magnitude /= 10;
    } while (magnitude != 0);

    char out[VmTextInlineBytes] = {};
    usize length = 0;
    if (value < 0) {
        out[length++] = '-';
    }

    const usize used = count + (value < 0 ? 1 : 0);
    for (usize pad = used; pad < static_cast<usize>(width); ++pad) {
        out[length++] = '0';
    }

    while (count > 0) {
        out[length++] = digits[--count];
    }

    return PutString(call, StringView(out, length));
}

// text[at]부터 10진 숫자를 건너뛴다(건넌 수를 돌려준다).
usize SkipDigits(StringView text, usize& at) noexcept {
    const usize begin = at;
    while (at < text.ByteCount() && text.Data()[at] >= '0' && text.Data()[at] <= '9') {
        ++at;
    }

    return at - begin;
}

// `Parse` 문법인지다(SparksVm 6a절): 부호(`+`·`-`) 하나 선택, 10진 숫자(`_` 없음). fraction이면
// 실수 — `숫자 [. 숫자] | . 숫자` 뒤 지수(`e`·`E`, 부호 선택, 숫자) 선택. `1.`·`inf`·`nan`·16진은
// 아니다(Core 파서가 더 넓어서 먼저 거른다).
bool ParseGrammar(StringView text, bool fraction) noexcept {
    usize at = 0;
    if (at < text.ByteCount() && (text.Data()[at] == '+' || text.Data()[at] == '-')) {
        ++at;
    }

    usize digits = SkipDigits(text, at);
    if (fraction && at < text.ByteCount() && text.Data()[at] == '.') {
        ++at;
        if (SkipDigits(text, at) == 0) {
            return false;
        }

        digits = 1;
    }

    if (digits == 0) {
        return false;
    }

    if (fraction && at < text.ByteCount() && (text.Data()[at] == 'e' || text.Data()[at] == 'E')) {
        ++at;
        if (at < text.ByteCount() && (text.Data()[at] == '+' || text.Data()[at] == '-')) {
            ++at;
        }

        if (SkipDigits(text, at) == 0) {
            return false;
        }
    }

    return at == text.ByteCount();
}

VmFaultKind ParseInt(T& call) noexcept {
    const StringView text = Text(call, 0);
    auto value = ParseGrammar(text, false) ? TryParseInt64(text)
                                           : Result<int64>::Failure(ErrorCode::InvalidArgument);
    PutOptional(call, static_cast<bool>(value), value ? uint64(value.Value()) : 0);
    return VmFaultKind::None;
}

VmFaultKind ParseFloat(T& call) noexcept {
    // 유한한 값만(범위를 넘는 값은 없음).
    const StringView text = Text(call, 0);
    auto value = ParseGrammar(text, true) ? TryParseDouble(text)
                                          : Result<double>::Failure(ErrorCode::InvalidArgument);
    const bool present = value && math::IsFinite(value.Value());
    PutOptional(call, present, present ? VmBits(value.Value()) : 0);
    return VmFaultKind::None;
}

// ---- Log·Assert ----

VmFaultKind LogAt(T& call, VmLogLevel level) noexcept {
    VmLibrary::Log(call, level, Text(call, 0));
    return VmFaultKind::None;
}

VmFaultKind LogInfo(T& call) noexcept {
    return LogAt(call, VmLogLevel::Info);
}

VmFaultKind LogWarning(T& call) noexcept {
    return LogAt(call, VmLogLevel::Warning);
}

VmFaultKind LogError(T& call) noexcept {
    return LogAt(call, VmLogLevel::Error);
}

VmFaultKind LogDebug(T& call) noexcept {
    return LogAt(call, VmLogLevel::Debug);
}

VmFaultKind LanguageAssert(T& call) noexcept {
    // 거짓이면 AssertFailed(모든 빌드 — 부록 B). message는 오류 기록으로 남긴다.
    if (call.Scalars(0)[0] != 0) {
        return VmFaultKind::None;
    }

    VmLibrary::Log(call, VmLogLevel::Error, Text(call, 1));
    return VmFaultKind::AssertFailed;
}

constexpr VmLibraryEntry VmTextTable[] = {
    {"string.Length", "s>i64", StringLength},
    {"string.CharCount", "s>i64", StringCharCount},
    {"string.Contains", "s,s>b", StringContains},
    {"string.StartsWith", "s,s>b", StringStartsWith},
    {"string.EndsWith", "s,s>b", StringEndsWith},
    {"string.Find", "s,s>?<i64>", StringFind},
    {"string.Slice", "s,i64,i64>s", StringSlice},
    {"string.Split", "s,s>A<s>", StringSplit},
    {"string.Trim", "s>s", StringTrim},
    {"string.ToUpperAscii", "s>s", StringUpper},
    {"string.ToLowerAscii", "s>s", StringLower},
    {"Format.Fixed", "F,i64>s", FormatFixed},
    {"Format.Padded", "i64,i64>s", FormatPadded},
    {"Parse.Int", "s>?<i64>", ParseInt},
    {"Parse.Float", "s>?<F>", ParseFloat},
    {"Log.Info", "s>void", LogInfo},
    {"Log.Warning", "s>void", LogWarning},
    {"Log.Error", "s>void", LogError},
    {"Log.Debug", "s>void", LogDebug},
    {"Assert", "b,s>void", LanguageAssert},
};

} // namespace

Span<const VmLibraryEntry> VmTextEntries() noexcept {
    return Span<const VmLibraryEntry>(VmTextTable, sizeof(VmTextTable) / sizeof(VmTextTable[0]));
}

} // namespace rocks::sparks::detail
