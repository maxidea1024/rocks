#include <SparksTestHost/Conformance.h>

#include <Core/Algorithms.h>
#include <Core/Move.h>
#include <Core/Text/NumberText.h>
#include <Core/Text/Path.h>
#include <Core/Text/StringBuilder.h>
#include <Platform/Fatal.h>
#include <SparksCompiler/Diagnostic.h>
#include <SparksCompiler/Lexer.h>
#include <SparksCompiler/Source.h>

namespace rocks::sparks::conformance {

namespace {

constexpr StringView SourceExtension = ".spark";
constexpr StringView ExpectExtension = ".expect";

// 실패 문장을 담을 builder의 inline 크기다(넘치면 allocator에서 늘린다).
constexpr usize MessageInlineBytes = 256;

// corpus 파일 하나의 상한이다. 원문 상한보다 크면 S0103 사례가 되므로 그보다 한 byte 많게 둔다.
constexpr usize MaxCorpusFileBytes = MaxSourceBytes + 1;

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

bool HasExtension(StringView name, StringView extension) noexcept {
    return name.ByteCount() > extension.ByteCount() &&
           name.EndsWith(extension, StringComparison::CaseSensitive);
}

Result<void> FormatExpected(StringBuilderBase& out, const ExpectedDiagnostic& expected) noexcept {
    if (auto appended = out.TryAppend("    error "); !appended) {
        return appended;
    }

    if (auto appended = AppendDiagnosticCode(out, expected.code); !appended) {
        return appended;
    }

    if (expected.line == 0) {
        return out.TryAppendAscii('\n');
    }

    return out.TryAppendFormat(" {}:{}\n", expected.line, expected.column);
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

bool ItemLess(const DirectoryItem& left, const DirectoryItem& right) noexcept {
    return StringView::CompareBytes(left.name.View(), right.name.View()) < 0;
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

    algo::Sort(items.View(), ItemLess);
    return Result<bool>::Success(true);
}

// 파일 전체를 읽는다. Result 실패는 할당 실패, 값 false는 읽지 못한 것(problem에 이유).
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

    StringView reason;
    uint64 size = 0;
    if (!file.Size(size)) {
        reason = "cannot read the file size";
    } else if (size > MaxCorpusFileBytes) {
        reason = "the file is larger than a corpus file may be";
    } else if (auto resized = out.TryResize(static_cast<usize>(size)); !resized) {
        static_cast<void>(file.Close());
        return Result<bool>::Failure(resized.Error());
    } else {
        const platform::FileTransfer read = file.ReadExact(out.Data(), out.Size());
        if (!read.status || read.bytes != out.Size()) {
            reason = "cannot read the file";
        }
    }

    if (!file.Close() && reason.IsEmpty()) {
        reason = "cannot close the file";
    }

    if (reason.IsEmpty()) {
        return Result<bool>::Success(true);
    }

    if (auto appended = problem.TryAppend(reason); !appended) {
        return Result<bool>::Failure(appended.Error());
    }

    return Result<bool>::Success(false);
}

// 이름에서 확장자를 뗀 사례 이름(stem)과 그 항목 번호다.
struct CaseFile {
    StringView stem;
    usize item = 0;
};

bool CaseFileLess(const CaseFile& left, const CaseFile& right) noexcept {
    return StringView::CompareBytes(left.stem, right.stem) < 0;
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

    // 컴파일 단계: 원문 검사(LC0) → 어휘 분석(LC1). 원문 검사가 막으면 뒤 단계를 돌리지 않는다.
    if (sources.Get(added.Value()).IsUsable()) {
        Array<Token> tokens(allocator);
        if (auto lexed = Tokenize(sources, added.Value(), diagnostics, tokens); !lexed) {
            return lexed;
        }
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

Result<void> AddLayoutFailure(Report& report, Allocator& allocator, StringView areaName,
                              StringView name, StringView reason) noexcept {
    StringBuilder<MessageInlineBytes> message(allocator);
    if (auto formatted = message.TryAppendFormat("{}/{}: {}", areaName, name, reason); !formatted) {
        return formatted;
    }

    return report.TryAddFailure(message.View());
}

// 사례 하나(짝이 맞은 두 파일)를 읽어 돈다. 읽기 실패는 그 파일 이름과 이유로 기록한다.
Result<void> RunPairedCase(Allocator& allocator, platform::FileSystem& files, StringView areaPath,
                           StringView areaName, StringView stem, StringView expectName,
                           StringView sourceName, Report& report) noexcept {
    auto expectPath = Path::Combine(allocator, areaPath, expectName);
    if (!expectPath) {
        return Result<void>::Failure(expectPath.Error());
    }

    auto sourcePath = Path::Combine(allocator, areaPath, sourceName);
    if (!sourcePath) {
        return Result<void>::Failure(sourcePath.Error());
    }

    Array<char> expectBytes(allocator);
    Array<char> sourceBytes(allocator);
    StringBuilder<MessageInlineBytes> problem(allocator);
    auto expectRead = ReadWholeFile(files, expectPath.Value().View(), expectBytes, problem);
    if (!expectRead) {
        return Result<void>::Failure(expectRead.Error());
    }

    if (!expectRead.Value()) {
        return AddLayoutFailure(report, allocator, areaName, expectName, problem.View());
    }

    auto sourceRead = ReadWholeFile(files, sourcePath.Value().View(), sourceBytes, problem);
    if (!sourceRead) {
        return Result<void>::Failure(sourceRead.Error());
    }

    if (!sourceRead.Value()) {
        return AddLayoutFailure(report, allocator, areaName, sourceName, problem.View());
    }

    StringBuilder<MessageInlineBytes> caseName(allocator);
    if (auto formatted = caseName.TryAppendFormat("{}/{}{}", areaName, stem, SourceExtension);
        !formatted) {
        return formatted;
    }

    return RunCase(allocator, caseName.View(), StringView(sourceBytes.Data(), sourceBytes.Size()),
                   StringView(expectBytes.Data(), expectBytes.Size()), report);
}

// 영역 폴더 하나의 사례를 돈다. 짝은 stem(확장자를 뗀 이름)으로 맞춘다.
Result<void> RunArea(Allocator& allocator, platform::FileSystem& files, StringView areaPath,
                     StringView areaName, Report& report) noexcept {
    Array<DirectoryItem> items(allocator);
    platform::FileStatus status;
    auto listed = ListDirectory(allocator, files, areaPath, items, status);
    if (!listed) {
        return Result<void>::Failure(listed.Error());
    }

    if (!listed.Value()) {
        StringBuilder<MessageInlineBytes> message(allocator);
        if (auto formatted = message.TryAppendFormat("{}: cannot list the folder", areaName);
            !formatted) {
            return formatted;
        }

        return report.TryAddFailure(message.View());
    }

    Array<CaseFile> sources(allocator);
    Array<CaseFile> expects(allocator);
    for (usize index = 0; index < items.Size(); ++index) {
        const StringView name = items[index].name.View();
        if (items[index].directory) {
            if (auto added = AddLayoutFailure(report, allocator, areaName, name,
                                              "unexpected folder in a corpus area");
                !added) {
                return added;
            }

            continue;
        }

        const bool isSource = HasExtension(name, SourceExtension);
        const bool isExpect = HasExtension(name, ExpectExtension);
        if (!isSource && !isExpect) {
            if (auto added = AddLayoutFailure(report, allocator, areaName, name,
                                              "unexpected file (only .spark and .expect)");
                !added) {
                return added;
            }

            continue;
        }

        const StringView extension = isSource ? SourceExtension : ExpectExtension;
        const CaseFile file{StringView(name.Data(), name.ByteCount() - extension.ByteCount()),
                            index};
        if (auto pushed = (isSource ? sources : expects).TryPushBack(file); !pushed) {
            return pushed;
        }
    }

    // 두 목록을 stem 순으로 정렬해 함께 훑는다(같은 stem이 짝이다).
    algo::Sort(sources.View(), CaseFileLess);
    algo::Sort(expects.View(), CaseFileLess);
    usize source = 0;
    usize expect = 0;
    while (source < sources.Size() || expect < expects.Size()) {
        int order = 0;
        if (source == sources.Size()) {
            order = 1;
        } else if (expect == expects.Size()) {
            order = -1;
        } else {
            order = StringView::CompareBytes(sources[source].stem, expects[expect].stem);
        }

        if (order < 0) {
            if (auto added = AddLayoutFailure(report, allocator, areaName,
                                              items[sources[source].item].name.View(),
                                              "missing the .expect file");
                !added) {
                return added;
            }

            ++source;
            continue;
        }

        if (order > 0) {
            if (auto added = AddLayoutFailure(report, allocator, areaName,
                                              items[expects[expect].item].name.View(),
                                              "missing the .spark file");
                !added) {
                return added;
            }

            ++expect;
            continue;
        }

        if (auto ran = RunPairedCase(allocator, files, areaPath, areaName, sources[source].stem,
                                     items[expects[expect].item].name.View(),
                                     items[sources[source].item].name.View(), report);
            !ran) {
            return ran;
        }

        ++source;
        ++expect;
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
