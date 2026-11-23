#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/Path.h>
#include <Core/Text/String.h>
#include <Core/Text/StringBuilder.h>
#include <HostConfig/HostSettings.h>
#include <Platform/Fatal.h>
#include <Platform/File.h>
#include <SparksCompiler/Formatter.h>
#include <SparksCompiler/Lexer.h>
#include <SparksCompiler/SyntaxTree.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

// 공식 formatter 시험이다(Docs/Design/SparksTools.ko.md 8.4절 — LC8c): 규칙별 golden(`Formatting/`
// 입력 → `.fmt`), conformance corpus 전체의 멱등·token 열 보존·주석 보존, 구문 오류 파일, CRLF·탭,
// 할당 실패 전수.
using namespace rocks;
using namespace rocks::sparks;
namespace sparks_formatter_tests_file {

namespace {

constexpr usize PathCapacity = 4096;

bool ReadText(Allocator& allocator, StringView path, String& out) noexcept {
    platform::File file;
    if (!file.Open(platform::PhysicalFileSystem(), path.Data(), path.ByteCount(),
                   platform::FileOpenMode::ReadExisting)) {
        return false;
    }

    uint64 size = 0;
    Array<char> bytes(allocator);
    const bool ok = file.Size(size) && bytes.TryResize(static_cast<usize>(size)) &&
                    (size == 0 || file.ReadExact(bytes.Data(), bytes.Size()).status);
    const bool closed = static_cast<bool>(file.Close());
    return ok && closed && out.TryAssign(StringView(bytes.Data(), bytes.Size()));
}

// folder 아래(하위 폴더 포함)의 `.spark` 파일 경로를 모은다.
bool CollectSparkFiles(Allocator& allocator, StringView folder, Array<String>& out) noexcept {
    struct Context {
        Allocator* allocator;
        StringView folder;
        Array<String>* out;
        bool ok;
    } context{&allocator, folder, &out, true};
    auto visit = [](const platform::DirectoryEntry& entry, void* raw) noexcept {
        auto& state = *static_cast<Context*>(raw);
        const StringView name(entry.name, entry.nameBytes);
        auto child = Path::Combine(*state.allocator, state.folder, name);
        if (!child) {
            state.ok = false;
            return false;
        }

        if (entry.info.type == platform::FileType::Directory) {
            state.ok =
                state.ok && CollectSparkFiles(*state.allocator, child.Value().View(), *state.out);
        } else if (name.EndsWith(".spark", StringComparison::CaseSensitive)) {
            state.ok = state.ok && state.out->TryPushBack(Move(child.Value()));
        }

        return state.ok;
    };
    return platform::PhysicalFileSystem().EnumerateDirectory(folder.Data(), folder.ByteCount(),
                                                             visit, &context) &&
           context.ok;
}

String EnginePath(Allocator& allocator, StringView relative) noexcept {
    char engine[PathCapacity];
    auto found = FindEngineDirectory(Span<char>(engine, PathCapacity));
    if (!(found)) {
        CHECK(false);
        return String(allocator);
    }
    auto path = Path::Combine(allocator, StringView(engine, found.Value()), relative);
    if (!(path)) {
        CHECK(false);
        return String(allocator);
    }
    return Move(path.Value());
}

// text를 서식한다. 값 false는 서식하지 않음(진단).
bool Format(Allocator& allocator, StringView text, String& out) noexcept {
    SourceSet sources(allocator);
    DiagnosticList diagnostics(allocator);
    auto id = sources.TryAdd("Case.spark", text, diagnostics);
    if (!(id)) {
        CHECK(false);
        return false;
    }
    if (!sources.Get(id.Value()).IsUsable()) {
        return false;
    }

    StringBuilder<4096> formatted(allocator);
    auto done = FormatSource(allocator, sources, id.Value(), diagnostics, formatted);
    if (!(done)) {
        CHECK(false);
        return false;
    }
    if (!done.Value()) {
        CHECK(formatted.IsEmpty());
        CHECK(diagnostics.Count() > 0);
        return false;
    }

    CHECK(diagnostics.Count() == 0);
    if (!(out.TryAssign(formatted.View()))) {
        CHECK(false);
        return false;
    }
    return true;
}

// 토큰 글자 열과 주석 글자 열이다(`>=`는 `>`·`=` 둘로 — 서식이 타입 인자를 닫는 `>=`를 나눈다.
// CRLF는 LF로, 주석은 줄마다 끝 공백을 뗀다 — 서식이 바꾸는 것은 이것뿐이다).
bool Signature(Allocator& allocator, StringView text, Array<String>& out) noexcept {
    SourceSet sources(allocator);
    DiagnosticList diagnostics(allocator);
    auto id = sources.TryAdd("Case.spark", text, diagnostics);
    if (!(id)) {
        CHECK(false);
        return false;
    }
    Array<Token> tokens(allocator);
    Array<SourceSpan> comments(allocator);
    if (!(TokenizeWithComments(sources, id.Value(), diagnostics, tokens, comments))) {
        CHECK(false);
        return false;
    }
    const StringView source = sources.Get(id.Value()).Text();
    auto push = [&](SourceSpan span, bool trim) noexcept -> bool {
        String piece(allocator);
        usize lineBegin = span.begin;
        for (usize at = span.begin; at <= span.end; ++at) {
            if (at < span.end && source.Data()[at] != '\n') {
                continue;
            }

            usize lineEnd = at;
            if (at < span.end && lineEnd > lineBegin && source.Data()[lineEnd - 1] == '\r') {
                --lineEnd;
            }

            while (trim && lineEnd > lineBegin &&
                   (source.Data()[lineEnd - 1] == ' ' || source.Data()[lineEnd - 1] == '\t')) {
                --lineEnd;
            }

            if (!(piece.TryAppend(StringView(source.Data() + lineBegin, lineEnd - lineBegin)))) {
                CHECK(false);
                return false;
            }
            if (at < span.end) {
                if (!(piece.TryAppend("\n"))) {
                    CHECK(false);
                    return false;
                }
            }

            lineBegin = at + 1;
        }

        if (!(out.TryPushBack(Move(piece)))) {
            CHECK(false);
            return false;
        }

        return true;
    };
    for (const Token& token : tokens) {
        if (token.kind == TokenKind::GreaterEqual) {
            if (!push(SourceSpan{token.span.begin, token.span.begin + 1}, false) ||
                !push(SourceSpan{token.span.begin + 1, token.span.end}, false)) {
                return false;
            }

            continue;
        }

        if (!push(token.span, token.kind == TokenKind::DocComment)) {
            return false;
        }
    }

    if (!out.TryPushBack(String(allocator))) { // 토큰과 주석의 구분
        return false;
    }

    for (const SourceSpan& comment : comments) {
        if (!push(comment, true)) {
            return false;
        }
    }

    return true;
}

bool SameStrings(const Array<String>& left, const Array<String>& right) noexcept {
    bool same = left.Size() == right.Size();
    for (usize at = 0; same && at < left.Size(); ++at) {
        same = left[at].View() == right[at].View();
    }

    return same;
}

void Report(StringView title, StringView text) noexcept {
    platform::WriteDiagnostic(title.Data(), title.ByteCount());
    platform::WriteDiagnostic("\n", 1);
    platform::WriteDiagnostic(text.Data(), text.ByteCount());
    platform::WriteDiagnostic("\n", 1);
}

} // namespace

TEST(SparksFormatter_Golden) {
    // 규칙별 golden: `Formatting/<이름>.spark`를 서식하면 `<이름>.fmt`와 같고, `.fmt`는 고정점이다.
    SystemAllocator allocator;
    {
        const String folder =
            EnginePath(allocator, "Source/Runtime/SparksCompiler/Tests/Formatting");
        Array<String> inputs(allocator);
        REQUIRE(CollectSparkFiles(allocator, folder.View(), inputs));
        CHECK(inputs.Size() >= 4);
        for (const String& input : inputs) {
            String source(allocator);
            REQUIRE(ReadText(allocator, input.View(), source));
            String formatted(allocator);
            if (!Format(allocator, source.View(), formatted)) {
                Report(input.View(), "(not formatted: lexical or syntax errors)");
                CHECK(false);
                continue;
            }

            String expectedPath(allocator);
            REQUIRE(expectedPath.TryAssign(
                StringView(input.Data(), input.ByteCount() - StringView(".spark").ByteCount())));
            REQUIRE(expectedPath.TryAppend(".fmt"));
            String expected(allocator);
            if (!ReadText(allocator, expectedPath.View(), expected) ||
                expected.View() != formatted.View()) {
                Report(input.View(), formatted.View());
                CHECK(false);
                continue;
            }

            String again(allocator);
            REQUIRE(Format(allocator, expected.View(), again));
            CHECK(again.View() == expected.View());
        }
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SparksFormatter_CrlfTabsAndBlankEdges) {
    // CRLF는 LF, 탭 들여쓰기는 공백 4칸 단위, 줄 끝 공백·파일 끝 빈 줄은 없다. 빈 파일은 빈 출력.
    SystemAllocator allocator;
    {
        String formatted(allocator);
        REQUIRE(Format(allocator,
                       "\r\n\r\nfn Tabs() {\r\n\tlet a = 1;\t\r\n\t\tif (a == 1) {\r\n\t\t\treturn;"
                       "\r\n\t\t}\r\n}\r\n\r\n\r\n",
                       formatted));
        CHECK(formatted.View() ==
              "fn Tabs() {\n    let a = 1;\n    if (a == 1) {\n        return;\n"
              "    }\n}\n");
        String empty(allocator);
        REQUIRE(Format(allocator, "", empty));
        CHECK(empty.IsEmpty());
        String onlyComment(allocator);
        REQUIRE(Format(allocator, "\n\n// note   \n\n", onlyComment));
        CHECK(onlyComment.View() == "// note\n");
        // UTF-8 BOM은 지운다(정규형은 BOM 없는 UTF-8 — 3절).
        String bom(allocator);
        REQUIRE(Format(allocator,
                       "\xEF\xBB\xBF"
                       "fn F() {\n}\n",
                       bom));
        CHECK(bom.View() == "fn F() {\n}\n");
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SparksFormatter_SyntaxErrorsAreNotFormatted) {
    SystemAllocator allocator;
    {
        const StringView broken[] = {"fn F( {\n}\n", "fn F() {\n    let x = ;\n}\n",
                                     "fn F() {\n    let s = \"open;\n}\n", "/* open\n",
                                     "fn F() {\n    let move_speed = 1;\n}\n"};
        for (const StringView text : broken) {
            String formatted(allocator);
            CHECK(!Format(allocator, text, formatted));
        }
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SparksFormatter_CorpusIdempotentAndTokenPreserving) {
    // conformance corpus의 모든 `.spark`(폴더 사례·revision 포함) 중 어휘·구문 진단 없는 것: 멱등,
    // token 글자 열 보존, 주석 보존, 서식 결과도 진단 없음.
    SystemAllocator allocator;
    {
        const String corpus = EnginePath(allocator, "Source/Developer/SparksTestHost/Conformance");
        Array<String> files(allocator);
        REQUIRE(CollectSparkFiles(allocator, corpus.View(), files));
        uint32 formattedCount = 0;
        uint32 failures = 0;
        for (const String& path : files) {
            String source(allocator);
            REQUIRE(ReadText(allocator, path.View(), source));
            String first(allocator);
            if (!Format(allocator, source.View(), first)) {
                continue; // 어휘·구문 진단이 있는 사례
            }

            ++formattedCount;
            String second(allocator);
            const bool reformatted = Format(allocator, first.View(), second);
            Array<String> before(allocator);
            Array<String> after(allocator);
            REQUIRE(Signature(allocator, source.View(), before));
            REQUIRE(Signature(allocator, first.View(), after));
            if (!reformatted || second.View() != first.View() || !SameStrings(before, after)) {
                ++failures;
                Report(path.View(), first.View());
                if (reformatted && second.View() != first.View()) {
                    Report("second pass:", second.View());
                }
            }
        }

        CHECK(formattedCount > 100);
        CHECK(failures == 0);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

namespace {

// 원문의 토큰 경계(문자열 안 제외)를 바꾼 결정적 변형이다. kind 0: 토큰마다 앞에 `/* c */ `,
// 1: 세 번째 토큰마다 앞에 `// c` 줄, 2: 공백만인 경계를 모두 줄바꿈으로.
bool Variant(Allocator& allocator, StringView text, uint32 kind, String& out) noexcept {
    SourceSet sources(allocator);
    DiagnosticList diagnostics(allocator);
    auto id = sources.TryAdd("Case.spark", text, diagnostics);
    Array<Token> tokens(allocator);
    if (!id || !Tokenize(sources, id.Value(), diagnostics, tokens)) {
        return false;
    }

    const StringView source = sources.Get(id.Value()).Text();
    usize cursor = 0;
    uint32 depth = 0; // 문자열 안
    for (usize index = 0; index < tokens.Size(); ++index) {
        const Token& token = tokens[index];
        const StringView gap(source.Data() + cursor, token.span.begin - cursor);
        bool whitespace = true;
        for (usize at = 0; at < gap.ByteCount(); ++at) {
            const char c = gap.Data()[at];
            whitespace = whitespace && (c == ' ' || c == '\t' || c == '\n' || c == '\r');
        }

        const bool open = depth == 0 && index > 0 && token.kind != TokenKind::EndOfFile;
        bool ok = true;
        if (open && kind == 2 && whitespace) {
            ok = static_cast<bool>(out.TryAppend("\n"));
        } else {
            ok = static_cast<bool>(out.TryAppend(gap));
            if (open && kind == 0) {
                ok = ok && out.TryAppend(" /* c */ ");
            } else if (open && kind == 1 && index % 3 == 0) {
                ok = ok && out.TryAppend(" // c\n");
            }
        }

        ok = ok && out.TryAppend(StringView(source.Data() + token.span.begin,
                                            token.span.end - token.span.begin));
        if (!ok) {
            return false;
        }

        depth += token.kind == TokenKind::StringBegin ? 1 : 0;
        depth -= token.kind == TokenKind::StringEnd && depth > 0 ? 1 : 0;
        cursor = token.span.end;
    }

    return static_cast<bool>(
        out.TryAppend(StringView(source.Data() + cursor, source.ByteCount() - cursor)));
}

} // namespace

TEST(SparksFormatter_CorpusVariantsIdempotentAndTokenPreserving) {
    // 이상한 자리의 주석·줄바꿈: corpus 원문마다 결정적 변형 셋(토큰마다 블록 주석, 세 토큰마다 줄
    // 주석, 모든 경계 줄바꿈)을 만들어 멱등·token 열·주석 열 보존을 본다.
    SystemAllocator allocator;
    {
        const String corpus = EnginePath(allocator, "Source/Developer/SparksTestHost/Conformance");
        Array<String> files(allocator);
        REQUIRE(CollectSparkFiles(allocator, corpus.View(), files));
        uint32 checked = 0;
        uint32 failures = 0;
        for (const String& path : files) {
            String source(allocator);
            REQUIRE(ReadText(allocator, path.View(), source));
            String plain(allocator);
            if (!Format(allocator, source.View(), plain)) {
                continue;
            }

            for (uint32 kind = 0; kind < 3; ++kind) {
                String variant(allocator);
                REQUIRE(Variant(allocator, source.View(), kind, variant));
                String first(allocator);
                if (!Format(allocator, variant.View(), first)) {
                    continue; // 변형이 어휘·구문 규칙에 걸림(문서 주석 자리 등)
                }

                ++checked;
                String second(allocator);
                const bool again = Format(allocator, first.View(), second);
                Array<String> before(allocator);
                Array<String> after(allocator);
                REQUIRE(Signature(allocator, variant.View(), before));
                REQUIRE(Signature(allocator, first.View(), after));
                if (!again || second.View() != first.View() || !SameStrings(before, after)) {
                    ++failures;
                    Report(path.View(), first.View());
                    if (again) {
                        Report("second pass:", second.View());
                    }
                }
            }
        }

        CHECK(checked > 200);
        CHECK(failures == 0);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SparksFormatter_AllocationFailureEverywhere) {
    SystemAllocator backing;
    constexpr StringView Text = "// c\nclass Door : ScriptableComponent {\n    @save var Open = "
                                "false; // t\n    fn F(a: Array<int>): int {\n        return a[0] "
                                "+ -1;\n    }\n}\n";
    uint64 attempts = 0;
    uint64 failures = 0;
    String baseline(backing);
    for (uint64 failAt = 0; failAt == 0 || failAt <= attempts; ++failAt) {
        test::NFailAllocator allocator(backing);
        const uint64 before = backing.Stats().liveAllocations; // 기준 출력은 살아 있다
        {
            SourceSet sources(backing);
            DiagnosticList diagnostics(backing);
            auto id = sources.TryAdd("Case.spark", Text, diagnostics);
            REQUIRE(id);
            StringBuilder<16> out(allocator);
            if (failAt != 0) {
                allocator.FailOnAttempt(failAt);
            }

            auto done = FormatSource(allocator, sources, id.Value(), diagnostics, out);
            const uint64 used = allocator.Attempts();
            allocator.DisableFailure();
            if (failAt == 0) {
                REQUIRE(done && done.Value());
                attempts = used;
                REQUIRE(baseline.TryAssign(out.View()));
            } else if (!done) {
                ++failures;
            } else {
                CHECK(done.Value());
                CHECK(out.View() == baseline.View()); // 삼킨 실패가 없다
            }
        }

        if (failAt != 0) {
            CHECK(backing.Stats().liveAllocations == before);
        }
    }

    CHECK(failures > 0);
}

} // namespace sparks_formatter_tests_file
