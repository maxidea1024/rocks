#pragma once

#include <Base/Types.h>
#include <Core/Diagnostics/LogEvent.h>
#include <Core/Diagnostics/LogRecord.h>
#include <Core/Result.h>
#include <Core/Text/FormatParse.h>
#include <Core/Text/StringView.h>

// 로그 sink의 레이아웃 패턴이다(Docs/Design/LoggingSystem.ko.md 9절, log4j PatternLayout·Serilog
// outputTemplate 대응). sink를 만들 때 한 번 해석하고, 기록마다 해석하지 않는다.
//
// 문법: 리터럴 문자열과 "{토큰}"·"{토큰:형식}"의 나열. "{{"·"}}"는 중괄호 문자다.
//
//   토큰                 출력(비면 생략 규칙을 따른다)
//   {Timestamp}          UTC ISO 8601, 소수부 가변("2026-11-17T09:30:00.25Z"). UTC가 없으면 빈 값
//   {Timestamp:ms}       UTC ISO 8601, 밀리초 3자리 고정("2026-11-17T09:30:00.250Z")
//   {Elapsed}            레이아웃 기준 시각부터의 단조 경과 "초.밀리초"("12.345")
//   {Level}·{Level:u3}   "Warning" · "WRN"(TRC DBG INF WRN ERR FTL)
//   {Category}           카테고리 이름
//   {EventId}            사건 종류 id, 16자리 16진(0이면 빈 값)
//   {Thread}             스레드 이름, 없으면 id(0이면 빈 값)
//   {Frame}              엔진 프레임 번호(10진)
//   {Message}·{Template} 렌더된 메시지 · 원본 템플릿
//   {Properties}         템플릿 자리에 쓰이지 않은 속성 "이름=값"(공백으로 구분)
//   {Context}            문맥 "World=Main Stage=BeginPlay"
//   {Location}           "파일:줄 함수". 기록 레벨이 options.locationMinimum(기본 Warning) 이상일
//                        때만, 아니면 빈 값
//   {NewLine}            줄바꿈("\n")
//   {Correlation}        상관 id "session=3 task=1842"(0인 칸은 빼고, 모두 비면 빈 값)
//
// 빈 값 생략 규칙(정확한 정의):
//   1. 토큰이 빈 값이고 바로 앞 리터럴이 '['로 끝나며 바로 뒤 리터럴이 ']'로 시작하면, 그 '['와
//      ']'를 함께 뺀다("[{Timestamp:ms}]"가 통째로 사라진다).
//   2. 그렇지 않고 바로 앞 리터럴이 공백으로 끝나면 그 공백 하나를 뺀다.
//   3. 리터럴 없이 앞 토큰에 바로 붙은 토큰({Message}{Context})은, 값이 있고 앞 출력이
//   공백·줄바꿈으로
//      끝나지 않았으면 공백 하나를 앞에 넣는다({NewLine}은 예외). 빈 토큰을 건너뛰어도 붙어 있는
//      것으로 본다({Message}{Context}{Location}에서 Context가 비면 Location이 Message에 붙는다).
//
// 기본 콘솔 레이아웃(DefaultConsoleLogLayout):
//   "[{Timestamp:ms}][{Level}][{Category}] {Message}{Context}{Location}{NewLine}"
//   → "[2026-11-17T09:30:00.250Z][Warning][Engine] Frame took 40 ms World=Main Engine.cpp:118 Tick"
//
// 사용 예:
//   auto layout = LogLayout::Parse("[{Level:u3}] {Message}{NewLine}");
//   if (!layout) { ... }                      // 잘못된 패턴은 여기서 실패한다
//   TextLogSink sink(output, layout.Value());
namespace rocks {

// 기본 콘솔 레이아웃이다.
inline constexpr StringView DefaultConsoleLogLayout =
    "[{Timestamp:ms}][{Level}][{Category}] {Message}{Context}{Location}{NewLine}";

// 한 레이아웃의 최대 토큰 수와 리터럴 byte 수다(해석 결과를 고정 배열에 둔다).
inline constexpr usize MaxLogLayoutTokens = 32;
inline constexpr usize MaxLogLayoutTextBytes = 256;

// 레이아웃 패턴의 문법 오류다(LogLayout::Parse의 선택 출력).
// - offset: 오류가 난 byte 위치(토큰이면 그 '{' 위치)
// - code: InvalidArgument(문법·모르는 토큰·형식) 또는 CapacityExceeded(토큰·리터럴 상한)
// - reason: 영어 리터럴 이유 문장(정적 저장소)
struct LogLayoutParseError {
    usize offset = 0;
    ErrorCode code = ErrorCode::None;
    StringView reason;
};

namespace detail {

// 레이아웃 토큰 이름이다. 순서가 LogLayout의 토큰 번호다.
inline constexpr StringView LogLayoutTokenNames[] = {
    "Timestamp", "Elapsed",  "Level",      "Category", "EventId",  "Thread",  "Frame",
    "Message",   "Template", "Properties", "Context",  "Location", "NewLine", "Correlation"};

inline constexpr usize LogLayoutTokenCount =
    sizeof(LogLayoutTokenNames) / sizeof(LogLayoutTokenNames[0]);
inline constexpr usize LogLayoutTimestampToken = 0;
inline constexpr usize LogLayoutLevelToken = 2;

constexpr bool FailLogLayout(LogLayoutParseError& error, usize offset, ErrorCode code,
                             StringView reason) noexcept {
    error.offset = offset;
    error.code = code;
    error.reason = reason;
    return false;
}

// pattern의 문법을 검사한다. Parse와 CheckLogLayoutSyntax가 같은 함수를 쓴다. 통과하면 true,
// 아니면 error를 채우고 false다. 순수 함수이고 상수 평가할 수 있다.
constexpr bool ScanLogLayout(StringView pattern, LogLayoutParseError& error) noexcept {
    if (!pattern.IsValidUtf8()) {
        return FailLogLayout(error, 0, ErrorCode::InvalidArgument,
                             "layout pattern is not valid UTF-8");
    }

    usize textBytes = 0;
    usize tokens = 0;
    FormatParser parser(pattern, true);
    FormatToken token;
    usize start = parser.Position();
    while (parser.Next(token)) {
        if (token.kind == FormatTokenKind::Literal) {
            textBytes += token.text.ByteCount();
            if (textBytes > MaxLogLayoutTextBytes) {
                return FailLogLayout(error, start, ErrorCode::CapacityExceeded,
                                     "layout literal text is longer than MaxLogLayoutTextBytes");
            }

            start = parser.Position();
            continue;
        }

        if (token.name.IsEmpty()) {
            return FailLogLayout(error, start, ErrorCode::InvalidArgument,
                                 "positional holes are not layout tokens");
        }

        if (tokens == MaxLogLayoutTokens) {
            return FailLogLayout(error, start, ErrorCode::CapacityExceeded,
                                 "layout has more than MaxLogLayoutTokens tokens");
        }

        usize found = LogLayoutTokenCount;
        for (usize index = 0; index < LogLayoutTokenCount; ++index) {
            if (LogLayoutTokenNames[index] == token.name) {
                found = index;
            }
        }

        if (found == LogLayoutTokenCount) {
            return FailLogLayout(error, start, ErrorCode::InvalidArgument, "unknown layout token");
        }

        const bool formatOk = !token.hasSpec ||
                              (found == LogLayoutTimestampToken && token.spec == "ms") ||
                              (found == LogLayoutLevelToken && token.spec == "u3");
        if (!formatOk) {
            return FailLogLayout(error, start, ErrorCode::InvalidArgument,
                                 "unsupported format for this layout token");
        }

        ++tokens;
        start = parser.Position();
    }

    if (parser.Error() != FormatParseError::None) {
        return FailLogLayout(error, parser.Position(), ErrorCode::InvalidArgument,
                             "unbalanced brace or malformed token in layout pattern");
    }

    return true;
}

} // namespace detail

// 레이아웃 패턴의 문법만 검사한다(설정 검증용, CVar Validate). 통과하면 빈 view, 아니면 영어
// 리터럴 이유 문장이다. 순수 함수이고 상수 평가할 수 있다. 위치는 LogLayout::Parse의 오류 출력이
// 준다.
//
//   static_assert(CheckLogLayoutSyntax("[{Level}] {Message}").IsEmpty());
constexpr StringView CheckLogLayoutSyntax(StringView pattern) noexcept {
    LogLayoutParseError error;
    return detail::ScanLogLayout(pattern, error) ? StringView() : error.reason;
}

// 레이아웃 해석 옵션이다.
// - locationMinimum: {Location}을 쓰는 최소 레벨
// - startMonotonicNanoseconds: {Elapsed}의 기준 단조 시각(0이면 Parse를 부른 시각)
struct LogLayoutOptions {
    LogLevel locationMinimum = LogLevel::Warning;
    uint64 startMonotonicNanoseconds = 0;
};

// 렌더한 조각을 받는 곳이다. 조각은 렌더 호출 동안만 유효하다.
struct LogLayoutOutput {
    void* context = nullptr;
    void (*append)(void*, StringView) noexcept = nullptr;
};

// 해석한 레이아웃이다. 값 타입(고정 배열)이라 복사해 sink에 둔다. 할당하지 않는다.
// 스레드: Render는 const이고 상태를 바꾸지 않으므로 여러 스레드에서 동시에 불러도 된다.
class LogLayout {
public:
    // pattern을 해석한다. 실패: 모르는 토큰·형식, 위치 자리("{}"·"{0}"), 짝 없는 중괄호는
    // InvalidArgument, 토큰이 MaxLogLayoutTokens를 넘거나 리터럴이 MaxLogLayoutTextBytes를 넘으면
    // CapacityExceeded다. error가 있으면 실패 때 위치·이유를 채운다.
    static Result<LogLayout> Parse(StringView pattern, const LogLayoutOptions& options = {},
                                   LogLayoutParseError* error = nullptr) noexcept;

    // DefaultConsoleLogLayout을 해석한 레이아웃이다(실패하지 않는다).
    static LogLayout DefaultConsole() noexcept;

    // event를 렌더해 조각을 순서대로 out.append에 넘긴다. 힙을 쓰지 않는다(숫자·시각은 스택 버퍼).
    void Render(const LogEvent& event, const LogLayoutOutput& out) const noexcept;

    usize TokenCount() const noexcept {
        return partCount_;
    }

private:
    enum class Token : uint8 {
        Timestamp,
        Elapsed,
        Level,
        Category,
        EventId,
        Thread,
        Frame,
        Message,
        Template,
        Properties,
        Context,
        Location,
        NewLine,
        Correlation
    };

    // 토큰 하나와 그 앞 리터럴(text_ 안의 위치)이다. variant는 형식(ms·u3)이다.
    struct Part {
        Token token = Token::Message;
        uint8 variant = 0;
        uint16 literalOffset = 0;
        uint16 literalBytes = 0;
    };

    LogLayout() noexcept = default;

    StringView Literal(uint16 offset, uint16 bytes) const noexcept {
        return StringView(text_ + offset, bytes);
    }

    bool IsEmpty(const Part& part, const LogEvent& event) const noexcept;
    void EmitToken(const Part& part, const LogEvent& event, void* writer) const noexcept;

    Part parts_[MaxLogLayoutTokens];
    usize partCount_ = 0;
    uint16 tailOffset_ = 0;
    uint16 tailBytes_ = 0;
    char text_[MaxLogLayoutTextBytes]{};
    LogLayoutOptions options_;
};

} // namespace rocks
