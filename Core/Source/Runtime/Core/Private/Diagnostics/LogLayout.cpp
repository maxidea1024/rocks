#include <Core/Diagnostics/LogLayout.h>
#include <Core/Text/FormatParse.h>
#include <Core/Text/StringBuilder.h>
#include <Core/Time/DateTime.h>
#include <Platform/Clock.h>

namespace rocks {
namespace {

// 형식 표지(Part::variant)다. 0은 형식 없음이다.
constexpr uint8 VariantMilliseconds = 1;
constexpr uint8 VariantUpper3 = 2;

// 숫자·시각 한 토큰을 렌더할 스택 버퍼 크기다(ISO 8601 시각 28자, 64-bit 10진 20자보다 크다).
constexpr usize TokenBufferBytes = 48;

constexpr StringView Upper3Names[] = {"TRC", "DBG", "INF", "WRN", "ERR", "FTL"};

static_assert(sizeof(Upper3Names) / sizeof(Upper3Names[0]) == LogLevelCount,
              "every LogLevel needs a three-letter name");

// 조각을 넘기며 마지막으로 쓴 문자를 기억한다(붙은 토큰 사이 공백 판단, 생략 규칙 3).
class Writer {
public:
    explicit Writer(const LogLayoutOutput& out) noexcept : out_(out) {
    }

    void Append(StringView text) noexcept {
        if (text.IsEmpty()) {
            return;
        }

        out_.append(out_.context, text);
        last_ = text.Data()[text.ByteCount() - 1];
    }

    bool EndsWithSpace() const noexcept {
        return last_ == 0 || last_ == ' ' || last_ == '\n' || last_ == '\t';
    }

private:
    const LogLayoutOutput& out_;
    char last_ = 0;
};

// 서식 하나를 스택 버퍼에 렌더해 넘긴다.
template <typename... Args>
void AppendFormatted(Writer& writer, StringView format, const Args&... args) noexcept {
    char storage[TokenBufferBytes];
    FixedStringBuilder text{Span<char>(storage)};
    if (text.TryAppendFormat(format, args...)) {
        writer.Append(text.View());
    }
}

bool HasCorrelationIds(const LogEvent& event) noexcept {
    return event.hasCorrelation && !IsEmptyCorrelation(event.correlation);
}

// "session=3 task=1842"(0인 칸은 뺀다)
void AppendCorrelation(Writer& writer, const LogEvent& event) noexcept {
    uint64 values[CorrelationFieldCount];
    CorrelationFieldValues(event.correlation, values);
    bool first = true;
    for (usize index = 0; index < CorrelationFieldCount; ++index) {
        if (values[index] == 0) {
            continue;
        }

        AppendFormatted(writer, first ? "{}={}" : " {}={}", CorrelationFieldNames[index],
                        values[index]);
        first = false;
    }
}

// 템플릿 자리에 쓰인 속성인지다. 이름 자리는 "{이름}"·"{이름:"·"{@이름}"이 템플릿에 있으면, 위치
// 자리(Arg0…)는 검증된 템플릿이 모든 인자를 쓰므로 늘 쓰인 것이다.
bool IsPropertyUsed(StringView templateText, StringView name) noexcept {
    if (name.StartsWith("Arg", StringComparison::CaseSensitive)) {
        return true;
    }

    const char* data = templateText.Data();
    const usize bytes = templateText.ByteCount();
    for (usize index = 0; index + name.ByteCount() + 1 < bytes; ++index) {
        if (data[index] != '{') {
            continue;
        }

        // 구조체 펼치기 자리("{@이름}")는 '@'를 건너뛰고 비교한다.
        const usize nameAt = data[index + 1] == '@' ? index + 2 : index + 1;
        if (nameAt + name.ByteCount() >= bytes ||
            StringView(data + nameAt, name.ByteCount()) != name) {
            continue;
        }

        const char next = data[nameAt + name.ByteCount()];
        if (next == '}' || next == ':') {
            return true;
        }
    }

    return false;
}

bool HasUnusedProperties(const LogEvent& event) noexcept {
    for (const LogProperty& property : event.properties) {
        if (!IsPropertyUsed(event.messageTemplate, property.name)) {
            return true;
        }
    }

    return false;
}

void AppendPairs(Writer& writer, Span<const LogProperty> pairs, StringView templateText,
                 bool unusedOnly) noexcept {
    bool first = true;
    for (const LogProperty& pair : pairs) {
        if (unusedOnly && IsPropertyUsed(templateText, pair.name)) {
            continue;
        }

        if (!first) {
            writer.Append(" ");
        }

        writer.Append(pair.name);
        writer.Append("=");
        writer.Append(pair.text);
        first = false;
    }
}

} // namespace

Result<LogLayout> LogLayout::Parse(StringView pattern, const LogLayoutOptions& options,
                                   LogLayoutParseError* error) noexcept {
    static_assert(static_cast<usize>(Token::Correlation) + 1 == detail::LogLayoutTokenCount,
                  "every layout token needs a name");
    static_assert(static_cast<usize>(Token::Timestamp) == detail::LogLayoutTimestampToken &&
                      static_cast<usize>(Token::Level) == detail::LogLayoutLevelToken,
                  "layout token numbers must match the name table");

    // 문법은 상수 평가용 검사와 같은 함수로 먼저 본다. 아래 해석은 통과한 패턴만 받는다.
    LogLayoutParseError scanned;
    if (!detail::ScanLogLayout(pattern, scanned)) {
        if (error != nullptr) {
            *error = scanned;
        }

        return Result<LogLayout>::Failure(scanned.code);
    }

    LogLayout layout;
    layout.options_ = options;
    if (layout.options_.startMonotonicNanoseconds == 0) {
        layout.options_.startMonotonicNanoseconds = platform::MonotonicNanoseconds();
    }

    usize textBytes = 0;
    usize literalStart = 0;
    detail::FormatParser parser(pattern, true);
    detail::FormatToken token;
    while (parser.Next(token)) {
        if (token.kind == detail::FormatTokenKind::Literal) {
            if (token.text.ByteCount() > MaxLogLayoutTextBytes - textBytes) {
                return Result<LogLayout>::Failure(ErrorCode::CapacityExceeded);
            }

            for (usize index = 0; index < token.text.ByteCount(); ++index) {
                layout.text_[textBytes++] = token.text.Data()[index];
            }

            continue;
        }

        // 위치 자리("{}"·"{0}")는 레이아웃 토큰이 아니다.
        if (token.name.IsEmpty()) {
            return Result<LogLayout>::Failure(ErrorCode::InvalidArgument);
        }

        if (layout.partCount_ == MaxLogLayoutTokens) {
            return Result<LogLayout>::Failure(ErrorCode::CapacityExceeded);
        }

        bool known = false;
        Part part;
        for (usize index = 0; index < detail::LogLayoutTokenCount; ++index) {
            if (detail::LogLayoutTokenNames[index] == token.name) {
                part.token = static_cast<Token>(index);
                known = true;
                break;
            }
        }

        if (!known) {
            return Result<LogLayout>::Failure(ErrorCode::InvalidArgument);
        }

        if (token.hasSpec) {
            if (part.token == Token::Timestamp && token.spec == "ms") {
                part.variant = VariantMilliseconds;
            } else if (part.token == Token::Level && token.spec == "u3") {
                part.variant = VariantUpper3;
            } else {
                return Result<LogLayout>::Failure(ErrorCode::InvalidArgument);
            }
        }

        part.literalOffset = static_cast<uint16>(literalStart);
        part.literalBytes = static_cast<uint16>(textBytes - literalStart);
        layout.parts_[layout.partCount_++] = part;
        literalStart = textBytes;
    }

    if (parser.Error() != detail::FormatParseError::None) {
        return Result<LogLayout>::Failure(ErrorCode::InvalidArgument);
    }

    layout.tailOffset_ = static_cast<uint16>(literalStart);
    layout.tailBytes_ = static_cast<uint16>(textBytes - literalStart);
    return Result<LogLayout>::Success(layout);
}

LogLayout LogLayout::DefaultConsole() noexcept {
    // 상수 패턴이라 실패하지 않는다(시험이 확인한다).
    auto layout = Parse(DefaultConsoleLogLayout);
    return layout ? layout.Value() : LogLayout();
}

bool LogLayout::IsEmpty(const Part& part, const LogEvent& event) const noexcept {
    switch (part.token) {
    case Token::Timestamp:
        return event.utcTicks <= 0;
    case Token::Elapsed:
    case Token::Level:
    case Token::Frame:
    case Token::NewLine:
        return false;
    case Token::Category:
        return event.category.IsEmpty();
    case Token::EventId:
        return event.eventId == 0;
    case Token::Thread:
        return event.threadName.IsEmpty() && event.threadId == 0;
    case Token::Message:
        return event.message.IsEmpty();
    case Token::Template:
        return event.messageTemplate.IsEmpty();
    case Token::Properties:
        return !HasUnusedProperties(event);
    case Token::Context:
        return event.context.Size() == 0;
    case Token::Correlation:
        return !HasCorrelationIds(event);
    case Token::Location:
        return event.level < options_.locationMinimum || event.location.file == nullptr ||
               event.location.file[0] == '\0';
    }

    return true;
}

void LogLayout::EmitToken(const Part& part, const LogEvent& event, void* raw) const noexcept {
    Writer& writer = *static_cast<Writer*>(raw);
    switch (part.token) {
    case Token::Timestamp: {
        auto time = DateTime::TryFromTicks(event.utcTicks);
        if (time) {
            AppendFormatted(writer, part.variant == VariantMilliseconds ? "{:ms}" : "{}",
                            time.Value());
        }

        return;
    }
    case Token::Elapsed: {
        constexpr uint64 NanosecondsPerMillisecond = 1000000;
        constexpr uint64 MillisecondsPerSecond = 1000;
        const uint64 start = options_.startMonotonicNanoseconds;
        const uint64 elapsed =
            event.monotonicNanoseconds > start
                ? (event.monotonicNanoseconds - start) / NanosecondsPerMillisecond
                : 0;
        AppendFormatted(writer, "{}.{:03}", elapsed / MillisecondsPerSecond,
                        elapsed % MillisecondsPerSecond);
        return;
    }
    case Token::Level:
        if (IsValidLogLevel(event.level)) {
            writer.Append(part.variant == VariantUpper3
                              ? Upper3Names[static_cast<usize>(event.level)]
                              : LogLevelName(event.level));
        }

        return;
    case Token::Category:
        writer.Append(event.category);
        return;
    case Token::EventId:
        AppendFormatted(writer, "{:016x}", event.eventId);
        return;
    case Token::Thread:
        if (!event.threadName.IsEmpty()) {
            writer.Append(event.threadName);
        } else {
            AppendFormatted(writer, "{}", event.threadId);
        }

        return;
    case Token::Frame:
        AppendFormatted(writer, "{}", event.frame);
        return;
    case Token::Message:
        writer.Append(event.message);
        return;
    case Token::Template:
        writer.Append(event.messageTemplate);
        return;
    case Token::Properties:
        AppendPairs(writer, event.properties, event.messageTemplate, true);
        return;
    case Token::Context:
        AppendPairs(writer, event.context, StringView(), false);
        return;
    case Token::Location:
        writer.Append(StringView(event.location.file));
        AppendFormatted(writer, ":{}", event.location.line);
        if (event.location.function != nullptr && event.location.function[0] != '\0') {
            writer.Append(" ");
            writer.Append(StringView(event.location.function));
        }

        return;
    case Token::NewLine:
        writer.Append("\n");
        return;
    case Token::Correlation:
        AppendCorrelation(writer, event);
        return;
    }
}

void LogLayout::Render(const LogEvent& event, const LogLayoutOutput& out) const noexcept {
    if (out.append == nullptr) {
        return;
    }

    Writer writer(out);
    bool dropBracket = false; // 다음 리터럴의 첫 ']'를 뺀다(규칙 1)
    bool afterToken = false;  // 마지막 출력이 토큰이고 그 뒤 리터럴이 아직 없다(규칙 3)
    for (usize index = 0; index < partCount_; ++index) {
        const Part& part = parts_[index];
        StringView literal = Literal(part.literalOffset, part.literalBytes);
        if (dropBracket && !literal.IsEmpty() && literal.Data()[0] == ']') {
            literal = StringView(literal.Data() + 1, literal.ByteCount() - 1);
        }

        dropBracket = false;
        const bool empty = IsEmpty(part, event);
        if (empty) {
            const StringView next =
                index + 1 < partCount_
                    ? Literal(parts_[index + 1].literalOffset, parts_[index + 1].literalBytes)
                    : Literal(tailOffset_, tailBytes_);
            const usize bytes = literal.ByteCount();
            if (bytes != 0 && literal.Data()[bytes - 1] == '[' && !next.IsEmpty() &&
                next.Data()[0] == ']') {
                literal = StringView(literal.Data(), bytes - 1);
                dropBracket = true;
            } else if (bytes != 0 && literal.Data()[bytes - 1] == ' ') {
                literal = StringView(literal.Data(), bytes - 1);
            }

            if (!literal.IsEmpty()) {
                writer.Append(literal);
                afterToken = false;
            }

            continue;
        }

        if (!literal.IsEmpty()) {
            writer.Append(literal);
            afterToken = false;
        }

        if (afterToken && part.token != Token::NewLine && !writer.EndsWithSpace()) {
            writer.Append(" ");
        }

        EmitToken(part, event, &writer);
        afterToken = true;
    }

    StringView tail = Literal(tailOffset_, tailBytes_);
    if (dropBracket && !tail.IsEmpty() && tail.Data()[0] == ']') {
        tail = StringView(tail.Data() + 1, tail.ByteCount() - 1);
    }

    writer.Append(tail);
}

} // namespace rocks
