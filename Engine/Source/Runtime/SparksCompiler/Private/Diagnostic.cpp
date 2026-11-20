#include <SparksCompiler/Diagnostic.h>

#include <Core/Algorithms.h>
#include <Core/Move.h>
#include <Platform/Fatal.h>
#include <Platform/Memory.h>

#include "TextColumns.h"

namespace rocks::sparks {

namespace {

// 번호 오름차순이다. 새 코드는 번호 자리에 끼워 넣는다(FindDiagnosticCode가 이분 탐색한다).
constexpr DiagnosticCodeInfo CodeTable[] = {
    {DiagnosticCode::InvalidUtf8, "invalid-utf8"},
    {DiagnosticCode::LoneCarriageReturn, "lone-carriage-return"},
    {DiagnosticCode::SourceTooLarge, "source-too-large"},
};

constexpr bool CodeTableIsSorted() noexcept {
    for (usize index = 1; index < sizeof(CodeTable) / sizeof(CodeTable[0]); ++index) {
        if (static_cast<uint16>(CodeTable[index - 1].code) >=
            static_cast<uint16>(CodeTable[index].code)) {
            return false;
        }
    }

    return true;
}

static_assert(CodeTableIsSorted(), "Sparks diagnostic code table must be sorted and unique");

// 8.1절 출력 형식의 들여쓰기다.
constexpr usize SourceLineIndent = 4;
constexpr usize NoteIndent = 2;
constexpr StringView FixPrefix = "fix: ";

// 표시에서 잘못된 UTF-8 byte 대신 쓰는 U+FFFD다.
constexpr uint32 ReplacementCharacter = 0xFFFDu;

// C0 제어 문자 끝(U+001F)과 DEL(U+007F)이다. 원문 줄을 보일 때 공백으로 바꾼다(tab 포함 — 열 계산과
// 맞추고, CR 같은 문자가 터미널 출력을 흐트러뜨리지 않게).
constexpr uint8 LastControlCharacter = 0x1Fu;
constexpr uint8 DeleteCharacter = 0x7Fu;

// 두 경로를 byte 순으로 비교한다(음수: left가 앞).
int ComparePaths(StringView left, StringView right) noexcept {
    const usize shared =
        left.ByteCount() < right.ByteCount() ? left.ByteCount() : right.ByteCount();
    const int prefix = platform::MemoryCompare(left.Data(), right.Data(), shared);
    if (prefix != 0) {
        return prefix;
    }

    if (left.ByteCount() == right.ByteCount()) {
        return 0;
    }

    return left.ByteCount() < right.ByteCount() ? -1 : 1;
}

// 원문 줄을 보이는 형태로 붙인다: 제어 문자는 공백, 잘못된 UTF-8 byte는 U+FFFD 하나씩.
Result<void> AppendDisplayLine(StringBuilderBase& out, StringView line) noexcept {
    usize offset = 0;
    while (offset < line.ByteCount()) {
        const StringView rest(line.Data() + offset, line.ByteCount() - offset);
        const usize invalid = rest.FindInvalidUtf8();
        const usize validBytes = invalid == IndexNone ? rest.ByteCount() : invalid;
        usize runBegin = 0;
        for (usize index = 0; index < validBytes; ++index) {
            const auto value = static_cast<uint8>(rest.Data()[index]);
            if (value > LastControlCharacter && value != DeleteCharacter) {
                continue;
            }

            if (auto appended = out.TryAppend(StringView(rest.Data() + runBegin, index - runBegin));
                !appended) {
                return appended;
            }

            if (auto appended = out.TryAppendAscii(' '); !appended) {
                return appended;
            }

            runBegin = index + 1;
        }

        if (auto appended =
                out.TryAppend(StringView(rest.Data() + runBegin, validBytes - runBegin));
            !appended) {
            return appended;
        }

        offset += validBytes;
        if (invalid != IndexNone) {
            if (auto appended = out.TryAppendCodePoint(ReplacementCharacter); !appended) {
                return appended;
            }

            ++offset;
        }
    }

    return Result<void>::Success();
}

// 설명을 줄마다 들여 쓴다. 끝의 LF는 빈 줄을 만들지 않는다.
Result<void> AppendNoteLines(StringBuilderBase& out, StringView text) noexcept {
    usize begin = 0;
    while (begin < text.ByteCount()) {
        usize end = begin;
        while (end < text.ByteCount() && text.Data()[end] != '\n') {
            ++end;
        }

        if (auto appended = out.TryAppendRepeated(' ', NoteIndent); !appended) {
            return appended;
        }

        if (auto appended = out.TryAppend(StringView(text.Data() + begin, end - begin));
            !appended) {
            return appended;
        }

        if (auto appended = out.TryAppendAscii('\n'); !appended) {
            return appended;
        }

        begin = end + 1;
    }

    return Result<void>::Success();
}

uint32 CodeNumber(DiagnosticCode code) noexcept {
    return static_cast<uint16>(code);
}

} // namespace

Span<const DiagnosticCodeInfo> AllDiagnosticCodes() noexcept {
    return Span<const DiagnosticCodeInfo>(CodeTable, sizeof(CodeTable) / sizeof(CodeTable[0]));
}

const DiagnosticCodeInfo* FindDiagnosticCode(uint32 number) noexcept {
    usize low = 0;
    usize high = sizeof(CodeTable) / sizeof(CodeTable[0]);
    while (low < high) {
        const usize middle = low + (high - low) / 2;
        const uint32 value = CodeNumber(CodeTable[middle].code);
        if (value == number) {
            return &CodeTable[middle];
        }

        if (value < number) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }

    return nullptr;
}

Diagnostic::Diagnostic(Allocator& allocator) noexcept
    : message_(allocator), explanation_(allocator), fix_(allocator) {
}

DiagnosticList::DiagnosticList(Allocator& allocator) noexcept
    : allocator_(&allocator), items_(allocator) {
}

Result<Diagnostic> DiagnosticList::TryPrepare(DiagnosticCode code, SourceId source, bool hasSpan,
                                              SourceSpan span, StringView message,
                                              StringView explanation, StringView fix) noexcept {
    if (message.IsEmpty() || (hasSpan && span.begin > span.end)) {
        return Result<Diagnostic>::Failure(ErrorCode::InvalidArgument);
    }

    Diagnostic diagnostic(*allocator_);
    if (auto assigned = diagnostic.message_.TryAssign(message); !assigned) {
        return Result<Diagnostic>::Failure(assigned.Error());
    }

    if (auto assigned = diagnostic.explanation_.TryAssign(explanation); !assigned) {
        return Result<Diagnostic>::Failure(assigned.Error());
    }

    if (auto assigned = diagnostic.fix_.TryAssign(fix); !assigned) {
        return Result<Diagnostic>::Failure(assigned.Error());
    }

    diagnostic.code_ = code;
    diagnostic.source_ = source;
    diagnostic.hasSpan_ = hasSpan;
    diagnostic.span_ = hasSpan ? span : SourceSpan{};
    return Result<Diagnostic>::Success(Move(diagnostic));
}

Result<void> DiagnosticList::TryReserveMore(usize count) noexcept {
    return items_.TryEnsureCapacity(items_.Size() + count);
}

void DiagnosticList::Commit(Diagnostic&& diagnostic) noexcept {
    diagnostic.sequence_ = nextSequence_++;
    if (!items_.TryPushBack(Move(diagnostic))) {
        platform::Fatal("Sparks reserved diagnostic push failed");
    }
}

Result<void> DiagnosticList::TryAdd(DiagnosticCode code, SourceId source, SourceSpan span,
                                    StringView message, StringView explanation,
                                    StringView fix) noexcept {
    auto diagnostic = TryPrepare(code, source, true, span, message, explanation, fix);
    if (!diagnostic) {
        return Result<void>::Failure(diagnostic.Error());
    }

    if (auto reserved = TryReserveMore(1); !reserved) {
        return reserved;
    }

    Commit(Move(diagnostic.Value()));
    return Result<void>::Success();
}

Result<void> DiagnosticList::TryAddFileLevel(DiagnosticCode code, SourceId source,
                                             StringView message, StringView explanation,
                                             StringView fix) noexcept {
    auto diagnostic = TryPrepare(code, source, false, SourceSpan{}, message, explanation, fix);
    if (!diagnostic) {
        return Result<void>::Failure(diagnostic.Error());
    }

    if (auto reserved = TryReserveMore(1); !reserved) {
        return reserved;
    }

    Commit(Move(diagnostic.Value()));
    return Result<void>::Success();
}

void DiagnosticList::Sort(const SourceSet& sources) noexcept {
    algo::Sort(items_.View(), [&sources](const Diagnostic& left, const Diagnostic& right) noexcept {
        if (left.source_.index != right.source_.index) {
            const int paths =
                ComparePaths(sources.Get(left.source_).Path(), sources.Get(right.source_).Path());
            if (paths != 0) {
                return paths < 0;
            }

            // 같은 경로의 서로 다른 파일은 더한 순서로 둔다.
            return left.source_.index < right.source_.index;
        }

        // 파일 전체 진단이 먼저다.
        if (left.hasSpan_ != right.hasSpan_) {
            return !left.hasSpan_;
        }

        if (left.span_.begin != right.span_.begin) {
            return left.span_.begin < right.span_.begin;
        }

        if (left.code_ != right.code_) {
            return CodeNumber(left.code_) < CodeNumber(right.code_);
        }

        return left.sequence_ < right.sequence_;
    });
}

Result<void> AppendDiagnostic(StringBuilderBase& out, const SourceSet& sources,
                              const Diagnostic& diagnostic) noexcept {
    const SourceFile& file = sources.Get(diagnostic.Source());
    const uint32 code = CodeNumber(diagnostic.Code());
    if (!diagnostic.HasSpan()) {
        if (auto appended = out.TryAppendFormat("{}: error S{:04}: {}\n", file.Path(), code,
                                                diagnostic.Message());
            !appended) {
            return appended;
        }
    } else {
        const SourceSpan span = diagnostic.Span();
        const SourceLocation location = file.Locate(span.begin);
        if (auto appended =
                out.TryAppendFormat("{}:{}:{}: error S{:04}: {}\n", file.Path(), location.line,
                                    location.column, code, diagnostic.Message());
            !appended) {
            return appended;
        }

        // 원문 줄과 밑줄. 밑줄은 범위가 그 줄 안에 있는 code point 수다(최소 1, 줄 끝에서 자름).
        const uint32 lineBegin = file.LineBegin(location.line);
        const StringView line = file.LineText(location.line);
        const uint32 lineEnd = lineBegin + static_cast<uint32>(line.ByteCount());
        const uint32 underlineEnd = span.end < lineEnd ? span.end : lineEnd;
        uint32 width = span.begin < underlineEnd
                           ? detail::CountColumns(file.Text().Data() + span.begin,
                                                  file.Text().Data() + underlineEnd)
                           : 0;
        if (width == 0) {
            width = 1;
        }

        if (auto appended = out.TryAppendRepeated(' ', SourceLineIndent); !appended) {
            return appended;
        }

        if (auto appended = AppendDisplayLine(out, line); !appended) {
            return appended;
        }

        if (auto appended = out.TryAppendAscii('\n'); !appended) {
            return appended;
        }

        if (auto appended = out.TryAppendRepeated(' ', SourceLineIndent + location.column - 1);
            !appended) {
            return appended;
        }

        if (auto appended = out.TryAppendRepeated('^', width); !appended) {
            return appended;
        }

        if (auto appended = out.TryAppendAscii('\n'); !appended) {
            return appended;
        }
    }

    if (!diagnostic.Explanation().IsEmpty()) {
        if (auto appended = AppendNoteLines(out, diagnostic.Explanation()); !appended) {
            return appended;
        }
    }

    if (!diagnostic.Fix().IsEmpty()) {
        if (auto appended = out.TryAppendRepeated(' ', NoteIndent); !appended) {
            return appended;
        }

        if (auto appended = out.TryAppend(FixPrefix); !appended) {
            return appended;
        }

        if (auto appended = out.TryAppend(diagnostic.Fix()); !appended) {
            return appended;
        }

        if (auto appended = out.TryAppendAscii('\n'); !appended) {
            return appended;
        }
    }

    return Result<void>::Success();
}

Result<void> AppendDiagnostics(StringBuilderBase& out, const SourceSet& sources,
                               const DiagnosticList& diagnostics) noexcept {
    for (const Diagnostic& diagnostic : diagnostics.View()) {
        if (auto appended = AppendDiagnostic(out, sources, diagnostic); !appended) {
            return appended;
        }
    }

    return Result<void>::Success();
}

} // namespace rocks::sparks
