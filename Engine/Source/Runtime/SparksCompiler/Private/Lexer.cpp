#include <SparksCompiler/Lexer.h>

#include <Core/Text/NumberText.h>
#include <Core/Text/StringBuilder.h>
#include <SparksCompiler/Literal.h>
#include <Platform/Fatal.h>
#include <Platform/Memory.h>

#include "TextColumns.h"

namespace rocks::sparks {

namespace {

// 진단 문장을 담을 builder의 inline 크기다(넘치면 allocator에서 늘린다).
constexpr usize MessageInlineBytes = 160;

// `\u{…}`의 16진 자리 수 상한과 Unicode scalar 범위다(3.6절).
constexpr usize MaxUnicodeEscapeDigits = 6;
constexpr uint32 MaxUnicodeScalar = 0x10FFFFu;
constexpr uint32 SurrogateFirst = 0xD800u;
constexpr uint32 SurrogateLast = 0xDFFFu;

// 어휘 진단 코드 범위다(같은 줄 중복 억제 표의 크기).
constexpr uint32 FirstLexicalCode = 104;
constexpr uint32 LastLexicalCode = 114;
constexpr usize LexicalCodeCount = LastLexicalCode - FirstLexicalCode + 1;

struct Keyword {
    StringView text;
    StringView quoted; // 진단 문장용 표기
    TokenKind kind;
};

constexpr Keyword Keywords[] = {
    {"import", "'import'", TokenKind::Import},
    {"as", "'as'", TokenKind::As},
    {"class", "'class'", TokenKind::Class},
    {"struct", "'struct'", TokenKind::Struct},
    {"enum", "'enum'", TokenKind::Enum},
    {"event", "'event'", TokenKind::Event},
    {"fn", "'fn'", TokenKind::Fn},
    {"async", "'async'", TokenKind::Async},
    {"const", "'const'", TokenKind::Const},
    {"var", "'var'", TokenKind::Var},
    {"let", "'let'", TokenKind::Let},
    {"if", "'if'", TokenKind::If},
    {"else", "'else'", TokenKind::Else},
    {"for", "'for'", TokenKind::For},
    {"in", "'in'", TokenKind::In},
    {"while", "'while'", TokenKind::While},
    {"switch", "'switch'", TokenKind::Switch},
    {"case", "'case'", TokenKind::Case},
    {"default", "'default'", TokenKind::Default},
    {"break", "'break'", TokenKind::Break},
    {"continue", "'continue'", TokenKind::Continue},
    {"return", "'return'", TokenKind::Return},
    {"on", "'on'", TokenKind::On},
    {"emit", "'emit'", TokenKind::Emit},
    {"await", "'await'", TokenKind::Await},
    {"try", "'try'", TokenKind::Try},
    {"override", "'override'", TokenKind::Override},
    {"this", "'this'", TokenKind::This},
    {"true", "'true'", TokenKind::True},
    {"false", "'false'", TokenKind::False},
    {"null", "'null'", TokenKind::Null},
    {"interface", "'interface'", TokenKind::Interface},
    {"open", "'open'", TokenKind::Open},
};

constexpr bool IsAsciiLetter(char value) noexcept {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z');
}

constexpr bool IsDigit(char value) noexcept {
    return value >= '0' && value <= '9';
}

constexpr bool IsHexDigit(char value) noexcept {
    return IsDigit(value) || (value >= 'a' && value <= 'f') || (value >= 'A' && value <= 'F');
}

constexpr bool IsNameCharacter(char value) noexcept {
    return IsAsciiLetter(value) || IsDigit(value) || value == '_';
}

constexpr uint32 HexValue(char value) noexcept {
    if (IsDigit(value)) {
        return static_cast<uint32>(value - '0');
    }

    constexpr uint32 LetterBase = 10;
    return static_cast<uint32>((value | 0x20) - 'a') + LetterBase;
}

// C# 숫자 접미사인지다(대소문자 무관): u, l, ul, lu, f, d, m.
bool IsNumericSuffix(StringView letters) noexcept {
    constexpr StringView Suffixes[] = {"u", "l", "ul", "lu", "f", "d", "m"};
    constexpr char LowerCaseBit = 0x20;
    for (const StringView suffix : Suffixes) {
        if (suffix.ByteCount() != letters.ByteCount()) {
            continue;
        }

        bool same = true;
        for (usize index = 0; index < suffix.ByteCount(); ++index) {
            same = same && (letters.Data()[index] | LowerCaseBit) == suffix.Data()[index];
        }

        if (same) {
            return true;
        }
    }

    return false;
}

TokenKind KeywordKind(StringView text) noexcept {
    for (const Keyword& keyword : Keywords) {
        if (keyword.text == text) {
            return keyword.kind;
        }
    }

    return TokenKind::Identifier;
}

// 어휘 분석 상태다. 문자열과 보간은 중첩될 수 있으므로 모드 stack을 둔다.
class Lexer final {
public:
    Lexer(const SourceFile& file, SourceId id, DiagnosticList& diagnostics, Array<Token>& tokens,
          Allocator& allocator) noexcept
        : file_(file), text_(file.Text()), id_(id), diagnostics_(diagnostics), tokens_(tokens),
          allocator_(allocator), modes_(allocator) {
        for (uint32& line : reportedLine_) {
            line = 0;
        }
    }

    Result<void> Run() noexcept {
        offset_ = file_.ContentBegin();
        while (true) {
            Result<void> step = modes_.IsEmpty() || modes_[modes_.Size() - 1].kind == ModeKind::Code
                                    ? LexCode()
                                    : LexStringPart();
            if (!step) {
                return step;
            }

            if (finished_) {
                return Result<void>::Success();
            }
        }
    }

private:
    enum class ModeKind : uint8 {
        String, // 문자열 안
        Code    // 보간 `${ }` 안(braceDepth는 그 안에서 연 `{` 수)
    };

    struct Mode {
        ModeKind kind = ModeKind::Code;
        uint32 stringBegin = 0; // 이 문자열(또는 보간을 둘러싼 문자열)의 여는 `"` 위치
        uint32 braceDepth = 0;
    };

    bool AtEnd() const noexcept {
        return offset_ >= text_.ByteCount();
    }

    char Peek(usize ahead = 0) const noexcept {
        const usize index = offset_ + ahead;
        return index < text_.ByteCount() ? text_.Data()[index] : '\0';
    }

    bool InInterpolation() const noexcept {
        return !modes_.IsEmpty();
    }

    Result<void> Emit(TokenKind kind, usize begin, usize end) noexcept {
        Token token;
        token.kind = kind;
        token.span = SourceSpan{static_cast<uint32>(begin), static_cast<uint32>(end)};
        return tokens_.TryPushBack(token);
    }

    // 같은 줄에서 같은 코드는 한 번만 낸다(연쇄 오류 억제).
    Result<void> Report(DiagnosticCode code, usize begin, usize end, StringView message,
                        StringView explanation, StringView fix) noexcept {
        const uint32 number = static_cast<uint16>(code);
        if (number < FirstLexicalCode || number > LastLexicalCode) {
            platform::Fatal("Sparks lexer reported a non-lexical code");
        }

        const uint32 line = file_.Locate(static_cast<uint32>(begin)).line;
        uint32& last = reportedLine_[number - FirstLexicalCode];
        if (last == line) {
            return Result<void>::Success();
        }

        last = line;
        return diagnostics_.TryAdd(code, id_,
                                   SourceSpan{static_cast<uint32>(begin), static_cast<uint32>(end)},
                                   message, explanation, fix);
    }

    // 닫히지 않은 문자열을 보고하고, 그 문자열(과 그 안의 보간)을 모두 닫는다.
    Result<void> CloseUnterminatedStrings() noexcept {
        while (!modes_.IsEmpty()) {
            const Mode mode = modes_[modes_.Size() - 1];
            static_cast<void>(modes_.TryPopBack());
            if (mode.kind == ModeKind::Code) {
                continue;
            }

            if (auto reported = Report(DiagnosticCode::UnterminatedString, mode.stringBegin,
                                       mode.stringBegin + 1, "String literal is not closed.",
                                       "A string ends with '\"' on the same line.",
                                       "Close the string with '\"' before the end of the line.");
                !reported) {
                return reported;
            }

            if (auto emitted = Emit(TokenKind::Invalid, offset_, offset_); !emitted) {
                return emitted;
            }

            if (auto emitted = Emit(TokenKind::StringEnd, offset_, offset_); !emitted) {
                return emitted;
            }
        }

        return Result<void>::Success();
    }

    // 문자열 밖(코드 또는 보간 안) 토큰 하나를 읽는다.
    Result<void> LexCode() noexcept {
        while (!AtEnd()) {
            const char value = Peek();
            if (value == '\n' && InInterpolation()) {
                // 문자열은 한 줄이다. 보간 안에서 줄이 바뀌면 둘러싼 문자열이 닫히지 않은 것이다.
                return CloseUnterminatedStrings();
            }

            if (value != ' ' && value != '\t' && value != '\n' && value != '\r') {
                break;
            }

            ++offset_;
        }

        if (AtEnd()) {
            if (auto closed = CloseUnterminatedStrings(); !closed) {
                return closed;
            }

            finished_ = true;
            return Emit(TokenKind::EndOfFile, offset_, offset_);
        }

        const usize begin = offset_;
        const char value = Peek();
        if (value == '/' && Peek(1) == '/') {
            return LexLineComment();
        }

        if (value == '/' && Peek(1) == '*') {
            return LexBlockComment();
        }

        if (IsAsciiLetter(value) || value == '_') {
            return LexName();
        }

        if (IsDigit(value)) {
            return LexNumber();
        }

        if (value == '"') {
            return BeginString(begin);
        }

        if (value == '@' && Peek(1) == '"') {
            return LexVerbatimString(begin);
        }

        if (value == '.' && IsDigit(Peek(1))) {
            return LexNumber();
        }

        return LexPunctuation();
    }

    Result<void> LexLineComment() noexcept {
        const usize begin = offset_;
        usize end = begin;
        while (end < text_.ByteCount() && text_.Data()[end] != '\n') {
            ++end;
        }

        offset_ = end;
        // 문서 주석은 정확히 `///` 뒤에 `/`가 없는 경우다. 줄 끝의 CR은 범위에서 뺀다.
        const bool documentation = end - begin >= 3 && text_.Data()[begin + 2] == '/' &&
                                   (end - begin == 3 || text_.Data()[begin + 3] != '/');
        if (!documentation) {
            return Result<void>::Success();
        }

        usize documentEnd = end;
        if (documentEnd > begin && text_.Data()[documentEnd - 1] == '\r') {
            --documentEnd;
        }

        return Emit(TokenKind::DocComment, begin, documentEnd);
    }

    Result<void> LexBlockComment() noexcept {
        const usize begin = offset_;
        usize cursor = begin + 2;
        while (cursor + 1 < text_.ByteCount()) {
            if (text_.Data()[cursor] == '*' && text_.Data()[cursor + 1] == '/') {
                offset_ = cursor + 2;
                return Result<void>::Success();
            }

            ++cursor;
        }

        offset_ = text_.ByteCount();
        if (auto reported = Report(DiagnosticCode::UnterminatedBlockComment, begin, begin + 2,
                                   "Block comment is not closed.",
                                   "A block comment starts with '/*' and ends with '*/'; block "
                                   "comments do not nest.",
                                   "Add '*/' where the comment ends.");
            !reported) {
            return reported;
        }

        return Emit(TokenKind::Invalid, begin, begin + 2);
    }

    Result<void> LexName() noexcept {
        const usize begin = offset_;
        while (!AtEnd() && IsNameCharacter(Peek())) {
            ++offset_;
        }

        const StringView name(text_.Data() + begin, offset_ - begin);
        if (name == StringView("_")) {
            return Emit(TokenKind::Underscore, begin, offset_);
        }

        bool underscore = false;
        for (const char character : name) {
            underscore = underscore || character == '_';
        }

        if (underscore) {
            if (auto reported = ReportUnderscore(name, begin); !reported) {
                return reported;
            }

            return Emit(TokenKind::Identifier, begin, offset_);
        }

        const TokenKind kind = KeywordKind(name);
        if (kind == TokenKind::As && Peek() == '?') {
            ++offset_;
            return Emit(TokenKind::AsQuestion, begin, offset_);
        }

        return Emit(kind, begin, offset_);
    }

    // `move_speed` → `moveSpeed`, `_x` → `x`처럼 `_`를 빼고 그 뒤 글자를 대문자로 바꾼 이름을
    // 제시한다.
    Result<void> ReportUnderscore(StringView name, usize begin) noexcept {
        StringBuilder<MessageInlineBytes> fix(allocator_);
        if (auto appended = fix.TryAppend("Rename to '"); !appended) {
            return appended;
        }

        bool upperNext = false;
        bool wroteAny = false;
        for (const char character : name) {
            if (character == '_') {
                upperNext = wroteAny;
                continue;
            }

            char output = character;
            if (upperNext && output >= 'a' && output <= 'z') {
                output = static_cast<char>(output - 'a' + 'A');
            }

            upperNext = false;
            wroteAny = true;
            if (auto appended = fix.TryAppendAscii(output); !appended) {
                return appended;
            }
        }

        if (!wroteAny) {
            fix.Clear();
            if (auto appended = fix.TryAppend("Use a name made of letters and digits"); !appended) {
                return appended;
            }
        } else if (auto appended = fix.TryAppend("'"); !appended) {
            return appended;
        }

        if (auto appended = fix.TryAppend("."); !appended) {
            return appended;
        }

        return Report(DiagnosticCode::UnderscoreInIdentifier, begin, offset_,
                      "Identifiers cannot contain '_'.",
                      "Types and members use PascalCase and local names use lowerCamel; a lone "
                      "'_' discards a value.",
                      fix.View());
    }

    // 숫자 자리와 구분자 `_`를 읽은 결과다.
    struct DigitRun {
        bool digits = false;       // 숫자가 하나라도 있었다
        bool badSeparator = false; // `_`가 맨 앞(허용되지 않을 때)이나 맨 끝에 있었다
    };

    // isDigit에 맞는 숫자와 `_`를 읽는다. `_`는 숫자 사이에 몇 개든 올 수 있고,
    // allowLeadingSeparator면(16진·2진 접두사 바로 뒤) 맨 앞에도 올 수 있다.
    DigitRun ScanDigits(bool (*isDigit)(char), bool allowLeadingSeparator) noexcept {
        DigitRun run;
        bool lastWasSeparator = false;
        if (Peek() == '_' && !allowLeadingSeparator) {
            run.badSeparator = true;
        }

        while (!AtEnd()) {
            const char value = Peek();
            if (value == '_') {
                lastWasSeparator = true;
            } else if (isDigit(value)) {
                run.digits = true;
                lastWasSeparator = false;
            } else {
                break;
            }

            ++offset_;
        }

        if (lastWasSeparator) {
            run.badSeparator = true;
        }

        return run;
    }

    // 숫자 리터럴 하나를 읽는다(3.6절, SD54 — 최신 C# 형태, 접미사 없음).
    Result<void> LexNumber() noexcept {
        const usize begin = offset_;
        StringView problem; // S0110 설명(비어 있으면 형식 문제 없음)
        StringView problemFix;
        bool isFloat = false;
        bool badSeparator = false;
        const bool prefixed =
            Peek() == '0' && (Peek(1) == 'x' || Peek(1) == 'X' || Peek(1) == 'b' || Peek(1) == 'B');
        if (prefixed) {
            const bool hex = Peek(1) == 'x' || Peek(1) == 'X';
            offset_ += 2;
            // 2진도 0–9를 모두 읽어 둔 뒤 2–9가 있으면 알린다(`0b102`가 `0b10`과 `2`로 나뉘지
            // 않게).
            const usize digitsBegin = offset_;
            const DigitRun run = ScanDigits(hex ? IsHexDigit : IsDigit, true);
            badSeparator = run.badSeparator;
            if (!run.digits) {
                problem =
                    hex ? StringView("A hexadecimal literal needs at least one digit after '0x'.")
                        : StringView("A binary literal needs at least one digit after '0b'.");
                problemFix = hex ? StringView("Write '0xFF'.") : StringView("Write '0b1010'.");
            } else if (!hex) {
                for (usize index = digitsBegin; index < offset_; ++index) {
                    const char digit = text_.Data()[index];
                    if (digit != '0' && digit != '1' && digit != '_') {
                        problem = "Binary literals use only the digits 0 and 1.";
                        problemFix = "Write '0b1010'.";
                        break;
                    }
                }
            }
        } else {
            if (Peek() == '.') {
                // `.5`(C#과 같다).
                ++offset_;
                isFloat = true;
            }

            badSeparator = ScanDigits(IsDigit, false).badSeparator;
            if (!isFloat && Peek() == '.' && (IsDigit(Peek(1)) || Peek(1) == '_')) {
                ++offset_;
                isFloat = true;
                badSeparator = ScanDigits(IsDigit, false).badSeparator || badSeparator;
            } else if (!isFloat && Peek() == '.' && Peek(1) != '.' && !IsAsciiLetter(Peek(1))) {
                // `1.`: 소수점 뒤에 숫자가 없다(`0..<n`의 `..`와 멤버 접근 `.Name`은 제외).
                ++offset_;
                isFloat = true;
                problem = "A decimal point must be followed by a digit.";
                problemFix = "Write '1.0' instead of '1.'.";
            }

            if (Peek() == 'e' || Peek() == 'E') {
                isFloat = true;
                ++offset_;
                if (Peek() == '+' || Peek() == '-') {
                    ++offset_;
                }

                const DigitRun exponent = ScanDigits(IsDigit, false);
                badSeparator = badSeparator || exponent.badSeparator;
                if (!exponent.digits && problem.IsEmpty()) {
                    problem = "An exponent needs at least one digit.";
                    problemFix = "Write '1.0e3'.";
                }
            }
        }

        if (badSeparator && problem.IsEmpty()) {
            problem = "Digit separators '_' go between digits (or right after '0x' or '0b').";
            problemFix = "Remove the misplaced '_'.";
        }

        // 숫자 바로 뒤의 영문자: C# 접미사면 S0114, 아니면 S0110.
        bool suffix = false;
        if (!AtEnd() && IsNameCharacter(Peek())) {
            const usize lettersBegin = offset_;
            while (!AtEnd() && IsNameCharacter(Peek())) {
                ++offset_;
            }

            suffix =
                IsNumericSuffix(StringView(text_.Data() + lettersBegin, offset_ - lettersBegin));
            if (!suffix && problem.IsEmpty()) {
                problem = "A number cannot be followed directly by a letter or '_'.";
                problemFix = "Insert a space or remove the letters.";
            }
        }

        if (!problem.IsEmpty()) {
            if (auto reported = Report(DiagnosticCode::MalformedNumber, begin, offset_,
                                       "Malformed number literal.", problem, problemFix);
                !reported) {
                return reported;
            }
        } else if (suffix) {
            if (auto reported = Report(
                    DiagnosticCode::NumericSuffix, begin, offset_,
                    "Numeric suffixes are not used in Sparks.",
                    "Number literals take the type of the place where they are used, as in Go.",
                    "Remove the suffix; state the type in the declaration or convert, as in "
                    "'var x: uint8 = 1' or 'float32(1.5)'.");
                !reported) {
                return reported;
            }
        } else if (isFloat) {
            auto value = TryDecodeFloatLiteral(allocator_,
                                               StringView(text_.Data() + begin, offset_ - begin));
            if (!value && value.Error() != ErrorCode::Overflow) {
                return Result<void>::Failure(value.Error());
            }

            if (!value) {
                if (auto reported = Report(DiagnosticCode::FloatLiteralOutOfRange, begin, offset_,
                                           "Float literal is out of range.",
                                           "The value does not fit in a finite 64-bit float.",
                                           "Use a smaller value.");
                    !reported) {
                    return reported;
                }
            }
        }

        return Emit(isFloat ? TokenKind::FloatLiteral : TokenKind::IntegerLiteral, begin, offset_);
    }

    // `@"..."` verbatim 문자열 하나를 읽는다(SD55): escape·보간 없음, `""`는 `"`, 여러 줄 가능.
    Result<void> LexVerbatimString(usize begin) noexcept {
        if (InInterpolation()) {
            if (auto reported = Report(DiagnosticCode::StringInInterpolation, begin, begin + 2,
                                       "String literal inside an interpolation.",
                                       "'${ }' cannot contain a string literal.",
                                       "Store the string in a local variable and interpolate the "
                                       "variable.");
                !reported) {
                return reported;
            }
        }

        offset_ += 2;
        while (!AtEnd()) {
            if (Peek() != '"') {
                ++offset_;
                continue;
            }

            if (Peek(1) == '"') {
                offset_ += 2;
                continue;
            }

            ++offset_;
            return Emit(TokenKind::VerbatimString, begin, offset_);
        }

        if (auto reported = Report(DiagnosticCode::UnterminatedString, begin, begin + 2,
                                   "Verbatim string literal is not closed.",
                                   "A verbatim string ends with '\"'; write '\"\"' for a quote "
                                   "inside it.",
                                   "Close the string with '\"'.");
            !reported) {
            return reported;
        }

        if (auto emitted = Emit(TokenKind::VerbatimString, begin, offset_); !emitted) {
            return emitted;
        }

        return Emit(TokenKind::Invalid, offset_, offset_);
    }

    Result<void> BeginString(usize begin) noexcept {
        if (InInterpolation()) {
            if (auto reported = Report(DiagnosticCode::StringInInterpolation, begin, begin + 1,
                                       "String literal inside an interpolation.",
                                       "'${ }' cannot contain a string literal.",
                                       "Store the string in a local variable and interpolate the "
                                       "variable.");
                !reported) {
                return reported;
            }
        }

        ++offset_;
        Mode mode;
        mode.kind = ModeKind::String;
        mode.stringBegin = static_cast<uint32>(begin);
        if (auto pushed = modes_.TryPushBack(mode); !pushed) {
            return pushed;
        }

        return Emit(TokenKind::StringBegin, begin, offset_);
    }

    // 문자열 안의 조각 하나(또는 끝·보간 시작)를 읽는다.
    Result<void> LexStringPart() noexcept {
        const Mode string = modes_[modes_.Size() - 1];
        const usize begin = offset_;
        while (!AtEnd()) {
            const char value = Peek();
            if (value == '"' || value == '\n' || (value == '$' && Peek(1) == '{')) {
                break;
            }

            if (value == '\\') {
                if (auto escaped = LexEscape(); !escaped) {
                    return escaped;
                }

                continue;
            }

            ++offset_;
        }

        if (offset_ > begin) {
            // 줄 끝 바로 앞의 CR은 조각에 넣지 않는다(닫히지 않은 문자열).
            usize end = offset_;
            if (Peek() == '\n' && text_.Data()[end - 1] == '\r') {
                --end;
            }

            if (end > begin) {
                if (auto emitted = Emit(TokenKind::StringText, begin, end); !emitted) {
                    return emitted;
                }
            }
        }

        if (AtEnd() || Peek() == '\n') {
            return CloseUnterminatedStrings();
        }

        if (Peek() == '"') {
            static_cast<void>(modes_.TryPopBack());
            ++offset_;
            return Emit(TokenKind::StringEnd, offset_ - 1, offset_);
        }

        // `${`: 보간을 연다.
        const usize interpolation = offset_;
        offset_ += 2;
        Mode code;
        code.kind = ModeKind::Code;
        code.stringBegin = string.stringBegin;
        if (auto pushed = modes_.TryPushBack(code); !pushed) {
            return pushed;
        }

        return Emit(TokenKind::InterpolationBegin, interpolation, offset_);
    }

    // `\`로 시작하는 escape 하나를 검사하며 건너뛴다(값은 구문 분석이 푼다).
    Result<void> LexEscape() noexcept {
        const usize begin = offset_;
        ++offset_;
        const char kind = Peek();
        if (kind == '"' || kind == '\\' || kind == 'n' || kind == 't' || kind == '$') {
            ++offset_;
            return Result<void>::Success();
        }

        bool valid = false;
        if (kind == 'u' && Peek(1) == '{') {
            offset_ += 2;
            usize digits = 0;
            uint32 value = 0;
            while (!AtEnd() && IsHexDigit(Peek())) {
                if (digits < MaxUnicodeEscapeDigits) {
                    value = (value << 4u) | HexValue(Peek());
                }

                ++digits;
                ++offset_;
            }

            if (Peek() == '}') {
                ++offset_;
                valid = digits >= 1 && digits <= MaxUnicodeEscapeDigits &&
                        value <= MaxUnicodeScalar &&
                        (value < SurrogateFirst || value > SurrogateLast);
            }
        } else if (!AtEnd() && Peek() != '\n') {
            usize width = 0;
            static_cast<void>(detail::DecodeCodePoint(text_.Data() + offset_,
                                                      text_.ByteCount() - offset_, width));
            offset_ += width;
        }

        if (valid) {
            return Result<void>::Success();
        }

        return Report(
            DiagnosticCode::InvalidEscape, begin, offset_, "Invalid escape sequence.",
            "Allowed escapes are \\\", \\\\, \\n, \\t, \\$ and \\u{X} with 1 to 6 hex digits "
            "naming a Unicode scalar value.",
            "Write '\\\\' for a backslash.");
    }

    Result<void> LexPunctuation() noexcept {
        const usize begin = offset_;
        const char value = Peek();
        const char next = Peek(1);
        TokenKind kind = TokenKind::EndOfFile;
        usize width = 1;
        switch (value) {
        case '{':
            kind = TokenKind::LeftBrace;
            if (InInterpolation()) {
                ++modes_[modes_.Size() - 1].braceDepth;
            }
            break;
        case '}':
            if (InInterpolation() && modes_[modes_.Size() - 1].braceDepth == 0) {
                static_cast<void>(modes_.TryPopBack());
                ++offset_;
                return Emit(TokenKind::InterpolationEnd, begin, offset_);
            }

            if (InInterpolation()) {
                --modes_[modes_.Size() - 1].braceDepth;
            }

            kind = TokenKind::RightBrace;
            break;
        case '(':
            kind = TokenKind::LeftParen;
            break;
        case ')':
            kind = TokenKind::RightParen;
            break;
        case '[':
            kind = TokenKind::LeftBracket;
            break;
        case ']':
            kind = TokenKind::RightBracket;
            break;
        case ';':
            kind = TokenKind::Semicolon;
            break;
        case ',':
            kind = TokenKind::Comma;
            break;
        case ':':
            kind = TokenKind::Colon;
            break;
        case '@':
            kind = TokenKind::At;
            break;
        case '.':
            if (next == '.') {
                width = Peek(2) == '<' ? 3 : 2;
                kind = TokenKind::DotDotLess;
                if (width == 2) {
                    offset_ += width;
                    if (auto reported = Report(
                            DiagnosticCode::RangeOperator, begin, offset_, "Use '..<' for ranges.",
                            "Sparks has a single range form 'a..<b' that excludes the end.",
                            "Write 'a..<b'.");
                        !reported) {
                        return reported;
                    }

                    return Emit(kind, begin, offset_);
                }
            } else {
                kind = TokenKind::Dot;
            }
            break;
        case '?':
            if (next == '.') {
                kind = TokenKind::QuestionDot;
                width = 2;
            } else if (next == '?') {
                kind = TokenKind::QuestionQuestion;
                width = 2;
            } else {
                kind = TokenKind::Question;
            }
            break;
        case '=':
            kind = next == '=' ? TokenKind::EqualEqual : TokenKind::Equal;
            width = next == '=' ? 2 : 1;
            break;
        case '!':
            kind = next == '=' ? TokenKind::BangEqual : TokenKind::Bang;
            width = next == '=' ? 2 : 1;
            break;
        case '<':
            kind = next == '=' ? TokenKind::LessEqual : TokenKind::Less;
            width = next == '=' ? 2 : 1;
            break;
        case '>':
            kind = next == '=' ? TokenKind::GreaterEqual : TokenKind::Greater;
            width = next == '=' ? 2 : 1;
            break;
        case '+':
            kind = next == '=' ? TokenKind::PlusEqual : TokenKind::Plus;
            width = next == '=' ? 2 : 1;
            break;
        case '-':
            kind = next == '=' ? TokenKind::MinusEqual : TokenKind::Minus;
            width = next == '=' ? 2 : 1;
            break;
        case '*':
            kind = next == '=' ? TokenKind::StarEqual : TokenKind::Star;
            width = next == '=' ? 2 : 1;
            break;
        case '/':
            kind = next == '=' ? TokenKind::SlashEqual : TokenKind::Slash;
            width = next == '=' ? 2 : 1;
            break;
        case '%':
            kind = TokenKind::Percent;
            break;
        case '&':
            if (next == '&') {
                kind = TokenKind::AmpersandAmpersand;
                width = 2;
            }
            break;
        case '|':
            if (next == '|') {
                kind = TokenKind::PipePipe;
                width = 2;
            }
            break;
        default:
            break;
        }

        if (kind != TokenKind::EndOfFile) {
            offset_ += width;
            return Emit(kind, begin, offset_);
        }

        return ReportInvalidCharacter();
    }

    Result<void> ReportInvalidCharacter() noexcept {
        const usize begin = offset_;
        usize width = 0;
        const uint32 codePoint =
            detail::DecodeCodePoint(text_.Data() + offset_, text_.ByteCount() - offset_, width);
        offset_ += width;
        StringBuilder<MessageInlineBytes> message(allocator_);
        constexpr uint32 FirstPrintable = 0x20u;
        constexpr uint32 LastPrintable = 0x7Eu;
        const bool printable = codePoint >= FirstPrintable && codePoint <= LastPrintable;
        if (auto formatted =
                printable ? message.TryAppendFormat("Unexpected character '{}'.",
                                                    StringView(text_.Data() + begin, width))
                          : message.TryAppendFormat("Unexpected character U+{:04X}.", codePoint);
            !formatted) {
            return formatted;
        }

        StringView explanation = "This character has no meaning in Sparks code.";
        StringView fix = "Remove the character.";
        if (codePoint == '&' || codePoint == '|') {
            explanation = "Sparks has no bitwise operators.";
            fix = codePoint == '&' ? StringView("Use '&&' for logical and.")
                                   : StringView("Use '||' for logical or.");
        } else if (codePoint == '\'') {
            fix = "Strings use double quotes, as in \"text\".";
        } else if (codePoint > LastPrintable) {
            explanation = "Code uses ASCII only; other text belongs in strings and comments.";
            fix = "Remove the character or move the text into a string or comment.";
        }

        if (auto reported = Report(DiagnosticCode::InvalidCharacter, begin, offset_, message.View(),
                                   explanation, fix);
            !reported) {
            return reported;
        }

        return Emit(TokenKind::Invalid, begin, offset_);
    }

    const SourceFile& file_;
    StringView text_;
    SourceId id_;
    DiagnosticList& diagnostics_;
    Array<Token>& tokens_;
    Allocator& allocator_;
    Array<Mode> modes_;
    uint32 reportedLine_[LexicalCodeCount];
    usize offset_ = 0;
    bool finished_ = false;
};

} // namespace

StringView TokenKindText(TokenKind kind) noexcept {
    switch (kind) {
    case TokenKind::EndOfFile:
        return "end of file";
    case TokenKind::Identifier:
        return "identifier";
    case TokenKind::Underscore:
        return "'_'";
    case TokenKind::IntegerLiteral:
        return "integer literal";
    case TokenKind::FloatLiteral:
        return "float literal";
    case TokenKind::StringBegin:
        return "string literal";
    case TokenKind::StringText:
        return "string text";
    case TokenKind::InterpolationBegin:
        return "'${'";
    case TokenKind::InterpolationEnd:
        return "'}'";
    case TokenKind::StringEnd:
        return "'\"'";
    case TokenKind::DocComment:
        return "documentation comment";
    case TokenKind::VerbatimString:
        return "verbatim string literal";
    case TokenKind::Invalid:
        return "invalid text";
    case TokenKind::LeftBrace:
        return "'{'";
    case TokenKind::RightBrace:
        return "'}'";
    case TokenKind::LeftParen:
        return "'('";
    case TokenKind::RightParen:
        return "')'";
    case TokenKind::LeftBracket:
        return "'['";
    case TokenKind::RightBracket:
        return "']'";
    case TokenKind::Semicolon:
        return "';'";
    case TokenKind::Comma:
        return "','";
    case TokenKind::Dot:
        return "'.'";
    case TokenKind::QuestionDot:
        return "'?.'";
    case TokenKind::Question:
        return "'?'";
    case TokenKind::Colon:
        return "':'";
    case TokenKind::Equal:
        return "'='";
    case TokenKind::EqualEqual:
        return "'=='";
    case TokenKind::BangEqual:
        return "'!='";
    case TokenKind::Less:
        return "'<'";
    case TokenKind::LessEqual:
        return "'<='";
    case TokenKind::Greater:
        return "'>'";
    case TokenKind::GreaterEqual:
        return "'>='";
    case TokenKind::Plus:
        return "'+'";
    case TokenKind::Minus:
        return "'-'";
    case TokenKind::Star:
        return "'*'";
    case TokenKind::Slash:
        return "'/'";
    case TokenKind::Percent:
        return "'%'";
    case TokenKind::PlusEqual:
        return "'+='";
    case TokenKind::MinusEqual:
        return "'-='";
    case TokenKind::StarEqual:
        return "'*='";
    case TokenKind::SlashEqual:
        return "'/='";
    case TokenKind::Bang:
        return "'!'";
    case TokenKind::AmpersandAmpersand:
        return "'&&'";
    case TokenKind::PipePipe:
        return "'||'";
    case TokenKind::QuestionQuestion:
        return "'?"
               "?'"; // 두 조각으로 써서 trigraph(??')를 피한다
    case TokenKind::DotDotLess:
        return "'..<'";
    case TokenKind::At:
        return "'@'";
    case TokenKind::AsQuestion:
        return "'as?'";
    default:
        break;
    }

    for (const Keyword& keyword : Keywords) {
        if (keyword.kind == kind) {
            return keyword.quoted;
        }
    }

    return "token";
}

bool IsKeyword(TokenKind kind) noexcept {
    return kind >= TokenKind::Import && kind <= TokenKind::Open;
}

bool IsKeywordText(StringView text) noexcept {
    return KeywordKind(text) != TokenKind::Identifier;
}

Result<void> Tokenize(const SourceSet& sources, SourceId id, DiagnosticList& diagnostics,
                      Array<Token>& tokens) noexcept {
    const SourceFile& file = sources.Get(id);
    if (!file.IsUsable()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    Lexer lexer(file, id, diagnostics, tokens, tokens.GetAllocator());
    return lexer.Run();
}

} // namespace rocks::sparks
