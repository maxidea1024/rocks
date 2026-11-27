#include <ShaderCompiler/ShaderDiagnostics.h>

#include <Core/Move.h>
#include <ShaderCompiler/ShaderSourceTree.h>
#include <stdio.h>

// 셰이더 컴파일 진단의 위치 해석(Docs/Design/ShaderSystem 16.3절 SSD72)이다. 규칙은 공개 헤더
// 주석을 따른다.
namespace rocks::shader {

namespace diagnostics_detail {

// 줄·열 숫자의 최대 자리 수다(넘으면 위치가 아니다).
constexpr usize MaxLocationDigits = 9;
// 한 줄 꼴의 위치 머리 버퍼 byte 수("(4294967295,4294967295): " 넉넉히)다.
constexpr usize LocationTextBytes = 48;

struct Location {
    usize pathStart = 0;
    usize pathEnd = 0;
    uint32 line = 0;
    uint32 column = 0;
    // 위치 뒤 문구가 시작하는 자리다.
    usize rest = 0;
};

bool IsDigit(char c) noexcept {
    return c >= '0' && c <= '9';
}

bool IsPathByte(char c) noexcept {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || IsDigit(c) || c == '_' || c == '.' ||
           c == '-' || c == '/';
}

// text[at]부터 숫자를 읽는다(1–MaxLocationDigits 자리). 읽은 뒤 자리를 at에 둔다.
bool ReadNumber(StringView text, usize& at, uint32& value) noexcept {
    const usize start = at;
    uint64 parsed = 0;
    while (at < text.ByteCount() && IsDigit(text.Data()[at]) && at - start < MaxLocationDigits) {
        parsed = parsed * 10 + static_cast<uint64>(text.Data()[at] - '0');
        ++at;
    }

    if (at == start || (at < text.ByteCount() && IsDigit(text.Data()[at]))) {
        return false;
    }

    value = static_cast<uint32>(parsed);
    return true;
}

// at 자리 뒤의 공백을 건너뛴다.
usize SkipSpaces(StringView text, usize at) noexcept {
    while (at < text.ByteCount() && (text.Data()[at] == ' ' || text.Data()[at] == '\t')) {
        ++at;
    }

    return at;
}

// 경로 끝(end) 뒤의 줄·열 꼴을 읽는다.
bool ReadPosition(StringView text, usize end, Location& out) noexcept {
    const usize bytes = text.ByteCount();
    const char* data = text.Data();
    usize at = end;
    if (at >= bytes) {
        return false;
    }

    if (data[at] == ':') {
        // clang 꼴: :줄[:열]: 뒤 문구.
        ++at;
        if (!ReadNumber(text, at, out.line)) {
            return false;
        }

        if (at + 1 < bytes && data[at] == ':' && IsDigit(data[at + 1])) {
            ++at;
            if (!ReadNumber(text, at, out.column)) {
                return false;
            }
        }

        if (at >= bytes || data[at] != ':') {
            return false;
        }

        out.rest = at + 1;
        return true;
    }

    if (data[at] != '(') {
        return false;
    }

    // MSVC·FXC 꼴: (줄[,열[-열]])[ ]: 뒤 문구.
    ++at;
    if (!ReadNumber(text, at, out.line)) {
        return false;
    }

    if (at < bytes && data[at] == ',') {
        ++at;
        if (!ReadNumber(text, at, out.column)) {
            return false;
        }

        if (at < bytes && data[at] == '-') {
            ++at;
            uint32 endColumn = 0;
            if (!ReadNumber(text, at, endColumn)) {
                return false;
            }
        }
    }

    if (at >= bytes || data[at] != ')') {
        return false;
    }

    at = SkipSpaces(text, at + 1);
    if (at >= bytes || data[at] != ':') {
        return false;
    }

    out.rest = at + 1;
    return true;
}

// 줄에서 첫 위치를 찾는다: 줄 처음이나 공백·따옴표 뒤의 '/'로 시작해 가상 경로 규칙을 통과하고
// 줄·열 꼴이 뒤따르는 자리다.
bool FindLocation(StringView line, Location& out) noexcept {
    const char* data = line.Data();
    const usize bytes = line.ByteCount();
    for (usize start = 0; start < bytes; ++start) {
        if (data[start] != '/') {
            continue;
        }

        if (start != 0 && data[start - 1] != ' ' && data[start - 1] != '\t' &&
            data[start - 1] != '"' && data[start - 1] != '\'') {
            continue;
        }

        usize end = start;
        while (end < bytes && IsPathByte(data[end])) {
            ++end;
        }

        const StringView path(data + start, end - start);
        if (!ValidateShaderVirtualPath(path)) {
            continue;
        }

        Location location;
        location.pathStart = start;
        location.pathEnd = end;
        if (ReadPosition(line, end, location)) {
            out = location;
            return true;
        }
    }

    return false;
}

// rest의 심각도 낱말을 떼고 심각도를 정한다. 문구 시작 자리를 돌려준다.
usize ReadSeverity(StringView line, usize at, ShaderDiagnosticSeverity& severity) noexcept {
    struct Word {
        StringView text;
        ShaderDiagnosticSeverity severity;
    };

    const Word words[] = {{"fatal error", ShaderDiagnosticSeverity::Error},
                          {"error", ShaderDiagnosticSeverity::Error},
                          {"warning", ShaderDiagnosticSeverity::Warning},
                          {"note", ShaderDiagnosticSeverity::Note}};
    at = SkipSpaces(line, at);
    const StringView rest(line.Data() + at, line.ByteCount() - at);
    severity = ShaderDiagnosticSeverity::Error;
    for (const Word& word : words) {
        if (!rest.StartsWith(word.text, StringComparison::CaseSensitive)) {
            continue;
        }

        const usize after = word.text.ByteCount();
        if (after < rest.ByteCount() && rest.Data()[after] == ':') {
            severity = word.severity;
            return SkipSpaces(line, at + after + 1);
        }

        // FXC 꼴: "error X3004: …"는 코드를 문구에 남긴다.
        if (after < rest.ByteCount() && rest.Data()[after] == ' ') {
            severity = word.severity;
            return SkipSpaces(line, at + after + 1);
        }
    }

    return at;
}

StringView TrimEnd(StringView text) noexcept {
    usize bytes = text.ByteCount();
    while (bytes > 0 && (text.Data()[bytes - 1] == '\r' || text.Data()[bytes - 1] == ' ' ||
                         text.Data()[bytes - 1] == '\t')) {
        --bytes;
    }

    return StringView(text.Data(), bytes);
}

bool IsBlank(StringView text) noexcept {
    for (const char c : text) {
        if (c != ' ' && c != '\t' && c != '\r') {
            return false;
        }
    }

    return true;
}

uint32 ContextLines(const ShaderDiagnostic& diagnostic) noexcept {
    if (diagnostic.context.IsEmpty()) {
        return 0;
    }

    uint32 lines = 1;
    for (const char c : diagnostic.context.View()) {
        lines += c == '\n' ? 1u : 0u;
    }

    return lines;
}

bool SameDiagnostic(const ShaderDiagnostic& left, const ShaderDiagnostic& right) noexcept {
    return left.line == right.line && left.column == right.column &&
           left.severity == right.severity && left.virtualPath.View() == right.virtualPath.View() &&
           left.message.View() == right.message.View();
}

Result<void> OutOfMemory() noexcept {
    return Result<void>::Failure(ErrorCode::OutOfMemory);
}

} // namespace diagnostics_detail

const char* ShaderDiagnosticSeverityName(ShaderDiagnosticSeverity severity) noexcept {
    switch (severity) {
    case ShaderDiagnosticSeverity::Error:
        return "error";
    case ShaderDiagnosticSeverity::Warning:
        return "warning";
    case ShaderDiagnosticSeverity::Note:
        return "note";
    }

    return "error";
}

Result<void> ParseShaderDiagnostics(StringView text, Array<ShaderDiagnostic>& out) noexcept {
    using namespace diagnostics_detail;
    Allocator& allocator = out.GetAllocator();
    // 이 호출이 만든 첫 진단 번호다(앞 호출의 진단에 이어지는 줄을 붙이지 않는다).
    const usize first = out.Size();
    usize start = 0;
    while (start <= text.ByteCount()) {
        usize end = start;
        while (end < text.ByteCount() && text.Data()[end] != '\n') {
            ++end;
        }

        const StringView line = TrimEnd(StringView(text.Data() + start, end - start));
        start = end + 1;
        if (IsBlank(line)) {
            if (end >= text.ByteCount()) {
                break;
            }

            continue;
        }

        Location location;
        if (FindLocation(line, location)) {
            ShaderDiagnosticSeverity severity = ShaderDiagnosticSeverity::Error;
            const usize message = ReadSeverity(line, location.rest, severity);
            const StringView body(line.Data() + message, line.ByteCount() - message);
            // 문구 없는 위치 줄("In file included from …:3:")은 이어지는 줄로 다룬다.
            if (!IsBlank(body)) {
                auto added = out.TryEmplaceBack(allocator);
                if (!added) {
                    return OutOfMemory();
                }

                ShaderDiagnostic& diagnostic = *added.Value();
                diagnostic.line = location.line;
                diagnostic.column = location.column;
                diagnostic.severity = severity;
                if (!diagnostic.virtualPath.TryAssign(StringView(
                        line.Data() + location.pathStart, location.pathEnd - location.pathStart)) ||
                    !diagnostic.message.TryAssign(body)) {
                    return OutOfMemory();
                }

                continue;
            }
        }

        if (out.Size() > first) {
            ShaderDiagnostic& previous = out[out.Size() - 1];
            if (ContextLines(previous) < MaxShaderDiagnosticContextLines) {
                if ((!previous.context.IsEmpty() && !previous.context.TryAppend("\n")) ||
                    !previous.context.TryAppend(line)) {
                    return OutOfMemory();
                }
            }

            continue;
        }

        auto added = out.TryEmplaceBack(allocator);
        if (!added) {
            return OutOfMemory();
        }

        String& message = added.Value()->message;
        if (!message.TryAssign(line)) {
            return OutOfMemory();
        }

        message.TrimAscii();
    }

    return Result<void>::Success();
}

Result<void> MergeShaderDiagnostics(Array<ShaderDiagnostic>& into,
                                    Array<ShaderDiagnostic>& from) noexcept {
    using namespace diagnostics_detail;
    for (ShaderDiagnostic& diagnostic : from) {
        bool merged = false;
        for (ShaderDiagnostic& existing : into) {
            if (SameDiagnostic(existing, diagnostic)) {
                existing.occurrences += diagnostic.occurrences;
                merged = true;
                break;
            }
        }

        if (!merged && !into.TryPushBack(Move(diagnostic))) {
            return OutOfMemory();
        }
    }

    from.Clear();
    return Result<void>::Success();
}

Result<void> FormatShaderDiagnostic(const ShaderDiagnostic& diagnostic, StringView displayPath,
                                    String& out) noexcept {
    using namespace diagnostics_detail;
    out.Clear();
    const StringView path = displayPath.IsEmpty() ? diagnostic.virtualPath.View() : displayPath;
    char location[LocationTextBytes] = {};
    if (!diagnostic.virtualPath.IsEmpty()) {
        if (diagnostic.line == 0) {
            (void)snprintf(location, sizeof(location), ": ");
        } else if (diagnostic.column == 0) {
            (void)snprintf(location, sizeof(location), "(%u): ", diagnostic.line);
        } else {
            (void)snprintf(location, sizeof(location), "(%u,%u): ", diagnostic.line,
                           diagnostic.column);
        }

        if (!out.TryAppend(path) || !out.TryAppend(StringView(location))) {
            return OutOfMemory();
        }
    }

    if (!out.TryAppend(StringView(ShaderDiagnosticSeverityName(diagnostic.severity))) ||
        !out.TryAppend(": ") || !out.TryAppend(diagnostic.message.View())) {
        return OutOfMemory();
    }

    return Result<void>::Success();
}

} // namespace rocks::shader
