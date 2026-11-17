#pragma once

#include <Base/TypeTraits.h>
#include <Base/Types.h>
#include <Core/Diagnostics/SourceLocation.h>
#include <Core/Hash.h>
#include <Core/Text/FormatParse.h>
#include <Core/Text/StringBuilder.h>
#include <Core/Text/StringView.h>

// 로그 메시지 템플릿(Docs/Design/LoggingSystem.ko.md 6.3·6.4절)이다.
//
// 문법:
// - "{Name}"·"{Name:spec}": 이름 자리. 나온 순서대로 인자에 묶이고, 이름은 속성 키다(Serilog 규칙).
//   이름은 영문자로 시작하고 영문자·숫자가 이어진다. 같은 이름을 두 번 쓰면 오류다.
// - "{}"·"{0}": 위치 자리(StringBuilder 서식과 같다). 속성 키는 Arg0, Arg1…이다. 이름 자리와 한
//   템플릿에 섞으면 오류다.
// - "{{"·"}}"는 중괄호 문자, spec은 StringBuilder 서식의 spec 그대로다.
//
// 검증(부분 검증): 리터럴 템플릿은 컴파일 타임에 중괄호 짝, 이름·위치 혼용, 이름 중복, 자리 수와
// 인자 수, 위치 번호 범위, UTF-8, **내장 타입** 인자의 spec을 검사한다. 사용자 Formatter<T> 타입의
// spec은 Formatter가 직접 해석하므로 캡처할 때(런타임) 검사된다. 위반은 컴파일 오류이며 진단에
// detail::LogTemplateError_* 함수 이름이 나온다.
namespace rocks {

// 로그 호출 하나가 받는 인자 수의 상한이다. 캡처 슬롯의 값 칸 수를 고정하기 위한 값이다.
inline constexpr usize MaxLogArguments = 16;

// 템플릿 검증 오류의 종류다. 이름은 컴파일 오류의 표지 함수 이름과 런타임 템플릿 오류 표기
// ("[template error: ArgumentCountMismatch]")에 그대로 쓴다.
enum class LogTemplateError : uint8 {
    None,
    // 짝 없는 '{' 또는 '}'
    UnbalancedBrace,
    // 이름 자리와 위치 자리를 섞었다
    MixedNamedAndPositional,
    // "{}"와 "{0}"을 섞었다
    MixedAutomaticAndManualIndex,
    // 위치 번호가 앞자리 0이거나 너무 크다
    InvalidArgumentIndex,
    // 자리 안의 이름이 문법에 맞지 않는다
    InvalidHoleName,
    // 같은 이름 자리를 두 번 썼다
    DuplicateHoleName,
    // 위치 번호가 인자 수 이상이다
    ArgumentIndexOutOfRange,
    // 자리 수와 인자 수가 다르다(번호 자리면 쓰지 않은 인자가 있다)
    ArgumentCountMismatch,
    // 내장 타입 인자의 spec이 잘못됐다
    InvalidSpec,
    // 템플릿이 유효한 UTF-8이 아니다
    InvalidUtf8,
    // 인자가 MaxLogArguments보다 많다
    TooManyArguments
};

// 오류 종류의 영어 이름이다(None은 "None").
constexpr StringView LogTemplateErrorName(LogTemplateError error) noexcept {
    switch (error) {
    case LogTemplateError::None:
        return "None";
    case LogTemplateError::UnbalancedBrace:
        return "UnbalancedBrace";
    case LogTemplateError::MixedNamedAndPositional:
        return "MixedNamedAndPositional";
    case LogTemplateError::MixedAutomaticAndManualIndex:
        return "MixedAutomaticAndManualIndex";
    case LogTemplateError::InvalidArgumentIndex:
        return "InvalidArgumentIndex";
    case LogTemplateError::InvalidHoleName:
        return "InvalidHoleName";
    case LogTemplateError::DuplicateHoleName:
        return "DuplicateHoleName";
    case LogTemplateError::ArgumentIndexOutOfRange:
        return "ArgumentIndexOutOfRange";
    case LogTemplateError::ArgumentCountMismatch:
        return "ArgumentCountMismatch";
    case LogTemplateError::InvalidSpec:
        return "InvalidSpec";
    case LogTemplateError::InvalidUtf8:
        return "InvalidUtf8";
    case LogTemplateError::TooManyArguments:
        return "TooManyArguments";
    }

    return "Unknown";
}

namespace detail {

// LogTemplate의 consteval 생성자가 위반을 컴파일 오류로 알리는 표지 함수다. constexpr이 아니므로
// 상수 평가 중에 부르면 컴파일이 실패하고, 진단에 이 함수 이름이 나온다. 런타임에는 부르지 않는다.
inline void LogTemplateError_UnbalancedBrace() noexcept {
}

inline void LogTemplateError_MixedNamedAndPositional() noexcept {
}

inline void LogTemplateError_MixedAutomaticAndManualIndex() noexcept {
}

inline void LogTemplateError_InvalidArgumentIndex() noexcept {
}

inline void LogTemplateError_InvalidHoleName() noexcept {
}

inline void LogTemplateError_DuplicateHoleName() noexcept {
}

inline void LogTemplateError_ArgumentIndexOutOfRange() noexcept {
}

inline void LogTemplateError_ArgumentCountMismatch() noexcept {
}

inline void LogTemplateError_InvalidSpec() noexcept {
}

inline void LogTemplateError_InvalidUtf8() noexcept {
}

inline void LogTemplateError_TooManyArguments() noexcept {
}

inline void LogTemplateError_NotNullTerminated() noexcept {
}

// 로그 인자 타입의 서식 종류다. 서식과 같되, Formatter가 없는 enum은 바탕 정수로 본다(캡처는 원시
// 값을 담는다).
template <typename T>
consteval FormatArgKind LogArgKindOf() noexcept {
    using U = RemoveCvRefT<T>;
    if constexpr (!HasFormatter<U> && IsEnumV<U>) {
        return IsSignedV<UnderlyingTypeT<U>> ? FormatArgKind::Signed : FormatArgKind::Unsigned;
    } else {
        return FormatArgKindOf<U>();
    }
}

// 템플릿을 해석한 결과다. 렌더(속성 이름)와 캡처(사용자 Formatter의 spec)가 쓴다.
// - named: 이름 자리 템플릿인지. names[i]는 i번째 인자의 이름(이름 자리일 때만)
// - specs[i]·hasSpec[i]: i번째 인자를 처음 쓴 자리의 spec(사용자 Formatter는 이 spec으로 한 번만
//   렌더한다)
// view는 모두 템플릿 원문을 빌린다.
struct LogTemplateLayout {
    bool named = false;
    StringView names[MaxLogArguments];
    StringView specs[MaxLogArguments];
    bool hasSpec[MaxLogArguments]{};
    bool referenced[MaxLogArguments]{};
};

constexpr LogTemplateError ToLogTemplateError(FormatParseError error) noexcept {
    switch (error) {
    case FormatParseError::None:
        return LogTemplateError::None;
    case FormatParseError::UnmatchedOpenBrace:
    case FormatParseError::UnmatchedCloseBrace:
        return LogTemplateError::UnbalancedBrace;
    case FormatParseError::InvalidArgumentIndex:
        return LogTemplateError::InvalidArgumentIndex;
    case FormatParseError::MixedAutomaticAndManualIndex:
        return LogTemplateError::MixedAutomaticAndManualIndex;
    case FormatParseError::MixedNamedAndPositional:
        return LogTemplateError::MixedNamedAndPositional;
    case FormatParseError::InvalidHoleName:
        return LogTemplateError::InvalidHoleName;
    }

    return LogTemplateError::UnbalancedBrace;
}

// 템플릿 text를 인자 종류 kinds[0..count)에 대해 검증하고 layout을 채운다. 컴파일 타임 검증
// (LogTemplate)과 런타임 템플릿의 렌더가 같은 함수를 쓴다. 오류가 나면 그 앞까지의 layout만
// 채워진다.
constexpr LogTemplateError AnalyzeLogTemplate(StringView text, const FormatArgKind* kinds,
                                              usize count, LogTemplateLayout& layout) noexcept {
    if (count > MaxLogArguments) {
        return LogTemplateError::TooManyArguments;
    }

    if (!text.IsValidUtf8()) {
        return LogTemplateError::InvalidUtf8;
    }

    FormatParser parser(text, true);
    FormatToken token;
    usize holes = 0;
    while (parser.Next(token)) {
        if (token.kind != FormatTokenKind::Hole) {
            continue;
        }

        ++holes;
        const usize argument = token.argument;
        if (argument >= count) {
            return parser.SawManualIndex() ? LogTemplateError::ArgumentIndexOutOfRange
                                           : LogTemplateError::ArgumentCountMismatch;
        }

        if (!token.name.IsEmpty()) {
            layout.named = true;
            for (usize previous = 0; previous < argument; ++previous) {
                if (layout.names[previous] == token.name) {
                    return LogTemplateError::DuplicateHoleName;
                }
            }

            layout.names[argument] = token.name;
        }

        if (kinds[argument] != FormatArgKind::Custom) {
            FormatSpec spec;
            if (token.hasSpec && !ParseFormatSpec(token.spec, spec)) {
                return LogTemplateError::InvalidSpec;
            }

            if (!IsValidBuiltinFormatSpec(kinds[argument], spec)) {
                return LogTemplateError::InvalidSpec;
            }
        }

        if (!layout.referenced[argument]) {
            layout.referenced[argument] = true;
            layout.specs[argument] = token.spec;
            layout.hasSpec[argument] = token.hasSpec;
        }
    }

    if (parser.Error() != FormatParseError::None) {
        return ToLogTemplateError(parser.Error());
    }

    // 자동·이름 자리는 자리 수가 인자 수와 같아야 하고, 번호 자리는 모든 인자를 한 번 이상 써야
    // 한다.
    if (!parser.SawManualIndex()) {
        return holes == count ? LogTemplateError::None : LogTemplateError::ArgumentCountMismatch;
    }

    for (usize index = 0; index < count; ++index) {
        if (!layout.referenced[index]) {
            return LogTemplateError::ArgumentCountMismatch;
        }
    }

    return LogTemplateError::None;
}

// consteval 문맥에서 오류를 표지 함수 호출로 바꾼다.
constexpr void ReportLogTemplateError(LogTemplateError error) noexcept {
    switch (error) {
    case LogTemplateError::None:
        break;
    case LogTemplateError::UnbalancedBrace:
        LogTemplateError_UnbalancedBrace();
        break;
    case LogTemplateError::MixedNamedAndPositional:
        LogTemplateError_MixedNamedAndPositional();
        break;
    case LogTemplateError::MixedAutomaticAndManualIndex:
        LogTemplateError_MixedAutomaticAndManualIndex();
        break;
    case LogTemplateError::InvalidArgumentIndex:
        LogTemplateError_InvalidArgumentIndex();
        break;
    case LogTemplateError::InvalidHoleName:
        LogTemplateError_InvalidHoleName();
        break;
    case LogTemplateError::DuplicateHoleName:
        LogTemplateError_DuplicateHoleName();
        break;
    case LogTemplateError::ArgumentIndexOutOfRange:
        LogTemplateError_ArgumentIndexOutOfRange();
        break;
    case LogTemplateError::ArgumentCountMismatch:
        LogTemplateError_ArgumentCountMismatch();
        break;
    case LogTemplateError::InvalidSpec:
        LogTemplateError_InvalidSpec();
        break;
    case LogTemplateError::InvalidUtf8:
        LogTemplateError_InvalidUtf8();
        break;
    case LogTemplateError::TooManyArguments:
        LogTemplateError_TooManyArguments();
        break;
    }
}

} // namespace detail

// 컴파일 타임에 검증한 리터럴 메시지 템플릿이다. Args는 템플릿 자리에 들어갈 인자 타입이다.
//
// 보통 직접 만들지 않는다. 로그 함수가 `LogTemplate<TypeIdentityT<Args>...>` 매개변수로 받고,
// 문자열 리터럴이 consteval 생성자로 바뀐다. TypeIdentityT가 템플릿 쪽 추론을 막으므로 Args는 뒤의
// 실제 인자에서만 추론된다. 생성자의 기본 인자 SourceLocation::Current()는 호출 지점에서 평가되므로
// 로그 함수를 부른 곳의 위치가 남는다.
//
// 값: 템플릿 원문(정적 저장소), EventId(원문의 HashStringBytes, 컴파일 타임 계산), 호출 위치.
// 수명: 원문과 위치 문자열은 정적 저장소라 프로그램 수명 동안 유효하다.
//
// 사용 예:
//   template <typename... Args>
//   void Info(const LogCategory& category, LogTemplate<TypeIdentityT<Args>...> text,
//             const Args&... args) noexcept;
//   Info(LogEngine, "Loaded {Count} assets in {Seconds:.2f}s", count, seconds);
template <typename... Args>
class LogTemplate {
    static_assert(sizeof...(Args) <= MaxLogArguments,
                  "A log call takes at most MaxLogArguments arguments");

public:
    template <usize Count>
    consteval LogTemplate(const char (&text)[Count],
                          SourceLocation where = SourceLocation::Current()) noexcept
        : text_(text), bytes_(Count - 1), eventId_(HashStringBytes(StringView(text, Count - 1))),
          location_(where) {
        if (text[Count - 1] != '\0') {
            detail::LogTemplateError_NotNullTerminated();
        }

        // 인자가 없을 때 길이 0 배열을 피하려고 끝에 표지 하나를 더 둔다(쓰지 않는다).
        constexpr detail::FormatArgKind kinds[] = {detail::LogArgKindOf<Args>()...,
                                                   detail::FormatArgKind::Custom};
        detail::LogTemplateLayout layout{};
        detail::ReportLogTemplateError(detail::AnalyzeLogTemplate(StringView(text, Count - 1),
                                                                  kinds, sizeof...(Args), layout));
    }

    // 템플릿 원문이다(끝 NUL 제외).
    constexpr StringView Text() const noexcept {
        return StringView(text_, bytes_);
    }

    // 사건 종류 id(원문의 HashStringBytes)다. 같은 원문이면 어디서 불러도 같다. 영속 식별자가
    // 아니다.
    constexpr uint64 EventId() const noexcept {
        return eventId_;
    }

    // 로그 함수를 부른 위치다.
    constexpr SourceLocation Location() const noexcept {
        return location_;
    }

private:
    const char* text_;
    usize bytes_;
    uint64 eventId_;
    SourceLocation location_;
};

// 실행 중에 정한 메시지 템플릿이다(설정 파일·스크립트에서 온 문자열 등). 명시적으로 만든다.
// - 검증은 렌더할 때 런타임에 한다. 잘못되면 message가 원본 + "[template error: <code>]"이고
//   LogEventFlags::TemplateError가 선다(로그 경로는 실패하지 않는다).
// - 캡처할 때 원문을 기록으로 복사하므로, 캡처가 끝나면 원문 저장소를 버려도 된다.
// - EventId는 원문의 HashStringBytes를 런타임에 계산한 값이다(같은 원문의 LogTemplate과 같다).
//
// 수명: 이 객체는 원문을 빌린다. 캡처 호출이 끝날 때까지 원문이 살아 있어야 한다.
//
// 사용 예:
//   CaptureLog(target, LogLevel::Info, LogScript, RuntimeTemplate{scriptText}, value);
class RuntimeTemplate {
public:
    explicit RuntimeTemplate(StringView text,
                             SourceLocation where = SourceLocation::Current()) noexcept
        : text_(text), eventId_(HashStringBytes(text)), location_(where) {
    }

    StringView Text() const noexcept {
        return text_;
    }

    uint64 EventId() const noexcept {
        return eventId_;
    }

    SourceLocation Location() const noexcept {
        return location_;
    }

private:
    StringView text_;
    uint64 eventId_;
    SourceLocation location_;
};

} // namespace rocks
