#include <SparksTestHost/Conformance.h>

#include <Core/Algorithms.h>
#include <Core/Move.h>
#include <Core/Text/NumberText.h>
#include <Core/Text/Path.h>
#include <Core/Text/StringBuilder.h>
#include <Platform/Fatal.h>
#include <Platform/Memory.h>
#include <SparksCompiler/Diagnostic.h>
#include <SparksCompiler/Source.h>

namespace rocks::sparks::conformance {

namespace {

constexpr StringView SourceExtension = ".spark";
constexpr StringView ExpectExtension = ".expect";

// 진단 코드 표기 "S0412"의 숫자 자리 수다.
constexpr usize CodeDigits = 4;

// 실패 문장을 담을 builder의 inline 크기다(넘치면 allocator에서 늘린다).
constexpr usize MessageInlineBytes = 256;

// corpus 파일 하나의 상한이다. 원문 상한보다 크면 S0103 사례가 되므로 그보다 한 byte 많게 둔다.
constexpr usize MaxCorpusFileBytes = MaxSourceBytes + 1;

// '\r'·공백을 양 끝에서 뗀다.
StringView TrimLine(StringView line) noexcept {
    if (!line.IsEmpty() && line.Data()[line.ByteCount() - 1] == '\r') {
        line = StringView(line.Data(), line.ByteCount() - 1);
    }

    return line.TrimAscii();
}

// 공백으로 나눈 단어를 최대 capacity개 꺼낸다. 단어가 더 있으면 capacity + 1을 돌려준다.
usize SplitWords(StringView line, StringView* words, usize capacity) noexcept {
    usize count = 0;
    usize index = 0;
    while (index < line.ByteCount()) {
        while (index < line.ByteCount() && line.Data()[index] == ' ') {
            ++index;
        }

        if (index == line.ByteCount()) {
            break;
        }

        const usize begin = index;
        while (index < line.ByteCount() && line.Data()[index] != ' ') {
            ++index;
        }

        if (count == capacity) {
            return capacity + 1;
        }

        words[count++] = StringView(line.Data() + begin, index - begin);
    }

    return count;
}

// "S0412" → 412. 형식이 틀리면 false.
bool ParseCode(StringView text, uint32& code) noexcept {
    if (text.ByteCount() != CodeDigits + 1 || text.Data()[0] != 'S') {
        return false;
    }

    auto parsed = TryParseUInt32(StringView(text.Data() + 1, CodeDigits));
    if (!parsed) {
        return false;
    }

    code = parsed.Value();
    return true;
}

// "14:14" → 줄·열(둘 다 1 이상). 형식이 틀리면 false.
bool ParsePosition(StringView text, uint32& line, uint32& column) noexcept {
    StringView left;
    StringView right;
    if (!text.Split(":", left, right)) {
        return false;
    }

    auto parsedLine = TryParseUInt32(left);
    auto parsedColumn = TryParseUInt32(right);
    if (!parsedLine || !parsedColumn || parsedLine.Value() == 0 || parsedColumn.Value() == 0) {
        return false;
    }

    line = parsedLine.Value();
    column = parsedColumn.Value();
    return true;
}

bool EndsWith(StringView text, StringView suffix) noexcept {
    return text.ByteCount() >= suffix.ByteCount() &&
           platform::MemoryCompare(text.Data() + text.ByteCount() - suffix.ByteCount(),
                                   suffix.Data(), suffix.ByteCount()) == 0;
}

bool NameLess(StringView left, StringView right) noexcept {
    const usize shared =
        left.ByteCount() < right.ByteCount() ? left.ByteCount() : right.ByteCount();
    const int prefix = platform::MemoryCompare(left.Data(), right.Data(), shared);
    if (prefix != 0) {
        return prefix < 0;
    }

    return left.ByteCount() < right.ByteCount();
}

Result<void> FormatExpected(StringBuilderBase& out, const ExpectedDiagnostic& expected) noexcept {
    if (expected.line == 0) {
        return out.TryAppendFormat("    error S{:04}\n", expected.code);
    }

    return out.TryAppendFormat("    error S{:04} {}:{}\n", expected.code, expected.line,
                               expected.column);
}

// 디렉터리 항목 하나다(열거 결과를 이름 순으로 정렬해 쓴다).
struct DirectoryItem {
    String name;
    bool directory = false;
};

struct ListContext {
    Allocator* allocator = nullptr;
    Array<DirectoryItem>* items = nullptr;
    ErrorCode error = ErrorCode::None;
};

bool CollectItem(const platform::DirectoryEntry& entry, void* context) noexcept {
    auto& list = *static_cast<ListContext*>(context);
    DirectoryItem item{String(*list.allocator), entry.info.type == platform::FileType::Directory};
    if (auto assigned = item.name.TryAssign(StringView(entry.name, entry.nameBytes)); !assigned) {
        list.error = assigned.Error();
        return false;
    }

    if (auto pushed = list.items->TryPushBack(Move(item)); !pushed) {
        list.error = pushed.Error();
        return false;
    }

    return true;
}

// 폴더 항목을 이름 순으로 모은다. Result 실패는 할당 실패, 값 false는 파일 오류(status에 원인).
Result<bool> ListDirectory(Allocator& allocator, platform::FileSystem& files, StringView path,
                           Array<DirectoryItem>& items, platform::FileStatus& status) noexcept {
    ListContext context{&allocator, &items, ErrorCode::None};
    status = files.EnumerateDirectory(path.Data(), path.ByteCount(), CollectItem, &context);
    if (context.error != ErrorCode::None) {
        return Result<bool>::Failure(context.error);
    }

    if (!status) {
        return Result<bool>::Success(false);
    }

    algo::Sort(items.View(), [](const DirectoryItem& left, const DirectoryItem& right) noexcept {
        return NameLess(left.name.View(), right.name.View());
    });
    return Result<bool>::Success(true);
}

// 파일 전체를 읽는다. Result 실패는 할당 실패, 값 false는 파일 오류나 크기 초과(problem에 이유).
Result<bool> ReadWholeFile(platform::FileSystem& files, StringView path, Array<char>& out,
                           StringBuilderBase& problem) noexcept {
    platform::File file;
    if (auto opened =
            file.Open(files, path.Data(), path.ByteCount(), platform::FileOpenMode::ReadExisting);
        !opened) {
        if (auto appended = problem.TryAppend("cannot open the file"); !appended) {
            return Result<bool>::Failure(appended.Error());
        }

        return Result<bool>::Success(false);
    }

    uint64 size = 0;
    bool ok = static_cast<bool>(file.Size(size));
    if (ok && size > MaxCorpusFileBytes) {
        ok = false;
    }

    if (ok) {
        if (auto resized = out.TryResize(static_cast<usize>(size)); !resized) {
            static_cast<void>(file.Close());
            return Result<bool>::Failure(resized.Error());
        }

        const platform::FileTransfer read = file.ReadExact(out.Data(), out.Size());
        ok = static_cast<bool>(read.status) && read.bytes == out.Size();
    }

    const bool closed = static_cast<bool>(file.Close());
    if (!ok || !closed) {
        if (auto appended = problem.TryAppend("cannot read the file"); !appended) {
            return Result<bool>::Failure(appended.Error());
        }

        return Result<bool>::Success(false);
    }

    return Result<bool>::Success(true);
}

} // namespace

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
    bool sawDirective = false;
    uint32 lineNumber = 0;
    usize begin = 0;
    StringBuilder<MessageInlineBytes> message(problem.GetAllocator());
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

    while (begin < text.ByteCount()) {
        usize end = begin;
        while (end < text.ByteCount() && text.Data()[end] != '\n') {
            ++end;
        }

        ++lineNumber;
        const StringView line = TrimLine(StringView(text.Data() + begin, end - begin));
        begin = end + 1;
        if (line.IsEmpty() || line.Data()[0] == '#') {
            continue;
        }

        constexpr usize MaxWords = 3;
        StringView words[MaxWords];
        const usize count = SplitWords(line, words, MaxWords);
        if (words[0] == StringView("clean") && count == 1) {
            if (out.clean_) {
                return fail("'clean' appears twice");
            }

            out.clean_ = true;
        } else if (words[0] == StringView("error") && (count == 2 || count == 3)) {
            ExpectedDiagnostic expected;
            if (!ParseCode(words[1], expected.code)) {
                return fail("diagnostic code must look like S0412");
            }

            if (FindDiagnosticCode(expected.code) == nullptr) {
                return fail("unknown diagnostic code");
            }

            if (count == 3 && !ParsePosition(words[2], expected.line, expected.column)) {
                return fail("position must look like 14:7 (line and column from 1)");
            }

            if (auto pushed = out.diagnostics_.TryPushBack(expected); !pushed) {
                return Result<bool>::Failure(pushed.Error());
            }
        } else {
            return fail("unknown directive (expected 'error' or 'clean')");
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

Result<void> RunCase(Allocator& allocator, StringView caseName, StringView source,
                     StringView expectText, Report& report) noexcept {
    report.CountCase();
    StringBuilder<MessageInlineBytes> message(allocator);
    Expectation expectation(allocator);
    String problem(allocator);
    auto parsed = TryParseExpectation(expectText, expectation, problem);
    if (!parsed) {
        return Result<void>::Failure(parsed.Error());
    }

    if (!parsed.Value()) {
        if (auto formatted =
                message.TryAppendFormat("{}: invalid .expect: {}", caseName, problem.View());
            !formatted) {
            return formatted;
        }

        return report.TryAddFailure(message.View());
    }

    SourceSet sources(allocator);
    DiagnosticList diagnostics(allocator);
    auto added = sources.TryAdd(caseName, source, diagnostics);
    if (!added) {
        return Result<void>::Failure(added.Error());
    }

    diagnostics.Sort(sources);
    const SourceFile& file = sources.Get(added.Value());
    const Span<const ExpectedDiagnostic> expected = expectation.Diagnostics();
    bool same = expected.Size() == diagnostics.Count();
    for (usize index = 0; same && index < expected.Size(); ++index) {
        const Diagnostic& actual = diagnostics[index];
        if (static_cast<uint16>(actual.Code()) != expected[index].code) {
            same = false;
        } else if (!actual.HasSpan()) {
            same = expected[index].line == 0;
        } else {
            const SourceLocation location = file.Locate(actual.Span().begin);
            same =
                location.line == expected[index].line && location.column == expected[index].column;
        }
    }

    if (same) {
        return Result<void>::Success();
    }

    if (auto formatted =
            message.TryAppendFormat("{}: diagnostics differ from .expect\n  expected:\n", caseName);
        !formatted) {
        return formatted;
    }

    if (expected.IsEmpty()) {
        if (auto appended = message.TryAppend("    clean\n"); !appended) {
            return appended;
        }
    }

    for (const ExpectedDiagnostic& item : expected) {
        if (auto appended = FormatExpected(message, item); !appended) {
            return appended;
        }
    }

    if (auto appended = message.TryAppend("  actual:\n"); !appended) {
        return appended;
    }

    if (diagnostics.IsEmpty()) {
        if (auto appended = message.TryAppend("    (no diagnostics)\n"); !appended) {
            return appended;
        }
    }

    if (auto appended = AppendDiagnostics(message, sources, diagnostics); !appended) {
        return appended;
    }

    return report.TryAddFailure(message.View());
}

namespace {

// 영역 폴더 하나의 사례를 돈다.
Result<void> RunArea(Allocator& allocator, platform::FileSystem& files, StringView areaPath,
                     StringView areaName, Report& report) noexcept {
    StringBuilder<MessageInlineBytes> message(allocator);
    Array<DirectoryItem> items(allocator);
    platform::FileStatus status;
    auto listed = ListDirectory(allocator, files, areaPath, items, status);
    if (!listed) {
        return Result<void>::Failure(listed.Error());
    }

    if (!listed.Value()) {
        if (auto formatted = message.TryAppendFormat("{}: cannot list the folder", areaName);
            !formatted) {
            return formatted;
        }

        return report.TryAddFailure(message.View());
    }

    // 이름 순으로 정렬되어 있으므로 같은 줄기의 ".expect"와 ".spark"는 이웃한다("X.expect" <
    // "X.spark").
    usize index = 0;
    while (index < items.Size()) {
        message.Clear();
        const StringView name = items[index].name.View();
        if (items[index].directory) {
            if (auto formatted = message.TryAppendFormat(
                    "{}/{}: unexpected folder in a corpus area", areaName, name);
                !formatted) {
                return formatted;
            }

            if (auto added = report.TryAddFailure(message.View()); !added) {
                return added;
            }

            ++index;
            continue;
        }

        const bool isSource = EndsWith(name, SourceExtension);
        const bool isExpect = EndsWith(name, ExpectExtension);
        if (!isSource && !isExpect) {
            if (auto formatted = message.TryAppendFormat(
                    "{}/{}: unexpected file (only .spark and .expect)", areaName, name);
                !formatted) {
                return formatted;
            }

            if (auto added = report.TryAddFailure(message.View()); !added) {
                return added;
            }

            ++index;
            continue;
        }

        const StringView stem(name.Data(),
                              name.ByteCount() - (isSource ? SourceExtension.ByteCount()
                                                           : ExpectExtension.ByteCount()));
        bool paired = false;
        if (isExpect && index + 1 < items.Size() && !items[index + 1].directory) {
            const StringView next = items[index + 1].name.View();
            paired = next.ByteCount() == stem.ByteCount() + SourceExtension.ByteCount() &&
                     EndsWith(next, SourceExtension) &&
                     platform::MemoryCompare(next.Data(), stem.Data(), stem.ByteCount()) == 0;
        }

        if (!paired) {
            if (auto formatted = message.TryAppendFormat("{}/{}: missing {}", areaName, name,
                                                         isSource ? StringView("the .expect file")
                                                                  : StringView("the .spark file"));
                !formatted) {
                return formatted;
            }

            if (auto added = report.TryAddFailure(message.View()); !added) {
                return added;
            }

            ++index;
            continue;
        }

        auto expectPath = Path::Combine(allocator, areaPath, name);
        auto sourcePath = Path::Combine(allocator, areaPath, items[index + 1].name.View());
        if (!expectPath || !sourcePath) {
            return Result<void>::Failure(!expectPath ? expectPath.Error() : sourcePath.Error());
        }

        Array<char> expectBytes(allocator);
        Array<char> sourceBytes(allocator);
        StringBuilder<MessageInlineBytes> problem(allocator);
        auto expectRead = ReadWholeFile(files, expectPath.Value().View(), expectBytes, problem);
        if (!expectRead) {
            return Result<void>::Failure(expectRead.Error());
        }

        auto sourceRead = expectRead.Value() ? ReadWholeFile(files, sourcePath.Value().View(),
                                                             sourceBytes, problem)
                                             : Result<bool>::Success(false);
        if (!sourceRead) {
            return Result<void>::Failure(sourceRead.Error());
        }

        if (!sourceRead.Value()) {
            if (auto formatted =
                    message.TryAppendFormat("{}/{}: {}", areaName, stem, problem.View());
                !formatted) {
                return formatted;
            }

            if (auto added = report.TryAddFailure(message.View()); !added) {
                return added;
            }
        } else {
            if (auto formatted =
                    message.TryAppendFormat("{}/{}{}", areaName, stem, SourceExtension);
                !formatted) {
                return formatted;
            }

            if (auto ran = RunCase(allocator, message.View(),
                                   StringView(sourceBytes.Data(), sourceBytes.Size()),
                                   StringView(expectBytes.Data(), expectBytes.Size()), report);
                !ran) {
                return ran;
            }
        }

        index += 2;
    }

    return Result<void>::Success();
}

} // namespace

Result<void> RunCorpus(Allocator& allocator, platform::FileSystem& files,
                       StringView corpusDirectory, Report& report) noexcept {
    Array<DirectoryItem> areas(allocator);
    platform::FileStatus status;
    auto listed = ListDirectory(allocator, files, corpusDirectory, areas, status);
    if (!listed) {
        return Result<void>::Failure(listed.Error());
    }

    if (!listed.Value()) {
        return Result<void>::Failure(status.code == platform::FileCode::NotFound
                                         ? ErrorCode::NotFound
                                         : ErrorCode::InvalidArgument);
    }

    StringBuilder<MessageInlineBytes> message(allocator);
    for (const DirectoryItem& area : areas) {
        if (!area.directory) {
            message.Clear();
            if (auto formatted = message.TryAppendFormat(
                    "{}: unexpected file at the corpus root (cases live in area folders)",
                    area.name.View());
                !formatted) {
                return formatted;
            }

            if (auto added = report.TryAddFailure(message.View()); !added) {
                return added;
            }

            continue;
        }

        auto areaPath = Path::Combine(allocator, corpusDirectory, area.name.View());
        if (!areaPath) {
            return Result<void>::Failure(areaPath.Error());
        }

        if (auto ran = RunArea(allocator, files, areaPath.Value().View(), area.name.View(), report);
            !ran) {
            return ran;
        }
    }

    return Result<void>::Success();
}

} // namespace rocks::sparks::conformance
