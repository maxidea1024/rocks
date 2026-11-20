#include <Core/Move.h>
#include <Core/Text/NumberText.h>
#include <SparksCompiler/Diagnostic.h>

#include "ConformanceInternal.h"

// `.expect` 해석(Docs/Design/SparksLanguage.ko.md 13.1절)과 Report다.

namespace rocks::sparks::conformance {

namespace {

constexpr bool IsWordSeparator(char value) noexcept {
    return value == ' ' || value == '\t';
}

// 공백·tab으로 나눈 단어를 최대 capacity개 꺼낸다. 단어가 더 있으면 capacity + 1을 돌려준다.
usize SplitWords(StringView line, StringView* words, usize capacity) noexcept {
    usize count = 0;
    usize index = 0;
    while (index < line.ByteCount()) {
        while (index < line.ByteCount() && IsWordSeparator(line.Data()[index])) {
            ++index;
        }

        if (index == line.ByteCount()) {
            break;
        }

        const usize begin = index;
        while (index < line.ByteCount() && !IsWordSeparator(line.Data()[index])) {
            ++index;
        }

        if (count == capacity) {
            return capacity + 1;
        }

        words[count++] = StringView(line.Data() + begin, index - begin);
    }

    return count;
}

// 10진 숫자만으로 된 uint32다(부호·공백 없음). 형식이 틀리면 false.
bool ParseDecimal(StringView text, uint32& value) noexcept {
    if (text.IsEmpty()) {
        return false;
    }

    for (const char character : text) {
        if (character < '0' || character > '9') {
            return false;
        }
    }

    auto parsed = TryParseUInt32(text);
    if (!parsed) {
        return false;
    }

    value = parsed.Value();
    return true;
}

// "S0412" → 412. 형식이 틀리면 false.
bool ParseCode(StringView text, uint32& code) noexcept {
    if (text.ByteCount() != DiagnosticCodeDigits + 1 || text.Data()[0] != 'S') {
        return false;
    }

    return ParseDecimal(StringView(text.Data() + 1, DiagnosticCodeDigits), code);
}

// "14:14" → 줄·열(둘 다 1 이상). 형식이 틀리면 false.
bool ParsePosition(StringView text, uint32& line, uint32& column) noexcept {
    StringView left;
    StringView right;
    if (!text.Split(":", left, right, StringComparison::CaseSensitive)) {
        return false;
    }

    return ParseDecimal(left, line) && ParseDecimal(right, column) && line != 0 && column != 0;
}

// 위치 자리의 단어다: ':'가 있거나 숫자로만 되어 있다(경로에는 ':'를 쓰지 않는다).
bool IsPositionLike(StringView word) noexcept {
    bool digitsOnly = true;
    for (const char character : word) {
        if (character == ':') {
            return true;
        }

        digitsOnly = digitsOnly && character >= '0' && character <= '9';
    }

    return digitsOnly;
}

} // namespace

namespace detail {

Result<void> FormatExpected(StringBuilderBase& out, const ExpectedDiagnostic& expected) noexcept {
    if (auto appended = out.TryAppend("    error "); !appended) {
        return appended;
    }

    if (auto appended = AppendDiagnosticCode(out, expected.code); !appended) {
        return appended;
    }

    if (!expected.path.IsEmpty()) {
        if (auto appended = out.TryAppendFormat(" {}", expected.path); !appended) {
            return appended;
        }
    }

    if (expected.line == 0) {
        return out.TryAppendAscii('\n');
    }

    return out.TryAppendFormat(" {}:{}\n", expected.line, expected.column);
}

} // namespace detail

Result<void> Report::TryAddFailure(StringView text) noexcept {
    String failure(*allocator_);
    if (auto assigned = failure.TryAssign(text); !assigned) {
        return assigned;
    }

    return failures_.TryPushBack(Move(failure));
}

Result<bool> TryParseExpectation(StringView text, Expectation& out, String& problem) noexcept {
    out.diagnostics_.Clear();
    out.clean_ = false;
    out.stage_ = CompilationStage::All;
    bool sawStage = false;
    bool sawDirective = false;
    uint32 lineNumber = 0;
    StringBuilder<detail::MessageInlineBytes> message(problem.GetAllocator());
    auto fail = [&](StringView reason) noexcept -> Result<bool> {
        if (auto formatted = message.TryAppendFormat("line {}: {}", lineNumber, reason);
            !formatted) {
            return Result<bool>::Failure(formatted.Error());
        }

        if (auto assigned = problem.TryAssign(message.View()); !assigned) {
            return Result<bool>::Failure(assigned.Error());
        }

        return Result<bool>::Success(false);
    };

    if (!text.IsValidUtf8()) {
        return fail("the file is not valid UTF-8");
    }

    // 편집기가 붙이는 처음의 BOM은 무시한다.
    constexpr StringView Bom = "\xEF\xBB\xBF";
    usize begin = text.StartsWith(Bom, StringComparison::CaseSensitive) ? Bom.ByteCount() : 0;
    while (begin < text.ByteCount()) {
        usize end = begin;
        while (end < text.ByteCount() && text.Data()[end] != '\n') {
            ++end;
        }

        ++lineNumber;
        const StringView line = StringView(text.Data() + begin, end - begin).TrimAscii();
        begin = end + 1;
        if (line.IsEmpty() || line.Data()[0] == '#') {
            continue;
        }

        constexpr usize MaxWords = 4;
        StringView words[MaxWords];
        const usize count = SplitWords(line, words, MaxWords);
        if (words[0] == StringView("clean") && count == 1) {
            if (out.clean_) {
                return fail("'clean' appears twice");
            }

            out.clean_ = true;
        } else if (words[0] == StringView("stage") && count == 2) {
            if (sawStage) {
                return fail("'stage' appears twice");
            }

            sawStage = true;
            if (words[1] == StringView("syntax")) {
                out.stage_ = CompilationStage::Syntax;
            } else if (words[1] == StringView("imports")) {
                out.stage_ = CompilationStage::Imports;
            } else if (words[1] == StringView("names")) {
                out.stage_ = CompilationStage::Names;
            } else {
                return fail("unknown stage (expected 'syntax', 'imports' or 'names')");
            }

            continue; // 단계만으로는 기대 진단 지시가 아니다
        } else if (words[0] == StringView("error") && count >= 2 && count <= MaxWords) {
            ExpectedDiagnostic expected;
            if (!ParseCode(words[1], expected.code)) {
                return fail("diagnostic code must look like S0412");
            }

            if (FindDiagnosticCode(expected.code) == nullptr) {
                return fail("unknown diagnostic code");
            }

            // 세 번째 단어는 위치(14:7) 또는 경로, 네 번째 단어는 위치다. ':'가 있거나 숫자로만 된
            // 단어는 위치로 읽는다(틀린 위치가 경로로 조용히 바뀌지 않게).
            if (count == 3 && IsPositionLike(words[2])) {
                if (!ParsePosition(words[2], expected.line, expected.column)) {
                    return fail("position must look like 14:7 (line and column from 1)");
                }
            } else if (count == 3) {
                expected.path = words[2];
            }

            if (count == MaxWords) {
                if (IsPositionLike(words[2])) {
                    return fail("the file path comes before the position");
                }

                expected.path = words[2];
                if (!ParsePosition(words[3], expected.line, expected.column)) {
                    return fail("position must look like 14:7 (line and column from 1)");
                }
            }

            if (auto pushed = out.diagnostics_.TryPushBack(expected); !pushed) {
                return Result<bool>::Failure(pushed.Error());
            }
        } else {
            return fail("unknown directive (expected 'error', 'clean' or 'stage')");
        }

        sawDirective = true;
    }

    if (!sawDirective) {
        lineNumber = 0;
        return fail("the file has no directive; write 'clean' when no diagnostic is expected");
    }

    if (out.clean_ && !out.diagnostics_.IsEmpty()) {
        lineNumber = 0;
        return fail("'clean' cannot be combined with 'error'");
    }

    return Result<bool>::Success(true);
}

} // namespace rocks::sparks::conformance
