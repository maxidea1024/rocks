#include "SparksToolsLspTestSupport.h"

// 언어 서버의 분석 시험이다(Docs/Design/SparksTools.ko.md 10.4절–10.8절 — LC8e): 열린 문서 덮기와
// 진단 발행·지우기, 작업 공간 뿌리, UTF-16 위치·file URI, 의미 강조 golden, conformance corpus와의
// 진단 동등성.
using namespace rocks;
namespace sparks_tools_lsp_analysis_tests_file {

using namespace rocks::test::sparks_lsp;

TEST(SparksLsp_PublishesAndOverlaysDiagnostics) {
    SystemAllocator allocator;
    {
        LspFolder folder(allocator);
        REQUIRE(folder.Write("Game/SparksPackage.json5", LspTestCoreManifest));
        REQUIRE(
            folder.Write("Game/Main.spark", "import Lib;\n\nfn F(): int {\n    return G();\n}\n"));
        REQUIRE(folder.Write("Game/Lib.spark", "fn G(): int {\n    return 1;\n}\n"));
        REQUIRE(folder.Write("Game/Bad.spark", "import Nope;\n"));
        const String root = folder.UriOf("");
        const String bad = folder.UriOf("Game/Bad.spark");
        const String lib = folder.UriOf("Game/Lib.spark");
        const String fresh = folder.UriOf("Game/Sub/New.spark");
        const String loose = folder.UriOf("Loose.spark");

        // URI 모양: 드라이브 소문자·`%3A`, 공백 `%20`, 한글 대문자 `%XX`.
        CHECK(root.View().StartsWith("file:///"));
        CHECK(root.View().Contains("/rocks%20sparks%20%ED%95%9C%EA%B8%80-",
                                   StringComparison::CaseSensitive));
        if (LspDrive(folder.Root())) {
            CHECK(root.View().Data()[8] >= 'a' && root.View().Data()[8] <= 'z');
            CHECK(StringView(root.View().Data() + 9, 3) == "%3A");
        }

        LspScript script(allocator);
        script.Initialize(1, root.View())
            .Notify("initialized")                                     // Bad(디스크)
            .Open(bad.View(), 1, "fn B(): int {\n    return 2;\n}\n")  // Bad 지움(v1)
            .Change(bad.View(), 2, "\nimport Nope;\n")                 // Bad 1:7(v2)
            .Change(bad.View(), 3, "\nimport Nope;\n")                 // 같은 진단 — 발행 없음
            .Open(lib.View(), 1, "fn H(): int {\n    return 1;\n}\n")  // 닫힌 Main에 진단
            .Close(lib.View())                                         // Main 지움
            .Open(fresh.View(), 4, "import Nope;\n")                   // 저장 전 새 파일
            .Close(fresh.View())                                       // 새 파일 지움
            .Close(bad.View())                                         // 디스크 Bad(0:7)
            .Open(loose.View(), 1, "import Nope;\n")                   // 패키지 밖 — 없음
            .Open("untitled:Untitled-1", 1, "import Nope;\n")          // file 아님 — 없음
            .Open("file:///c%3/Broken.spark", 1, "import Nope;\n")     // 잘못된 percent — 없음
            .Open("file://server/share/X.spark", 1, "import Nope;\n"); // authority — 없음
        const LspRun first = LspRunScript(allocator, {}, script.text.View());
        CHECK(first.code == LspTestUnclean); // exit 없이 입력 끝
        const String text = LspPublishText(allocator, first.output.View(), root.View());
        LspExpectText(text.View(),
                      "Game/Bad.spark [S0301 0:7-0:11]\n"
                      "Game/Bad.spark v1 []\n"
                      "Game/Bad.spark v2 [S0301 1:7-1:11]\n"
                      "Game/Main.spark [S0317 3:11-3:12]\n"
                      "Game/Main.spark []\n"
                      "Game/Sub/New.spark v4 [S0301 0:7-0:11]\n"
                      "Game/Sub/New.spark []\n"
                      "Game/Bad.spark [S0301 0:7-0:11]\n",
                      "overlay publishes");

        // 디스크에서 지운 파일은 다음 분석에서 빈 목록으로 지운다. 대문자 드라이브·`:` 그대로인
        // URI도 같은 문서로 본다(발행은 그 URI로). 지움이 먼저다(10.5절).
        auto upperOf = [&](StringView relative) noexcept {
            String upper(allocator);
            const String path = folder.PathOf(relative);
            if (LspDrive(path.View())) {
                CHECK(upper.TryAppend("file:///"));
                const char drive = static_cast<char>(path.View().Data()[0] - 'a' + 'A');
                CHECK(upper.TryAppend(StringView(&drive, 1)));
                CHECK(upper.TryAppend(":"));
                const String encoded = LspUriOf(allocator, path.View());
                // `file:///c%3A` 뒤
                CHECK(upper.TryAppend(
                    StringView(encoded.View().Data() + 12, encoded.ByteCount() - 12)));
            }

            return upper;
        };
        const String upper = upperOf("Game/Main.spark");

        // 한 세션 안에서 처음 분석 뒤 파일을 지운다 — 채널이 두 번째 읽기 직전에 지운다(첫 읽기가
        // initialize·initialized를 전부 주므로 그 분석이 끝난 뒤다).
        class RemovingChannel final : public MessageChannel {
        public:
            RemovingChannel(LspScriptChannel& inner, const LspFolder& folder,
                            usize removeAt) noexcept
                : inner_(inner), folder_(folder), removeAt_(removeAt) {
            }

            Result<usize> Read(Span<char> buffer) noexcept override {
                if (++reads_ == removeAt_) {
                    CHECK(folder_.Remove("Game/Bad.spark"));
                }

                return inner_.Read(buffer);
            }

            Result<void> Write(StringView bytes) noexcept override {
                return inner_.Write(bytes);
            }

            void Log(StringView line) noexcept override {
                inner_.Log(line);
            }

        private:
            LspScriptChannel& inner_;
            const LspFolder& folder_;
            usize removeAt_;
            usize reads_ = 0;
        };

        // 메시지마다 한 번 읽게 조각을 나눈다: 앞 두 메시지(initialize·initialized)를 따로 보낸다.
        LspScript head(allocator);
        head.Initialize(1, root.View()).Notify("initialized");
        LspScript tail(allocator);
        if (!upper.IsEmpty()) {
            tail.Open(upper.View(), 7, "fn F(): int {\n    return Nope;\n}\n");
        } else {
            tail.Open(folder.UriOf("Game/Main.spark").View(), 7,
                      "fn F(): int {\n    return Nope;\n}\n");
        }

        String joined(allocator);
        CHECK(joined.TryAppend(head.text.View()));
        CHECK(joined.TryAppend(tail.text.View()));
        LspScriptChannel inner(allocator, joined.View(), head.text.ByteCount());
        RemovingChannel removing(inner, folder, 2);
        CHECK(RunLanguageServer(allocator, platform::PhysicalFileSystem(), {}, removing) ==
              LspTestUnclean);
        const String after = LspPublishText(allocator, inner.output.View(), root.View());
        String expected(allocator);
        CHECK(expected.TryAppend("Game/Bad.spark [S0301 0:7-0:11]\n"));
        CHECK(expected.TryAppend("Game/Bad.spark []\n"));
        if (!upper.IsEmpty()) {
            CHECK(expected.TryAppend(upper.View()));
            CHECK(expected.TryAppend(" v7 [S0317 1:11-1:15]\n"));
        } else {
            CHECK(expected.TryAppend("Game/Main.spark v7 [S0317 1:11-1:15]\n"));
        }

        LspExpectText(after.View(), expected.View(), "removed file and drive spelling");
        CHECK(!folder.Remove("Game/Bad.spark")); // 채널이 지웠다

        // 진단이 있던 파일을 다른 철자로 열면: 옛 URI 지움이 먼저, 새 URI 진단이 뒤다.
        const String upperBad = upperOf("Game/Bad.spark");
        if (!upperBad.IsEmpty()) {
            REQUIRE(folder.Write("Game/Bad.spark", "import Nope;\n"));
            LspScript respelled(allocator);
            respelled.Initialize(1, root.View())
                .Notify("initialized")
                .Open(upperBad.View(), 5, "\nimport Nope;\n");
            const LspRun run = LspRunScript(allocator, {}, respelled.text.View());
            String respelledExpected(allocator);
            CHECK(respelledExpected.TryAppend("Game/Bad.spark [S0301 0:7-0:11]\n"
                                              "Game/Bad.spark []\n"));
            CHECK(respelledExpected.TryAppend(upperBad.View()));
            CHECK(respelledExpected.TryAppend(" v5 [S0301 1:7-1:11]\n"));
            LspExpectText(LspPublishText(allocator, run.output.View(), root.View()).View(),
                          respelledExpected.View(), "respelled document");
        }
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SparksLsp_WorkspaceRoots) {
    // workspaceFolders(file 아닌 폴더는 기록 후 무시), 없는 폴더(작업 공간 읽기 실패 — 기록, 진단
    // 없음), 작업 공간 없음(열린 문서도 분석하지 않는다).
    SystemAllocator allocator;
    {
        LspFolder folder(allocator);
        REQUIRE(folder.Write("Game/SparksPackage.json5", LspTestCoreManifest));
        REQUIRE(folder.Write("Game/Bad.spark", "import Nope;\n"));
        const String game = folder.UriOf("Game");
        String params(allocator);
        CHECK(params.TryAppend("{\"capabilities\":{},\"rootUri\":\"file:///ignored\","
                               "\"workspaceFolders\":[{\"uri\":\"https://example.com/x\","
                               "\"name\":\"web\"},{\"uri\":"));
        LspQuote(params, game.View());
        CHECK(params.TryAppend(",\"name\":\"game\"},7]}"));
        LspScript folders(allocator);
        folders.Call(1, "initialize", params.View()).Notify("initialized").End(2);
        const LspRun run = LspRunScript(allocator, {}, folders.text.View());
        CHECK(run.code == LspTestClean);
        const String text = LspPublishText(allocator, run.output.View(), game.View());
        LspExpectText(text.View(), "Bad.spark [S0301 0:7-0:11]\n", "workspace folders");
        LspExpectText(run.log.View(),
                      "sparks-lsp: a workspace folder is not a file URI; it is ignored\n",
                      "folder log");

        const String missing = folder.UriOf("Missing");
        LspScript absent(allocator);
        absent.Initialize(1, missing.View())
            .Notify("initialized")
            .Open(folder.UriOf("Missing/A.spark").View(), 1, "import Nope;\n")
            .Tokens(2, folder.UriOf("Missing/A.spark").View())
            .End(3);
        const LspRun none = LspRunScript(allocator, {}, absent.text.View());
        CHECK(none.code == LspTestClean);
        CHECK(LspPublishText(allocator, none.output.View(), {}).IsEmpty());
        CHECK(none.log.View().StartsWith("sparks-lsp: "));
        CHECK(none.log.View().Contains("not a folder or a .spark file",
                                       StringComparison::CaseSensitive));
        CHECK(LspTokenLines(allocator, none.output.View(), 2).IsEmpty());

        LspScript empty(allocator);
        empty.Initialize(1, {})
            .Notify("initialized")
            .Open(folder.UriOf("Game/Bad.spark").View(), 1, "import Nope;\n")
            .End(2);
        const LspRun bare = LspRunScript(allocator, {}, empty.text.View());
        CHECK(bare.code == LspTestClean);
        CHECK(LspPublishText(allocator, bare.output.View(), {}).IsEmpty());
        CHECK(bare.log.View() == LspTestNoWorkspaceLog);

        // 같은 읽기 실패는 한 번만 기록한다(문서 알림마다 쓰지 않는다).
        LspScript repeated(allocator);
        repeated.Initialize(1, missing.View())
            .Notify("initialized")
            .Open(folder.UriOf("Missing/A.spark").View(), 1, "import Nope;\n")
            .Change(folder.UriOf("Missing/A.spark").View(), 2, "import Nope2;\n")
            .End(2);
        const LspRun once = LspRunScript(allocator, {}, repeated.text.View());
        usize lines = 0;
        for (usize at = 0; at < once.log.ByteCount(); ++at) {
            lines += once.log.View().Data()[at] == '\n' ? 1 : 0;
        }

        CHECK(lines == 1);

        // URI 철자: `localhost` authority·소문자 `%3a`는 받고, `%2F`가 든 URI·다른 authority는
        // 받지 않는다(진단 없음).
        const String bad = folder.UriOf("Game/Bad.spark");
        String localhost(allocator);
        CHECK(localhost.TryAppend("file://localhost/"));
        CHECK(localhost.TryAppend(StringView(game.View().Data() + 8, game.ByteCount() - 8)));
        String lower(allocator);
        if (LspDrive(folder.Root())) {
            CHECK(lower.TryAppend(StringView(bad.View().Data(), 9)));
            CHECK(lower.TryAppend("%3a"));
            CHECK(lower.TryAppend(StringView(bad.View().Data() + 12, bad.ByteCount() - 12)));
        } else {
            CHECK(lower.TryAssign(bad.View()));
        }

        String slash(allocator);
        CHECK(slash.TryAppend(game.View()));
        CHECK(slash.TryAppend("/Sub%2FX.spark"));
        LspScript spellings(allocator);
        spellings.Initialize(1, localhost.View())
            .Notify("initialized")
            .Open(lower.View(), 3, "\nimport Nope;\n")
            .Open(slash.View(), 1, "import Nope;\n")
            .End(2);
        const LspRun spelled = LspRunScript(allocator, {}, spellings.text.View());
        CHECK(spelled.code == LspTestClean);
        String spelledExpected(allocator);
        CHECK(spelledExpected.TryAppend("Bad.spark [S0301 0:7-0:11]\n"));
        if (lower.View() != bad.View()) {
            CHECK(spelledExpected.TryAppend("Bad.spark []\n"));
            CHECK(spelledExpected.TryAppend(lower.View()));
            CHECK(spelledExpected.TryAppend(" v3 [S0301 1:7-1:11]\n"));
        } else {
            CHECK(spelledExpected.TryAppend("Bad.spark v3 [S0301 1:7-1:11]\n"));
        }

        LspExpectText(LspPublishText(allocator, spelled.output.View(), game.View()).View(),
                      spelledExpected.View(), "uri spellings");
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SparksLsp_PositionsAreUtf16) {
    // UTF-16 열(10.6절): BOM 뒤부터, 한글 1·이모지 2(서로게이트 쌍), CRLF 줄 끝, 디스크의 잘못된
    // UTF-8 byte는 1(lexer가 첫 잘못된 byte에서 멈추므로(S0101) 그 byte 앞 글자와 범위 끝으로
    // 본다).
    SystemAllocator allocator;
    {
        LspFolder folder(allocator);
        REQUIRE(folder.Write("Game/SparksPackage.json5", LspTestCoreManifest));
        // String은 잘못된 UTF-8을 담지 않으므로 byte 그대로의 StringView로 쓴다.
        constexpr StringView Invalid = "// \xFF\xFE\n\nimport Nope;\n";
        constexpr StringView InvalidLine =
            "fn F(): int {\n    let s = \"\xED\x95\x9C\xEA\xB8\x80\xFF\xFE"
            "\"; return Nope;\n}\n";
        REQUIRE(folder.Write("Game/Invalid.spark", Invalid));
        REQUIRE(folder.Write("Game/InvalidLine.spark", InvalidLine));
        const String root = folder.UriOf("");

        String bom(allocator);
        CHECK(bom.TryAppend("\xEF\xBB\xBFimport Nope;\r\n"));
        String wide(allocator);
        CHECK(wide.TryAppend("fn F(): int {\r\n    let s = \""));
        CHECK(wide.TryAppend(LspTestEmoji));
        CHECK(wide.TryAppend(LspTestHangul));
        CHECK(wide.TryAppend("\"; return Nope;\r\n}\r\n"));
        LspScript script(allocator);
        script.Initialize(1, root.View())
            .Open(folder.UriOf("Game/Bom.spark").View(), 1, bom.View())
            .Open(folder.UriOf("Game/Wide.spark").View(), 1, wide.View());
        const LspRun run = LspRunScript(allocator, {}, script.text.View());
        const String text = LspPublishText(allocator, run.output.View(), root.View());
        // 첫 분석(didOpen Bom): 디스크 두 파일과 Bom, 둘째(didOpen Wide): Wide만 새로.
        LspExpectText(text.View(),
                      "Game/Bom.spark v1 [S0301 0:7-0:11]\n"
                      "Game/Invalid.spark [S0101 0:3-0:4]\n"
                      "Game/InvalidLine.spark [S0101 1:15-1:16]\n"
                      "Game/Wide.spark v1 [S0317 1:27-1:31]\n",
                      "utf-16 positions");
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SparksLsp_SemanticTokensGolden) {
    // 의미 강조(10.7절): 종류·modifier가 고루 나오는 사례의 토큰 golden, 디스크 모듈(열지 않은
    // 문서), 작업 공간 밖 URI는 빈 data.
    SystemAllocator allocator;
    {
        LspFolder folder(allocator);
        REQUIRE(folder.Write("Game/SparksPackage.json5", LspTestFullManifest));
        REQUIRE(folder.Write("Game/Lib.spark", "fn G(): int {\n    return 1;\n}\n"));
        const String schema = LspSchemaFolder(allocator);
        String sample(allocator);
        CHECK(sample.TryAppend("import Lib;\n"
                               "\n"
                               "enum Mood {\n"
                               "    Calm,\n"
                               "    Angry\n"
                               "}\n"
                               "\n"
                               "struct Point {\n"
                               "    var X: int = 0;\n"
                               "}\n"
                               "\n"
                               "interface Greeter {\n"
                               "    fn Greet(): int;\n"
                               "}\n"
                               "\n"
                               "const Limit: int = 3;\n"
                               "\n"
                               "class Speaker : ScriptableComponent {\n"
                               "    var Volume = 1;\n"
                               "    event Spoke(word: int);\n"
                               "\n"
                               "    async fn Wait(seconds: float) {\n"
                               "        await Delay(seconds, Clock.Game);\n"
                               "    }\n"
                               "\n"
                               "    fn Say(word: int): Mood {\n"
                               "        let total = word + Volume + Limit + G();\n"
                               "        Log.Info(\""));
        // 같은 줄의 뒤 토큰 열: 한글 1 + 이모지 2(UTF-16).
        CHECK(sample.TryAppend(StringView(LspTestHangul.Data(), 3)));
        CHECK(sample.TryAppend(LspTestEmoji));
        CHECK(sample.TryAppend("\"); emit Spoke(total);\n"
                               "        Log.Info(\"said\");\n"
                               "        return Mood.Calm;\n"
                               "    }\n"
                               "\n"
                               "    on Owner.Damaged(amount: float) {\n"
                               "        Log.Info(\"${amount}\");\n"
                               "    }\n"
                               "}\n"
                               "\n"
                               "fn Top(p: Point): int {\n"
                               "    return p.X;\n"
                               "}\n"));
        const String root = folder.UriOf("");
        const String main = folder.UriOf("Game/Main.spark");
        LspScript script(allocator);
        script.Initialize(1, root.View())
            .Open(main.View(), 1, sample.View())
            .Tokens(2, main.View())
            .Tokens(3, folder.UriOf("Game/Lib.spark").View())
            .Tokens(4, "untitled:x")
            .Tokens(5, folder.UriOf("Elsewhere/X.spark").View())
            .End(6);
        const StringView arguments[] = {"--schema", schema.View()};
        const LspRun run = LspRunScript(allocator, arguments, script.text.View());
        CHECK(run.code == LspTestClean);
        LspExpectText(LspPublishText(allocator, run.output.View(), root.View()).View(), "",
                      "sample is clean");
        const String tokens = LspTokenLines(allocator, run.output.View(), 2);
        LspExpectText(tokens.View(),
                      "0:7 3 namespace\n"
                      "2:5 4 enum declaration\n"
                      "3:4 4 enumMember declaration\n"
                      "4:4 5 enumMember declaration\n"
                      "7:7 5 struct declaration\n"
                      "8:8 1 property declaration\n"
                      "8:11 3 type defaultLibrary\n"
                      "11:10 7 interface declaration\n"
                      "12:7 5 method declaration\n"
                      "12:16 3 type defaultLibrary\n"
                      "15:6 5 variable declaration readonly static\n"
                      "15:13 3 type defaultLibrary\n"
                      "17:6 7 class declaration\n"
                      "17:16 19 class defaultLibrary\n"
                      "18:8 6 property declaration\n"
                      "19:10 5 event declaration\n"
                      "19:16 4 parameter declaration\n"
                      "19:22 3 type defaultLibrary\n"
                      "21:13 4 method declaration async\n"
                      "21:18 7 parameter declaration\n"
                      "21:27 5 type defaultLibrary\n"
                      "22:14 5 function static async defaultLibrary\n"
                      "22:20 7 parameter\n"
                      "22:29 5 type defaultLibrary\n"
                      "22:35 4 enumMember defaultLibrary\n"
                      "25:7 3 method declaration\n"
                      "25:11 4 parameter declaration\n"
                      "25:17 3 type defaultLibrary\n"
                      "25:23 4 enum\n"
                      "26:12 5 variable declaration readonly\n"
                      "26:20 4 parameter\n"
                      "26:27 6 property\n"
                      "26:36 5 variable readonly static\n"
                      "26:44 1 function static\n"
                      "27:8 3 namespace defaultLibrary\n"
                      "27:12 4 method static defaultLibrary\n"
                      "27:30 5 event\n"
                      "27:36 5 variable readonly\n"
                      "28:8 3 namespace defaultLibrary\n"
                      "28:12 4 method static defaultLibrary\n"
                      "29:15 4 enum\n"
                      "29:20 4 enumMember\n"
                      "32:7 5 property defaultLibrary\n"
                      "32:13 7 event defaultLibrary\n"
                      "32:21 6 parameter declaration\n"
                      "32:29 5 type defaultLibrary\n"
                      "33:8 3 namespace defaultLibrary\n"
                      "33:12 4 method static defaultLibrary\n"
                      "33:20 6 parameter\n"
                      "37:3 3 function declaration static\n"
                      "37:7 1 parameter declaration\n"
                      "37:10 5 struct\n"
                      "37:18 3 type defaultLibrary\n"
                      "38:11 1 parameter\n"
                      "38:13 1 property\n",
                      "semantic tokens");
        const String disk = LspTokenLines(allocator, run.output.View(), 3);
        LspExpectText(disk.View(),
                      "0:3 1 function declaration static\n"
                      "0:8 3 type defaultLibrary\n",
                      "disk module tokens");
        CHECK(LspTokenLines(allocator, run.output.View(), 4).IsEmpty());
        CHECK(LspTokenLines(allocator, run.output.View(), 5).IsEmpty());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SparksLsp_NestedPackagesAndHiddenFolders) {
    // 저장 전 새 파일은 가장 안쪽 패키지의 모듈이다(10.4절). `.` 폴더 아래는 모듈이 아니다.
    SystemAllocator allocator;
    {
        LspFolder folder(allocator);
        REQUIRE(folder.Write("Outer/SparksPackage.json5",
                             "{ name: \"Outer\", sparks: 1, profile: \"core\" }\n"));
        REQUIRE(folder.Write("Outer/Top.spark", "fn T(): int {\n    return 2;\n}\n"));
        REQUIRE(folder.Write("Outer/Inner/SparksPackage.json5",
                             "{ name: \"Inner\", sparks: 1, profile: \"core\" }\n"));
        REQUIRE(folder.Write("Outer/Inner/Sib.spark", "fn S(): int {\n    return 1;\n}\n"));
        const String root = folder.UriOf("");
        LspScript script(allocator);
        script.Initialize(1, root.View())
            .Notify("initialized")
            .Open(folder.UriOf("Outer/Inner/New.spark").View(), 1,
                  "import Sib;\nimport Nope;\n\nfn N(): int {\n    return S();\n}\n")
            .Open(folder.UriOf("Outer/.hidden/X.spark").View(), 1, "import Nope;\n")
            .Open(folder.UriOf("Outer/Inner/.cache/Y.spark").View(), 1, "import Nope;\n")
            .End(2);
        const LspRun run = LspRunScript(allocator, {}, script.text.View());
        CHECK(run.code == LspTestClean);
        LspExpectText(LspPublishText(allocator, run.output.View(), root.View()).View(),
                      "Outer/Inner/New.spark v1 [S0301 1:7-1:11]\n", "nested packages");
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SparksLsp_DocumentStoreEdges) {
    // 문서 저장소 경계(10.4절): 열린 문서 상한, 열지 않은 문서의 바꿈·닫기, 같은 URI 다시 열기,
    // 부분 바꿈(range) 거부, manifest 덮기.
    SystemAllocator allocator;
    {
        // 상한: 4,096개 뒤의 didOpen은 기록하고 무시한다(작업 공간 없음 — 분석 없이 표만 본다).
        constexpr uint32 Documents = 4096;
        LspScript many(allocator);
        many.Initialize(1, {});
        for (uint32 index = 0; index <= Documents; ++index) {
            StringBuilder<64> uri(allocator);
            uri.AppendFormat("untitled:{}", index);
            many.Open(uri.View(), 1, "fn F() {\n}\n");
        }

        many.End(2);
        const LspRun limited = LspRunScript(allocator, {}, many.text.View());
        CHECK(limited.code == LspTestClean);
        String limitedLog(allocator);
        CHECK(limitedLog.TryAppend(LspTestNoWorkspaceLog));
        CHECK(limitedLog.TryAppend("sparks-lsp: too many open documents; the document is "
                                   "ignored\n"));
        LspExpectText(limited.log.View(), limitedLog.View(), "document limit");

        LspFolder folder(allocator);
        REQUIRE(folder.Write("Game/SparksPackage.json5", LspTestCoreManifest));
        REQUIRE(folder.Write("Game/Bad.spark", "import Nope;\n"));
        const String root = folder.UriOf("");
        const String bad = folder.UriOf("Game/Bad.spark");
        const String manifest = folder.UriOf("Game/SparksPackage.json5");
        String ranged(allocator);
        CHECK(ranged.TryAppend("{\"textDocument\":{\"uri\":"));
        LspQuote(ranged, bad.View());
        CHECK(ranged.TryAppend(",\"version\":3},\"contentChanges\":[{\"range\":{\"start\":{"
                               "\"line\":0,\"character\":0},\"end\":{\"line\":0,\"character\":"
                               "0}},\"text\":\"x\"}]}"));
        LspScript script(allocator);
        script.Initialize(1, root.View())
            .Notify("initialized")
            .Change(bad.View(), 1, "fn B(): int {\n    return 2;\n}\n") // 열지 않음 — 무시
            .Close(bad.View())                                          // 열지 않음 — 무시
            .Open(bad.View(), 1, "\nimport Nope;\n")
            .Open(bad.View(), 2, "\n\nimport Nope;\n") // 다시 열기 — 내용·version 바꿈
            .Notify("textDocument/didChange", ranged.View())
            .Open(manifest.View(), 1,
                  "{ name: \"Game\", sparks: 1, profile: \"core\", bogus: 1 }\n")
            .Close(manifest.View())
            .End(2);
        const LspRun run = LspRunScript(allocator, {}, script.text.View());
        CHECK(run.code == LspTestClean);
        LspExpectText(LspPublishText(allocator, run.output.View(), root.View()).View(),
                      "Game/Bad.spark [S0301 0:7-0:11]\n"
                      "Game/Bad.spark v1 [S0301 1:7-1:11]\n"
                      "Game/Bad.spark v2 [S0301 2:7-2:11]\n"
                      "Game/Bad.spark v2 []\n" // manifest가 깨져 패키지 모듈이 없다
                      "Game/SparksPackage.json5 v1 [S1102 0:51-0:52]\n"
                      "Game/Bad.spark v2 [S0301 2:7-2:11]\n"
                      "Game/SparksPackage.json5 []\n",
                      "document store");
        LspExpectText(run.log.View(),
                      "sparks-lsp: 'textDocument/didChange' names no open document or has no "
                      "change; it is ignored\n"
                      "sparks-lsp: 'textDocument/didChange' is not a full-content change; it is "
                      "ignored\n",
                      "document store log");
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SparksLsp_SchemaFolders) {
    // schema 폴더(10.1절): 초기화 옵션의 상대 경로는 첫 뿌리 기준, 명령줄 상대 경로는 작업 디렉터리
    // 기준 — 둘 다 발행 URI가 절대 경로다.
    SystemAllocator allocator;
    {
        LspFolder folder(allocator);
        REQUIRE(folder.Write("Game/SparksPackage.json5", LspTestFullManifest));
        REQUIRE(folder.Write("Game/Main.spark", "class C : ScriptableComponent {\n}\n"));
        REQUIRE(LspCopySchema(allocator, folder, "Schemas"));
        const String root = folder.UriOf("");

        // schema 없음 — 기반 타입을 모른다.
        LspScript without(allocator);
        without.Initialize(1, root.View()).Notify("initialized").End(2);
        const LspRun missing = LspRunScript(allocator, {}, without.text.View());
        CHECK(LspPublishText(allocator, missing.output.View(), root.View())
                  .View()
                  .StartsWith("Game/Main.spark [S0"));

        // 초기화 옵션 `Schemas`(뿌리 기준) — 진단 없음.
        String params(allocator);
        CHECK(params.TryAppend("{\"capabilities\":{},\"rootUri\":"));
        LspQuote(params, root.View());
        CHECK(params.TryAppend(",\"initializationOptions\":{\"schemas\":[\"Schemas\"]}}"));
        LspScript with(allocator);
        with.Call(1, "initialize", params.View()).Notify("initialized").End(2);
        const LspRun found = LspRunScript(allocator, {}, with.text.View());
        CHECK(found.code == LspTestClean);
        LspExpectText(LspPublishText(allocator, found.output.View(), root.View()).View(), "",
                      "relative schema option");

        // 명령줄 상대 경로(작업 디렉터리 기준): 깨진 schema의 진단 URI가 절대 경로다.
        char working[LspTestPathCapacity];
        usize bytes = 0;
        REQUIRE(platform::GetStandardDirectory(platform::StandardDirectory::Working, working,
                                               LspTestPathCapacity, bytes));
        StringBuilder<128> relative(allocator);
        relative.AppendFormat("rocks-sparks-lsp-schema-{}-{}", platform::CurrentProcessId(),
                              platform::MonotonicTicks());
        StringBuilder<LspTestPathCapacity> full(allocator);
        full.AppendFormat("{}/{}", StringView(working, bytes), relative.View());
        REQUIRE(!relative.HasFailed() && !full.HasFailed());
        platform::FileSystem& files = platform::PhysicalFileSystem();
        REQUIRE(platform::MakeDirectoryTree(files, full.View().Data(), full.View().ByteCount()));
        StringBuilder<LspTestPathCapacity> file(allocator);
        file.AppendFormat("{}/Bad.sparkbind.json", full.View());
        {
            platform::File handle;
            REQUIRE(handle.Open(files, file.View().Data(), file.View().ByteCount(),
                                platform::FileOpenMode::CreateAlways));
            CHECK(handle.WriteAll("{", 1).status);
            CHECK(handle.Close());
        }

        const StringView arguments[] = {"--schema", relative.View()};
        LspScript broken(allocator);
        broken.Initialize(1, root.View()).Notify("initialized").End(2);
        const LspRun run = LspRunScript(allocator, arguments, broken.text.View());
        static_cast<void>(
            platform::DeleteDirectoryTree(files, full.View().Data(), full.View().ByteCount()));
        CHECK(run.code == LspTestClean);
        const String uri = LspUriOf(allocator, file.View());
        const String published = LspPublishText(allocator, run.output.View(), {});
        // 깨진 schema라 Main의 기반 타입도 모른다 — schema 파일 진단 줄만 본다.
        String line(allocator);
        CHECK(line.TryAppend(uri.View()));
        CHECK(line.TryAppend(" [S1120 0:1-0:1]\n"));
        CHECK(published.View().Contains(line.View(), StringComparison::CaseSensitive));
        if (!published.View().Contains(line.View(), StringComparison::CaseSensitive)) {
            LspExpectText(published.View(), line.View(), "relative schema argument");
        }
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

namespace {

// 폴더 항목 이름을 모은다(이름 byte 순 — 파일 체계 순서 그대로가 아니라 정렬한다).
struct LspListed {
    explicit LspListed(Allocator& allocator) noexcept : names(allocator), folders(allocator) {
    }

    Array<String> names;
    Array<uint8> folders;
};

bool LspListFolder(Allocator& allocator, StringView path, LspListed& out) noexcept {
    struct Context {
        Allocator* allocator;
        LspListed* out;
        bool ok;
    } context{&allocator, &out, true};
    auto collect = [](const platform::DirectoryEntry& entry, void* raw) noexcept {
        auto& list = *static_cast<Context*>(raw);
        String name(*list.allocator);
        list.ok =
            list.ok && name.TryAssign(StringView(entry.name, entry.nameBytes)) &&
            list.out->names.TryPushBack(Move(name)) &&
            list.out->folders.TryPushBack(entry.info.type == platform::FileType::Directory ? 1 : 0);
        return list.ok;
    };
    return platform::PhysicalFileSystem().EnumerateDirectory(path.Data(), path.ByteCount(), collect,
                                                             &context) &&
           context.ok;
}

// `.expect`의 진단 지시 줄을 모은다. stage 지시가 있으면 false(비교하지 않는다).
bool LspExpectedLines(StringView expect, Array<String>& lines, Allocator& allocator) noexcept {
    usize at = 0;
    while (at < expect.ByteCount()) {
        usize end = at;
        while (end < expect.ByteCount() && expect.Data()[end] != '\n') {
            ++end;
        }

        StringView line(expect.Data() + at, end - at);
        at = end + 1;
        while (!line.IsEmpty() && (line.Data()[line.ByteCount() - 1] == '\r' ||
                                   line.Data()[line.ByteCount() - 1] == ' ')) {
            line = StringView(line.Data(), line.ByteCount() - 1);
        }

        if (line.IsEmpty() || line.StartsWith("#") || line == "clean") {
            continue;
        }

        if (line.StartsWith("stage ")) {
            return false;
        }

        String copy(allocator);
        if (!copy.TryAssign(line) || !lines.TryPushBack(Move(copy))) {
            return false;
        }
    }

    return true;
}

// 발행 진단을 `.expect` 줄 모양(`error <코드>[ <경로>][ <줄:열>]` — 1부터 센 줄·code point 열)으로
// 모은다. casePrefix·schemaPrefix는 경로 비교 모양 + `/`다. 0:0–0:0 범위는 위치 없는 진단이다.
// corpus에는 U+10000 이상 글자가 없어 UTF-16 열 + 1이 code point 열이다.
bool LspActualLines(Allocator& allocator, StringView output, StringView casePrefix,
                    StringView schemaPrefix, bool folder, Array<String>& lines) noexcept {
    Array<String> bodies(allocator);
    if (!LspBodies(output, bodies, allocator)) {
        return false;
    }

    for (const String& body : bodies) {
        JsonDocument document(allocator);
        if (!document.Parse(body.View()).Succeeded()) {
            return false;
        }

        const JsonValue root = document.Root();
        const JsonValue method = root.Find("method");
        if (!method.IsValid() || method.GetString() != "textDocument/publishDiagnostics") {
            continue;
        }

        const JsonValue params = root.Find("params");
        const String path = LspPathOfUri(allocator, params.Find("uri").GetString());
        const bool caseFile = path.View().StartsWith(casePrefix, StringComparison::CaseSensitive);
        const bool schemaFile =
            path.View().StartsWith(schemaPrefix, StringComparison::CaseSensitive);
        const StringView prefix = caseFile ? casePrefix : schemaPrefix;
        const StringView relative = caseFile || schemaFile
                                        ? StringView(path.View().Data() + prefix.ByteCount(),
                                                     path.ByteCount() - prefix.ByteCount())
                                        : path.View();
        const JsonValue diagnostics = params.Find("diagnostics");
        for (usize at = 0; at < diagnostics.Size(); ++at) {
            const JsonValue diagnostic = diagnostics.At(at);
            const JsonValue start = diagnostic.Find("range").Find("start");
            const JsonValue end = diagnostic.Find("range").Find("end");
            const bool positioned =
                LspNumber(start.Find("line")) != 0 || LspNumber(start.Find("character")) != 0 ||
                LspNumber(end.Find("line")) != 0 || LspNumber(end.Find("character")) != 0;
            String text(allocator);
            bool ok =
                text.TryAppend("error ") && text.TryAppend(diagnostic.Find("code").GetString());
            if (schemaFile) {
                ok = ok && text.TryAppend(" Schema/") && text.TryAppend(relative);
            } else if (folder || !caseFile) {
                ok = ok && text.TryAppend(" ") && text.TryAppend(relative);
            }

            if (positioned) {
                ok = ok && text.TryAppend(" ");
                LspAppendNumber(text, LspNumber(start.Find("line")) + 1, allocator);
                ok = ok && text.TryAppend(":");
                LspAppendNumber(text, LspNumber(start.Find("character")) + 1, allocator);
            }

            if (!ok || !lines.TryPushBack(Move(text))) {
                return false;
            }
        }
    }

    return true;
}

void LspSortLines(Array<String>& lines) noexcept {
    algo::Sort(lines.View(), [](const String& left, const String& right) noexcept {
        return StringView::CompareBytes(left.View(), right.View()) < 0;
    });
}

} // namespace

TEST(SparksLsp_MatchesConformanceCorpus) {
    // 진단 동등성(10.8절): CLI 동등성 시험(SparksTools_CheckMatchesConformanceCorpus)과 같은 사례
    // 집합을 작업 공간으로 열어(initialize → initialized) 발행 진단(코드·경로·위치)이 `.expect`와
    // 같다. 순서는 발행 단위(URI 순)라 양쪽을 정렬해 비교한다. 파일 사례는 임시 패키지
    // Main.spark다.
    SystemAllocator allocator;
    {
        char engine[LspTestPathCapacity];
        auto found = FindEngineDirectory(Span<char>(engine, LspTestPathCapacity));
        REQUIRE(found);
        auto corpus = Path::Combine(allocator, StringView(engine, found.Value()),
                                    "Source/Developer/SparksTestHost/Conformance");
        REQUIRE(corpus);
        const String schema = LspSchemaFolder(allocator);
        const String schemaKey = LspKeyOf(allocator, schema.View());
        StringBuilder<LspTestPathCapacity> schemaPrefix(allocator);
        REQUIRE(schemaPrefix.TryAppendFormat("{}/", schemaKey.View()));
        LspFolder temp(allocator);
        REQUIRE(temp.Write("Case/SparksPackage.json5",
                           "{ name: \"Test\", sparks: 1, profile: \"full\" }\n"));
        LspListed areas(allocator);
        REQUIRE(LspListFolder(allocator, corpus.Value().View(), areas));
        uint32 compared = 0;
        uint32 mismatched = 0;
        for (usize area = 0; area < areas.names.Size(); ++area) {
            if (areas.folders[area] == 0) {
                continue;
            }

            auto areaPath =
                Path::Combine(allocator, corpus.Value().View(), areas.names[area].View());
            REQUIRE(areaPath);
            LspListed items(allocator);
            REQUIRE(LspListFolder(allocator, areaPath.Value().View(), items));
            for (usize at = 0; at < items.names.Size(); ++at) {
                const StringView name = items.names[at].View();
                const bool folder = items.folders[at] != 0;
                const bool file = !folder && name.EndsWith(".spark");
                if (!folder && !file) {
                    continue;
                }

                const StringView stem(name.Data(), file ? name.ByteCount() - 6 : name.ByteCount());
                StringBuilder<256> expectName(allocator);
                REQUIRE(expectName.TryAppendFormat("{}.expect", stem));
                auto expectPath =
                    Path::Combine(allocator, areaPath.Value().View(), expectName.View());
                REQUIRE(expectPath);
                String expect(allocator);
                if (!LspReadText(allocator, expectPath.Value().View(), expect)) {
                    continue;
                }

                Array<String> expected(allocator);
                if (!LspExpectedLines(expect.View(), expected, allocator)) {
                    continue;
                }

                auto casePath = Path::Combine(allocator, areaPath.Value().View(), name);
                REQUIRE(casePath);
                String target(allocator);
                if (file) {
                    String source(allocator);
                    REQUIRE(LspReadText(allocator, casePath.Value().View(), source));
                    REQUIRE(temp.Write("Case/Main.spark", source.View()));
                    target = temp.PathOf("Case");
                } else {
                    REQUIRE(target.TryAssign(casePath.Value().View()));
                }

                const String targetKey = LspKeyOf(allocator, target.View());
                StringBuilder<LspTestPathCapacity> prefix(allocator);
                REQUIRE(prefix.TryAppendFormat("{}/", targetKey.View()));
                const String rootUri = LspUriOf(allocator, target.View());
                LspScript script(allocator);
                script.Initialize(1, rootUri.View()).Notify("initialized").End(2);
                const StringView arguments[] = {"--schema", schema.View()};
                const LspRun run = LspRunScript(allocator, arguments, script.text.View());
                Array<String> actual(allocator);
                REQUIRE(LspActualLines(allocator, run.output.View(), prefix.View(),
                                       schemaPrefix.View(), folder, actual));
                LspSortLines(actual);
                LspSortLines(expected);
                bool same = run.code == LspTestClean && actual.Size() == expected.Size();
                for (usize index = 0; same && index < actual.Size(); ++index) {
                    same = actual[index].View() == expected[index].View();
                }

                ++compared;
                if (!same) {
                    ++mismatched;
                    StringBuilder<4096> report(allocator);
                    report.AppendFormat("LSP differs from the corpus: {}/{} (exit {})\n",
                                        areas.names[area].View(), name, run.code);
                    for (const String& line : expected) {
                        report.AppendFormat("  expected {}\n", line.View());
                    }

                    for (const String& line : actual) {
                        report.AppendFormat("  actual   {}\n", line.View());
                    }

                    if (!run.log.IsEmpty()) {
                        report.AppendFormat("  log      {}", run.log.View());
                    }

                    platform::WriteDiagnostic(report.View().Data(), report.View().ByteCount());
                }
            }
        }

        CHECK(compared > 0);
        CHECK(mismatched == 0);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

} // namespace sparks_tools_lsp_analysis_tests_file
