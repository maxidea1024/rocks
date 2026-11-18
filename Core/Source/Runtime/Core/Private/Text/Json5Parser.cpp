#include <Core/Text/Json.h>

#include "JsonParser.h"
#include <Base/NumericLimits.h>
#include <Core/Text/Unicode.h>
#include <Platform/Memory.h>

// JSON5 1.0.0(https://spec.json5.org/) 어휘다(JsonSyntax::Json5). 규칙과 Rocks 정책 차이는 Json.h의
// JsonReadOptions::syntax 주석에 있다. C# Json5Reader(Programs/Rocks.Tooling.Common)가 같은 규칙을
// 같은 순서로 구현한다 — 여기를 바꾸면 그쪽과 공용 대조 사례(Tests/Json5ParityCases.h)도 함께
// 바꾼다.
namespace rocks {

namespace {

constexpr uint32 NoBreakSpace = 0xA0;
constexpr uint32 ZeroWidthNonJoiner = 0x200C;
constexpr uint32 ZeroWidthJoiner = 0x200D;
constexpr uint32 LineSeparator = 0x2028;
constexpr uint32 ParagraphSeparator = 0x2029;
constexpr uint32 ByteOrderMark = 0xFEFF;

// 양의 무한대·음의 무한대·NaN의 double 비트다. NaN은 부호와 무관하게 이 한 값(양의 quiet NaN)으로
// 저장한다(JsonEquals가 NaN끼리 같다고 보고, 문서 비교가 비트에 좌우되지 않게).
constexpr uint64 PositiveInfinityBits = 0x7FF0000000000000ull;
constexpr uint64 NegativeInfinityBits = 0xFFF0000000000000ull;
constexpr uint64 QuietNanBits = 0x7FF8000000000000ull;

bool IsDigit(uint8 byte) noexcept {
    return byte >= '0' && byte <= '9';
}

// ASCII 밖 JSON5 공백: NBSP·BOM·LS·PS와 General_Category Zs(ASCII 공백은 호출자가 먼저 본다).
bool IsJson5Space(uint32 codePoint) noexcept {
    return codePoint == NoBreakSpace || codePoint == ByteOrderMark || codePoint == LineSeparator ||
           codePoint == ParagraphSeparator ||
           GetGeneralCategory(static_cast<char32_t>(codePoint)) == GeneralCategory::Zs;
}

// ECMAScript 5.1 IdentifierStart(이스케이프 제외): UnicodeLetter(Lu Ll Lt Lm Lo Nl)·'$'·'_'.
bool IsIdentifierStart(uint32 codePoint) noexcept {
    if (codePoint == '$' || codePoint == '_') {
        return true;
    }

    switch (GetGeneralCategory(static_cast<char32_t>(codePoint))) {
    case GeneralCategory::Lu:
    case GeneralCategory::Ll:
    case GeneralCategory::Lt:
    case GeneralCategory::Lm:
    case GeneralCategory::Lo:
    case GeneralCategory::Nl:
        return true;
    default:
        return false;
    }
}

// ECMAScript 5.1 IdentifierPart(이스케이프 제외): IdentifierStart·Mn·Mc·Nd·Pc·ZWNJ·ZWJ.
bool IsIdentifierPart(uint32 codePoint) noexcept {
    if (IsIdentifierStart(codePoint) || codePoint == ZeroWidthNonJoiner ||
        codePoint == ZeroWidthJoiner) {
        return true;
    }

    switch (GetGeneralCategory(static_cast<char32_t>(codePoint))) {
    case GeneralCategory::Mn:
    case GeneralCategory::Mc:
    case GeneralCategory::Nd:
    case GeneralCategory::Pc:
        return true;
    default:
        return false;
    }
}

// codePoint의 UTF-8 byte 수다.
usize Utf8EncodedLength(uint32 codePoint) noexcept {
    if (codePoint < 0x80) {
        return 1;
    }

    if (codePoint < 0x800) {
        return 2;
    }

    return codePoint < 0x10000 ? 3 : 4;
}

} // namespace

void JsonParser::SkipJson5Whitespace() noexcept {
    while (position_ < size_) {
        const uint8 byte = data_[position_];
        if (byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r' || byte == '\v' ||
            byte == '\f') {
            ++position_;
            continue;
        }

        if (byte == '/' && position_ + 1 < size_ &&
            (data_[position_ + 1] == '/' || data_[position_ + 1] == '*')) {
            const bool block = data_[position_ + 1] == '*';
            position_ += 2;
            if (!SkipJson5Comment(block)) {
                return;
            }

            continue;
        }

        // 짝 없는 '/'와 그 밖의 ASCII는 다음 토큰 해석이 판정한다.
        if (byte < 0x80) {
            return;
        }

        uint32 codePoint = 0;
        const usize count = detail::DecodeUtf8(data_, size_, position_, codePoint);
        if (count == 0) {
            static_cast<void>(Fail(JsonErrorCode::InvalidUtf8, position_));
            return;
        }

        if (!IsJson5Space(codePoint)) {
            return;
        }

        position_ += count;
    }
}

bool JsonParser::SkipJson5Comment(bool block) noexcept {
    while (true) {
        if (position_ >= size_) {
            // 줄 주석은 입력 끝에서 끝나고, 블록 주석은 닫히지 않았다.
            return block ? Fail(JsonErrorCode::UnexpectedEnd, size_) : true;
        }

        const uint8 byte = data_[position_];
        if (block && byte == '*' && position_ + 1 < size_ && data_[position_ + 1] == '/') {
            position_ += 2;
            return true;
        }

        // 줄 주석은 줄 끝(LF·CR·LS·PS) 앞에서 끝난다. 줄 끝은 공백으로 소비한다.
        if (!block && (byte == '\n' || byte == '\r')) {
            return true;
        }

        if (byte < 0x80) {
            ++position_;
            continue;
        }

        uint32 codePoint = 0;
        const usize count = detail::DecodeUtf8(data_, size_, position_, codePoint);
        if (count == 0) {
            return Fail(JsonErrorCode::InvalidUtf8, position_);
        }

        if (!block && (codePoint == LineSeparator || codePoint == ParagraphSeparator)) {
            return true;
        }

        position_ += count;
    }
}

bool JsonParser::IsIdentifierStartAt(usize at) const noexcept {
    if (at >= size_) {
        return false;
    }

    const uint8 byte = data_[at];
    if (byte == '\\') {
        return true;
    }

    if (byte < 0x80) {
        return IsIdentifierStart(byte);
    }

    uint32 codePoint = 0;
    return detail::DecodeUtf8(data_, size_, at, codePoint) != 0 && IsIdentifierStart(codePoint);
}

bool JsonParser::IsIdentifierContinuation(usize at) const noexcept {
    if (at >= size_) {
        return false;
    }

    const uint8 byte = data_[at];
    if (byte == '\\') {
        return true;
    }

    if (byte < 0x80) {
        return IsIdentifierPart(byte);
    }

    uint32 codePoint = 0;
    return detail::DecodeUtf8(data_, size_, at, codePoint) != 0 && IsIdentifierPart(codePoint);
}

JsonParser::WordMatch JsonParser::MatchWord(usize at, const char* word,
                                            usize length) const noexcept {
    const usize available = size_ - at;
    if (available < length) {
        return platform::MemoryCompare(data_ + at, word, available) == 0 ? WordMatch::Truncated
                                                                         : WordMatch::Mismatch;
    }

    if (platform::MemoryCompare(data_ + at, word, length) != 0) {
        return WordMatch::Mismatch;
    }

    return IsIdentifierContinuation(at + length) ? WordMatch::Mismatch : WordMatch::Match;
}

bool JsonParser::ParseJson5Value(uint32& node) noexcept {
    const usize start = position_;
    const uint8 byte = data_[position_];
    if (byte == '"' || byte == '\'') {
        Node value{};
        value.type = JsonType::String;
        return ParseJson5String(value.first, value.count) && AddNode(value, node, start);
    }

    if (byte == '+' || byte == '-' || byte == '.' || IsDigit(byte) || byte == 'I' || byte == 'N') {
        return ParseJson5Number(node);
    }

    return ParseJson5Word(node);
}

bool JsonParser::ParseJson5String(uint32& offset, uint32& length) noexcept {
    // position_은 여는 따옴표('"' 또는 '\'')다. 닫는 따옴표는 같은 종류다.
    const usize start = position_;
    const uint8 quote = data_[position_];
    ++position_;
    const usize textStart = document_.text_.Size();
    offset = static_cast<uint32>(textStart);
    while (true) {
        // 따옴표·이스케이프·줄바꿈·비 ASCII가 아닌 구간은 한 번에 붙인다. LF·CR 밖의 제어 문자는
        // JSON5에서 문자열에 그대로 올 수 있다.
        usize run = position_;
        while (run < size_ && data_[run] != quote && data_[run] != '\\' && data_[run] != '\n' &&
               data_[run] != '\r' && data_[run] < 0x80) {
            ++run;
        }

        if (run != position_) {
            if (!WithinStringLimit(textStart, run - position_)) {
                return Fail(JsonErrorCode::StringTooLong, start);
            }

            if (!AppendText(data_ + position_, run - position_)) {
                return false;
            }

            position_ = run;
            continue;
        }

        if (position_ >= size_) {
            return Fail(JsonErrorCode::UnexpectedEnd, position_);
        }

        const uint8 byte = data_[position_];
        if (byte == quote) {
            ++position_;
            break;
        }

        if (byte == '\n' || byte == '\r') {
            return Fail(JsonErrorCode::ControlCharacter, position_);
        }

        if (byte >= 0x80) {
            // LS·PS도 문자열 안에서는 그대로 글자다(JSON5 명세).
            uint32 codePoint = 0;
            const usize count = detail::DecodeUtf8(data_, size_, position_, codePoint);
            if (count == 0) {
                return Fail(JsonErrorCode::InvalidUtf8, position_);
            }

            if (!WithinStringLimit(textStart, count)) {
                return Fail(JsonErrorCode::StringTooLong, start);
            }

            if (!AppendText(data_ + position_, count)) {
                return false;
            }

            position_ += count;
            continue;
        }

        if (!ParseJson5Escape(start, textStart)) {
            return false;
        }
    }

    length = static_cast<uint32>(document_.text_.Size() - textStart);
    return true;
}

bool JsonParser::ParseJson5Escape(usize stringStart, usize textStart) noexcept {
    const usize escape = position_;
    if (position_ + 1 >= size_) {
        return Fail(JsonErrorCode::UnexpectedEnd, size_);
    }

    const uint8 kind = data_[position_ + 1];
    position_ += 2;
    char simple = 0;
    switch (kind) {
    case '\'':
    case '"':
    case '\\':
        simple = static_cast<char>(kind);
        break;
    case 'b':
        simple = '\b';
        break;
    case 'f':
        simple = '\f';
        break;
    case 'n':
        simple = '\n';
        break;
    case 'r':
        simple = '\r';
        break;
    case 't':
        simple = '\t';
        break;
    case 'v':
        simple = '\v';
        break;
    case '0':
        // \0 뒤에 10진 숫자가 오면 8진 이스케이프라 받지 않는다.
        if (position_ < size_ && IsDigit(data_[position_])) {
            return Fail(JsonErrorCode::InvalidEscape, escape);
        }

        simple = '\0';
        break;
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7':
    case '8':
    case '9':
        return Fail(JsonErrorCode::InvalidEscape, escape);
    case 'x': {
        uint32 value = 0;
        for (usize index = 0; index < 2; ++index) {
            if (position_ + index >= size_) {
                return Fail(JsonErrorCode::UnexpectedEnd, size_);
            }

            const int digit = detail::HexDigitValue(data_[position_ + index]);
            if (digit < 0) {
                return Fail(JsonErrorCode::InvalidEscape, escape);
            }

            value = (value << 4) | static_cast<uint32>(digit);
        }

        position_ += 2;
        if (!WithinStringLimit(textStart, Utf8EncodedLength(value))) {
            return Fail(JsonErrorCode::StringTooLong, stringStart);
        }

        return AppendCodePoint(value);
    }
    case 'u': {
        // 엄격 문법과 같다(짝 없는 surrogate는 UnpairedSurrogate, 한도는 4 byte로 미리 본다).
        uint32 codePoint = 0;
        if (!ReadUnicodeEscape(escape, codePoint)) {
            return false;
        }

        if (!WithinStringLimit(textStart, 4)) {
            return Fail(JsonErrorCode::StringTooLong, stringStart);
        }

        return AppendCodePoint(codePoint);
    }
    case '\n':
        return true; // 줄 이음: 백슬래시와 줄 끝은 문자열에 들어가지 않는다
    case '\r':
        if (position_ < size_ && data_[position_] == '\n') {
            ++position_;
        }

        return true;
    default:
        if (kind < 0x80) {
            simple = static_cast<char>(kind); // 그 밖의 글자 이스케이프는 그 글자 자체다
            break;
        }

        {
            uint32 codePoint = 0;
            const usize count = detail::DecodeUtf8(data_, size_, escape + 1, codePoint);
            if (count == 0) {
                return Fail(JsonErrorCode::InvalidUtf8, escape + 1);
            }

            position_ = escape + 1 + count;
            if (codePoint == LineSeparator || codePoint == ParagraphSeparator) {
                return true; // LS·PS 줄 이음
            }

            if (!WithinStringLimit(textStart, count)) {
                return Fail(JsonErrorCode::StringTooLong, stringStart);
            }

            return AppendText(data_ + escape + 1, count);
        }
    }

    if (!WithinStringLimit(textStart, 1)) {
        return Fail(JsonErrorCode::StringTooLong, stringStart);
    }

    return AppendText(&simple, 1);
}

bool JsonParser::ParseJson5Identifier(uint32& offset, uint32& length) noexcept {
    // ECMAScript 5.1 IdentifierName. 이스케이프는 \uXXXX 하나씩이고, 푼 글자도 그 자리의 식별자
    // 글자여야 한다(surrogate 이스케이프는 Cs라 받지 않는다 — 참조 구현 json5와 같다).
    const usize start = position_;
    const usize textStart = document_.text_.Size();
    offset = static_cast<uint32>(textStart);
    bool first = true;
    while (position_ < size_) {
        const usize at = position_;
        const uint8 byte = data_[at];
        uint32 codePoint = 0;
        if (byte == '\\') {
            if (at + 1 >= size_) {
                return Fail(JsonErrorCode::UnexpectedEnd, size_);
            }

            if (data_[at + 1] != 'u') {
                return Fail(JsonErrorCode::InvalidEscape, at);
            }

            position_ = at + 2;
            if (!ReadHex(at, codePoint)) {
                return false;
            }

            if (!(first ? IsIdentifierStart(codePoint) : IsIdentifierPart(codePoint))) {
                return Fail(JsonErrorCode::InvalidEscape, at);
            }
        } else {
            usize count = 1;
            codePoint = byte;
            if (byte >= 0x80) {
                count = detail::DecodeUtf8(data_, size_, at, codePoint);
                if (count == 0) {
                    return Fail(JsonErrorCode::InvalidUtf8, at);
                }
            }

            if (!(first ? IsIdentifierStart(codePoint) : IsIdentifierPart(codePoint))) {
                if (first) {
                    return Fail(JsonErrorCode::UnexpectedCharacter, at);
                }

                break;
            }

            position_ = at + count;
        }

        if (!WithinStringLimit(textStart, Utf8EncodedLength(codePoint))) {
            return Fail(JsonErrorCode::StringTooLong, start);
        }

        if (!AppendCodePoint(codePoint)) {
            return false;
        }

        first = false;
    }

    length = static_cast<uint32>(document_.text_.Size() - textStart);
    return true;
}

bool JsonParser::ParseJson5Number(uint32& node) noexcept {
    // [+-]?(Infinity | NaN | 0[xX]hex+ | (0|[1-9]digit*)(.digit*)?exponent? | .digit+ exponent?).
    // 원문 숫자 토큰(RawNumber)은 엄격 문법이 같은 값·종류로 읽는 RFC 8259 꼴로 바꿔 둔다.
    const usize start = position_;
    usize index = position_;
    bool negative = false;
    if (data_[index] == '+' || data_[index] == '-') {
        negative = data_[index] == '-';
        ++index;
    }

    if (index >= size_) {
        return Fail(JsonErrorCode::UnexpectedEnd, index);
    }

    const uint8 lead = data_[index];
    if (lead == 'I' || lead == 'N') {
        const bool infinity = lead == 'I';
        const char* word = infinity ? "Infinity" : "NaN";
        const usize wordLength = infinity ? 8 : 3;
        switch (MatchWord(index, word, wordLength)) {
        case WordMatch::Truncated:
            return Fail(JsonErrorCode::UnexpectedEnd, size_);
        case WordMatch::Mismatch:
            return Fail(JsonErrorCode::InvalidLiteral, index);
        case WordMatch::Match:
            break;
        }

        position_ = index + wordLength;
        // 토큰: 앞 '+'는 버리고, NaN은 부호 없이 "NaN" 하나다.
        const StringView token = infinity
                                     ? (negative ? StringView("-Infinity") : StringView("Infinity"))
                                     : StringView("NaN");
        Node value{};
        value.type = JsonType::Double;
        value.first = static_cast<uint32>(document_.text_.Size());
        value.count = static_cast<uint32>(token.ByteCount());
        value.bits =
            infinity ? (negative ? NegativeInfinityBits : PositiveInfinityBits) : QuietNanBits;
        return AppendText(token.Data(), token.ByteCount()) && AddNode(value, node, start);
    }

    const bool hex =
        lead == '0' && index + 1 < size_ && (data_[index + 1] == 'x' || data_[index + 1] == 'X');
    if (hex) {
        index += 2;
        const usize digitsStart = index;
        uint64 magnitude = 0;
        bool overflow = false;
        for (; index < size_; ++index) {
            const int digit = detail::HexDigitValue(data_[index]);
            if (digit < 0) {
                break;
            }

            if (magnitude > (NumericLimits<uint64>::Max() >> 4)) {
                overflow = true;
            }

            magnitude = (magnitude << 4) | static_cast<uint64>(digit);
        }

        if (index == digitsStart) {
            return Fail(index >= size_ ? JsonErrorCode::UnexpectedEnd
                                       : JsonErrorCode::InvalidNumber,
                        index);
        }

        if (IsIdentifierContinuation(index)) {
            return Fail(JsonErrorCode::InvalidNumber, index);
        }

        // 16진수는 정수 전용이다. 64비트 크기를 넘으면 반올림하지 않고 거부한다(Rocks 정책).
        if (overflow) {
            return Fail(JsonErrorCode::NumberOutOfRange, start);
        }

        position_ = index;
        char digits[24];
        usize count = 0;
        do {
            digits[count++] = static_cast<char>('0' + magnitude % 10);
            magnitude /= 10;
        } while (magnitude != 0);

        const usize textStart = document_.text_.Size();
        if (negative && !AppendText("-", 1)) {
            return false;
        }

        while (count != 0) {
            if (!AppendText(&digits[--count], 1)) {
                return false;
            }
        }

        return AddNumberNode(textStart, true, start, node);
    }

    const usize integerStart = index;
    if (lead == '0') {
        ++index;
        if (index < size_ && IsDigit(data_[index])) {
            return Fail(JsonErrorCode::InvalidNumber, index); // 앞자리 0(8진 꼴)
        }
    } else {
        while (index < size_ && IsDigit(data_[index])) {
            ++index;
        }
    }

    const usize integerEnd = index;
    bool integer = true;
    bool point = false;
    usize fractionStart = index;
    usize fractionEnd = index;
    if (index < size_ && data_[index] == '.') {
        integer = false;
        point = true;
        ++index;
        fractionStart = index;
        while (index < size_ && IsDigit(data_[index])) {
            ++index;
        }

        fractionEnd = index;
    }

    if (integerStart == integerEnd && fractionStart == fractionEnd) {
        // 숫자가 하나도 없다("+", ".", "-.", "+x").
        return Fail(index >= size_ ? JsonErrorCode::UnexpectedEnd : JsonErrorCode::InvalidNumber,
                    index);
    }

    const usize exponentStart = index;
    if (index < size_ && (data_[index] == 'e' || data_[index] == 'E')) {
        integer = false;
        ++index;
        if (index < size_ && (data_[index] == '+' || data_[index] == '-')) {
            ++index;
        }

        if (index >= size_ || !IsDigit(data_[index])) {
            return Fail(index >= size_ ? JsonErrorCode::UnexpectedEnd
                                       : JsonErrorCode::InvalidNumber,
                        index);
        }

        while (index < size_ && IsDigit(data_[index])) {
            ++index;
        }
    }

    if (IsIdentifierContinuation(index)) {
        return Fail(JsonErrorCode::InvalidNumber, index);
    }

    position_ = index;
    // 정규화: 앞 '+' 제거, 정수부가 없으면 "0", 소수점 뒤 숫자가 없으면 "0"을 채운다(".5" →
    // "0.5", "5." → "5.0", "5.e3" → "5.0e3"). 지수는 그대로다.
    const usize textStart = document_.text_.Size();
    const bool written =
        (!negative || AppendText("-", 1)) &&
        (integerStart == integerEnd
             ? AppendText("0", 1)
             : AppendText(data_ + integerStart, integerEnd - integerStart)) &&
        (!point || AppendText(".", 1)) &&
        (!point || (fractionStart == fractionEnd
                        ? AppendText("0", 1)
                        : AppendText(data_ + fractionStart, fractionEnd - fractionStart))) &&
        (index == exponentStart || AppendText(data_ + exponentStart, index - exponentStart));
    if (!written) {
        return false;
    }

    return AddNumberNode(textStart, integer, start, node);
}

bool JsonParser::ParseJson5Word(uint32& node) noexcept {
    struct Literal {
        const char* text;
        usize length;
        JsonType type;
        bool value;
    };
    const Literal literals[] = {{"true", 4, JsonType::Bool, true},
                                {"false", 5, JsonType::Bool, false},
                                {"null", 4, JsonType::Null, false}};
    for (const Literal& literal : literals) {
        if (data_[position_] != static_cast<uint8>(literal.text[0])) {
            continue;
        }

        switch (MatchWord(position_, literal.text, literal.length)) {
        case WordMatch::Truncated:
            return Fail(JsonErrorCode::UnexpectedEnd, size_);
        case WordMatch::Mismatch:
            return Fail(JsonErrorCode::InvalidLiteral, position_);
        case WordMatch::Match:
            break;
        }

        const usize start = position_;
        position_ += literal.length;
        Node value{};
        value.type = literal.type;
        value.boolean = literal.value;
        return AddNode(value, node, start);
    }

    // 식별자처럼 시작하는 그 밖의 단어는 모르는 리터럴이다.
    return Fail(IsIdentifierStartAt(position_) ? JsonErrorCode::InvalidLiteral
                                               : JsonErrorCode::UnexpectedCharacter,
                position_);
}

} // namespace rocks
