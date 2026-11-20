#pragma once

#include <Base/Types.h>
#include <Core/Text/StringView.h>

// 서식 문자열 문법 해석기다. 런타임 서식(StringBuilderBase::TryAppendFormat)과 로그 메시지 템플릿의
// 컴파일 타임 검증(LogTemplate)·렌더가 같은 constexpr 함수를 쓴다. 문법은 StringBuilder.h 첫머리와
// Docs/Design/LoggingSystem.ko.md 6.3절을 따른다.
//
// 사용자 코드는 보통 직접 쓰지 않는다. 서식은 TryAppendFormat, 로그는 LogTemplate을 쓴다.
namespace rocks::detail {

// 정밀도를 지정하지 않은 표지다.
inline constexpr usize FormatNoPrecision = MaxSize;

// 폭·정밀도·인자 번호의 상한이다.
inline constexpr usize FormatMaxNumber = 65535;

// double의 정확한 10진 전개는 소수점 아래 최대 1074자리다. 그보다 큰 정밀도는 받지 않는다(실수
// 표기를 고정 크기 stack buffer에 담기 위한 상한).
inline constexpr usize FormatMaxFloatingPrecision = 1074;

// 서식 인자의 내장 종류다. Custom은 Formatter<T>를 특수화한 사용자 타입이며 spec을 Formatter가
// 직접 해석한다. NullText는 null C 문자열(늘 서식 오류)이다.
enum class FormatArgKind : uint8 {
    Bool,
    Char,
    Signed,
    Unsigned,
    Float,
    Double,
    Text,
    NullText,
    Pointer,
    Custom
};

// 해석한 spec이다: [[채움]정렬][부호][#][0][폭][.정밀도][형식].
struct FormatSpec {
    char fill = ' ';
    char align = 0;
    char sign = 0;
    bool alternate = false;
    bool zero = false;
    usize width = 0;
    usize precision = FormatNoPrecision;
    char type = 0;
};

constexpr bool IsFormatDigit(char character) noexcept {
    return character >= '0' && character <= '9';
}

constexpr bool IsFormatAlign(char character) noexcept {
    return character == '<' || character == '>' || character == '^';
}

// 10진 숫자열을 읽는다. FormatMaxNumber를 넘거나 숫자가 없으면 false다.
constexpr bool ParseFormatNumber(const char* text, usize bytes, usize& index,
                                 usize& value) noexcept {
    const usize start = index;
    value = 0;
    while (index < bytes && IsFormatDigit(text[index])) {
        value = value * 10 + static_cast<usize>(text[index] - '0');
        if (value > FormatMaxNumber) {
            return false;
        }

        ++index;
    }

    return index != start;
}

// spec 원문(':' 뒤부터 '}' 앞까지)을 해석한다. 문법에 맞지 않으면 false이고 spec은 일부만 채워졌을
// 수 있다. 형식 문자와 인자 종류의 짝은 IsValidBuiltinFormatSpec이 따로 본다.
constexpr bool ParseFormatSpec(StringView text, FormatSpec& spec) noexcept {
    const char* data = text.Data();
    const usize bytes = text.ByteCount();
    usize index = 0;
    if (bytes >= 2 && IsFormatAlign(data[1])) {
        const auto fill = static_cast<unsigned char>(data[0]);
        if (fill < 0x20 || fill >= 0x7f) {
            return false;
        }

        spec.fill = data[0];
        spec.align = data[1];
        index = 2;
    } else if (bytes >= 1 && IsFormatAlign(data[0])) {
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
        if (!ParseFormatNumber(data, bytes, index, number)) {
            return false;
        }

        spec.width = number;
    }

    if (index < bytes && data[index] == '.') {
        ++index;
        if (!ParseFormatNumber(data, bytes, index, number)) {
            return false;
        }

        spec.precision = number;
    }

    if (index < bytes) {
        spec.type = data[index++];
    }

    return index == bytes;
}

// 정수 형식(d x X b B o)과 정밀도 없음이다. bool·char의 정수 표기와 포인터도 이 규칙을 지난다.
constexpr bool IsValidIntegerFormatSpec(const FormatSpec& spec) noexcept {
    if (spec.precision != FormatNoPrecision) {
        return false;
    }

    switch (spec.type) {
    case 0:
    case 'd':
    case 'x':
    case 'X':
    case 'b':
    case 'B':
    case 'o':
        return true;
    default:
        return false;
    }
}

// 문자열 형식(s)이다. 부호·#·0 채움은 받지 않는다.
constexpr bool IsValidTextFormatSpec(const FormatSpec& spec) noexcept {
    return (spec.type == 0 || spec.type == 's') && spec.sign == 0 && !spec.alternate && !spec.zero;
}

// 내장 종류의 spec이 받을 수 있는 조합인지다(값과 무관한 검사만). 런타임 서식과 로그 템플릿의
// 컴파일 타임 검증이 같이 쓴다. 값에 따라 정해지는 실패(비ASCII char의 'c', 잘못된 UTF-8 문자열,
// null 문자열)는 렌더 때 따로 본다. Custom은 Formatter가 spec을 직접 해석하므로 늘 true다.
constexpr bool IsValidBuiltinFormatSpec(FormatArgKind kind, const FormatSpec& spec) noexcept {
    switch (kind) {
    case FormatArgKind::Bool:
        if (spec.type == 0 || spec.type == 's') {
            // C++ 표준 format처럼 bool·char의 정밀도는 받지 않는다.
            return spec.precision == FormatNoPrecision && IsValidTextFormatSpec(spec);
        }

        return IsValidIntegerFormatSpec(spec);
    case FormatArgKind::Char:
        if (spec.type == 0 || spec.type == 'c') {
            FormatSpec text = spec;
            text.type = 0;
            return spec.precision == FormatNoPrecision && IsValidTextFormatSpec(text);
        }

        return IsValidIntegerFormatSpec(spec);
    case FormatArgKind::Signed:
    case FormatArgKind::Unsigned:
        return IsValidIntegerFormatSpec(spec);
    case FormatArgKind::Float:
    case FormatArgKind::Double: {
        const char type = spec.type;
        const bool known = type == 0 || type == 'f' || type == 'F' || type == 'e' || type == 'E' ||
                           type == 'g' || type == 'G';
        return known && !spec.alternate &&
               (spec.precision == FormatNoPrecision ||
                spec.precision <= FormatMaxFloatingPrecision);
    }
    case FormatArgKind::Text:
    case FormatArgKind::NullText:
        return IsValidTextFormatSpec(spec);
    case FormatArgKind::Pointer:
        return (spec.type == 0 || spec.type == 'p') && spec.sign == 0 && !spec.alternate &&
               spec.precision == FormatNoPrecision;
    case FormatArgKind::Custom:
        return true;
    }

    return false;
}

// 문법 오류의 종류다. TryAppendFormat은 모두 InvalidArgument로 알리고, 로그 템플릿은 종류별
// 이름으로 알린다(LogTemplateErrorName).
enum class FormatParseError : uint8 {
    None,
    // '{'의 짝 '}'가 없다(문자열 끝 또는 spec 안의 '{').
    UnmatchedOpenBrace,
    // '}' 하나가 '}}'가 아닌 곳에 있다.
    UnmatchedCloseBrace,
    // 인자 번호가 앞자리 0이거나 FormatMaxNumber를 넘는다.
    InvalidArgumentIndex,
    // "{}"와 "{0}"을 한 문자열에서 섞었다.
    MixedAutomaticAndManualIndex,
    // 이름 자리("{Name}")와 위치 자리("{}"·"{0}")를 섞었다.
    MixedNamedAndPositional,
    // 자리 안의 이름이 문법에 맞지 않거나, 이름 자리를 허용하지 않는 곳에 썼다.
    InvalidHoleName
};

enum class FormatTokenKind : uint8 {
    Literal,
    Hole
};

// 해석 단위 하나다.
// - Literal: text를 그대로 붙인다("{{"·"}}"는 중괄호 한 글자의 text).
// - Hole: argument번째 인자를 spec으로 서식화한다. name은 이름 자리의 이름(위치 자리는 빈 view),
//   hasSpec은 ':'가 있었는지다(빈 spec "{:}"과 spec 없음 "{}"을 구별한다). structured는 구조체
//   펼치기 자리("{@Name}")인지다(allowStructure일 때만 참이 될 수 있다, name에는 '@'가 없다).
// view는 모두 원본 서식 문자열을 빌린다.
struct FormatToken {
    FormatTokenKind kind = FormatTokenKind::Literal;
    StringView text;
    StringView name;
    StringView spec;
    bool hasSpec = false;
    bool structured = false;
    usize argument = 0;
};

// 서식 문자열을 앞에서부터 한 토큰씩 해석한다. 인자 번호의 범위(인자 수)는 보지 않는다. 호출자가
// 인자 수와 비교한다.
//
// 이름 자리: allowNamed가 참일 때만 "{Name}"·"{Name:spec}"을 받는다. 이름은 영문자로 시작하고
// 영문자· 숫자가 이어진다(숫자로 시작하면 위치 번호다). 이름 자리는 나온 순서대로 0, 1, 2…번째
// 인자에 묶인다. 이름·위치 자리를 섞으면 오류다.
//
// 구조체 펼치기 자리: allowStructure도 참일 때만 이름 앞 '@'("{@Name}")를 받는다(로그 템플릿 전용,
// Docs/Design/LoggingSystem.ko.md 14.1.6절). 그 밖의 파서(StringBuilder 서식·로그 레이아웃)에서
// '@'는 InvalidHoleName이다. '@' 뒤에는 이름이 와야 한다("{@}"·"{@0}"은 InvalidHoleName).
//
// 사용 예:
//   FormatParser parser(format, false);
//   FormatToken token;
//   while (parser.Next(token)) { ... }
//   if (parser.Error() != FormatParseError::None) { ... }
class FormatParser {
public:
    constexpr FormatParser(StringView format, bool allowNamed, bool allowStructure = false) noexcept
        : text_(format.Data()), bytes_(format.ByteCount()), allowNamed_(allowNamed),
          allowStructure_(allowNamed && allowStructure) {
    }

    // 다음 토큰을 token에 쓰고 true를 돌려준다. 끝이거나 오류면 false다(Error()로 구별한다). 오류
    // 뒤에는 계속 false다.
    constexpr bool Next(FormatToken& token) noexcept {
        if (error_ != FormatParseError::None || index_ >= bytes_) {
            return false;
        }

        token = FormatToken{};
        const char character = text_[index_];

        // 일반 문자열 구간. 중괄호는 ASCII이므로 구간 경계가 UTF-8 문자를 자르지 않는다.
        if (character != '{' && character != '}') {
            usize end = index_;
            while (end < bytes_ && text_[end] != '{' && text_[end] != '}') {
                ++end;
            }

            token.kind = FormatTokenKind::Literal;
            token.text = StringView(text_ + index_, end - index_);
            index_ = end;
            return true;
        }

        // "}}"는 '}' 문자다. 짝 없는 '}'는 오류다.
        if (character == '}') {
            if (index_ + 1 >= bytes_ || text_[index_ + 1] != '}') {
                return Fail(FormatParseError::UnmatchedCloseBrace);
            }

            token.kind = FormatTokenKind::Literal;
            token.text = StringView(text_ + index_, 1);
            index_ += 2;
            return true;
        }

        // "{{"는 '{' 문자다.
        if (index_ + 1 < bytes_ && text_[index_ + 1] == '{') {
            token.kind = FormatTokenKind::Literal;
            token.text = StringView(text_ + index_, 1);
            index_ += 2;
            return true;
        }

        return ParseHole(token);
    }

    constexpr FormatParseError Error() const noexcept {
        return error_;
    }

    // 다음 토큰이 시작할 byte 위치다. 오류 뒤에는 오류를 알아챈 위치다.
    constexpr usize Position() const noexcept {
        return index_;
    }

    // 지금까지 이름 자리를 만났는지다.
    constexpr bool SawNamedHole() const noexcept {
        return named_;
    }

    // 지금까지 번호 자리("{0}")를 만났는지다.
    constexpr bool SawManualIndex() const noexcept {
        return manual_;
    }

private:
    static constexpr bool IsNameStart(char character) noexcept {
        return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z');
    }

    static constexpr bool IsNameCharacter(char character) noexcept {
        return IsNameStart(character) || IsFormatDigit(character);
    }

    constexpr bool Fail(FormatParseError error) noexcept {
        error_ = error;
        return false;
    }

    // 치환 필드: '{' [arg-id | ['@'] name] [':' spec] '}'
    constexpr bool ParseHole(FormatToken& token) noexcept {
        const usize holeStart = index_;
        ++index_;
        usize argument = 0;
        if (allowStructure_ && index_ < bytes_ && text_[index_] == '@') {
            ++index_;
            if (index_ >= bytes_) {
                return Fail(FormatParseError::UnmatchedOpenBrace);
            }

            if (!IsNameStart(text_[index_])) {
                return Fail(FormatParseError::InvalidHoleName);
            }

            token.structured = true;
        }

        if (index_ < bytes_ && IsFormatDigit(text_[index_])) {
            if (named_) {
                return Fail(FormatParseError::MixedNamedAndPositional);
            }

            if (automatic_) {
                return Fail(FormatParseError::MixedAutomaticAndManualIndex);
            }

            manual_ = true;
            if (text_[index_] == '0' && index_ + 1 < bytes_ && IsFormatDigit(text_[index_ + 1])) {
                return Fail(FormatParseError::InvalidArgumentIndex); // 앞자리 0은 받지 않는다
            }

            if (!ParseFormatNumber(text_, bytes_, index_, argument)) {
                return Fail(FormatParseError::InvalidArgumentIndex);
            }
        } else if (allowNamed_ && index_ < bytes_ && IsNameStart(text_[index_])) {
            if (automatic_ || manual_) {
                return Fail(FormatParseError::MixedNamedAndPositional);
            }

            named_ = true;
            const usize nameStart = index_;
            while (index_ < bytes_ && IsNameCharacter(text_[index_])) {
                ++index_;
            }

            token.name = StringView(text_ + nameStart, index_ - nameStart);
            argument = next_++;
        } else {
            if (named_) {
                return Fail(FormatParseError::MixedNamedAndPositional);
            }

            if (manual_) {
                return Fail(FormatParseError::MixedAutomaticAndManualIndex);
            }

            automatic_ = true;
            argument = next_++;
        }

        usize specStart = index_;
        if (index_ < bytes_ && text_[index_] == ':') {
            token.hasSpec = true;
            specStart = ++index_;
            while (index_ < bytes_ && text_[index_] != '}' && text_[index_] != '{') {
                ++index_;
            }
        }

        if (index_ >= bytes_ || text_[index_] == '{') {
            return Fail(FormatParseError::UnmatchedOpenBrace);
        }

        if (text_[index_] != '}') {
            return Fail(FormatParseError::InvalidHoleName);
        }

        token.kind = FormatTokenKind::Hole;
        token.spec = StringView(text_ + specStart, index_ - specStart);
        token.argument = argument;
        ++index_;
        token.text = StringView(text_ + holeStart, index_ - holeStart);
        return true;
    }

    const char* text_ = nullptr;
    usize bytes_ = 0;
    usize index_ = 0;
    usize next_ = 0;
    bool allowNamed_ = false;
    bool allowStructure_ = false;
    bool automatic_ = false;
    bool manual_ = false;
    bool named_ = false;
    FormatParseError error_ = FormatParseError::None;
};

} // namespace rocks::detail
