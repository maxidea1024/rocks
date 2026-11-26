#include "SparksToolsLspTestSupport.h"

// 언어 서버의 틀·수명 시험이다(Docs/Design/SparksTools.ko.md 10.2절·10.3절·10.8절 — LC8e):
// 각본 입력으로 RunLanguageServer를 같은 프로세스에서 돌려 출력 byte·종료 코드·기록을 대조하고,
// 짧은 세션의 할당 실패 전수를 본다.
using namespace rocks;
namespace sparks_tools_lsp_protocol_tests_file {

using namespace rocks::test::sparks_lsp;

namespace {

// 응답 본문(`{"jsonrpc":"2.0","id":<id>,"error":{"code":<code>,"message":"<message>"}}`)이다.
String LspErrorBody(Allocator& allocator, StringView id, int64 code, StringView message) noexcept {
    StringBuilder<256> text(allocator);
    text.AppendFormat(
        "{{\"jsonrpc\":\"2.0\",\"id\":{},\"error\":{{\"code\":{},\"message\":\"{}\"}}}}\n", id,
        code, message);
    String out(allocator);
    CHECK(out.TryAssign(text.View()));
    return out;
}

// initialize 결과 본문이다(10.3절).
String LspInitializeBody(Allocator& allocator, StringView id) noexcept {
    String out(allocator);
    CHECK(out.TryAppend("{\"jsonrpc\":\"2.0\",\"id\":"));
    CHECK(out.TryAppend(id));
    CHECK(out.TryAppend(",\"result\":{\"capabilities\":{\"positionEncoding\":\"utf-16\","
                        "\"textDocumentSync\":{\"openClose\":true,\"change\":1},"
                        "\"hoverProvider\":true,\"definitionProvider\":true,"
                        "\"documentFormattingProvider\":true,"
                        "\"completionProvider\":{\"triggerCharacters\":[\".\",\"@\"]},"
                        "\"codeActionProvider\":{\"codeActionKinds\":[\"quickfix\"]},"
                        "\"semanticTokensProvider\":{\"legend\":{\"tokenTypes\":["));
    for (usize at = 0; at < sizeof(LspTestTypes) / sizeof(LspTestTypes[0]); ++at) {
        CHECK(out.TryAppend(at == 0 ? "\"" : ",\""));
        CHECK(out.TryAppend(LspTestTypes[at]));
        CHECK(out.TryAppend("\""));
    }

    CHECK(out.TryAppend("],\"tokenModifiers\":["));
    for (usize at = 0; at < sizeof(LspTestModifiers) / sizeof(LspTestModifiers[0]); ++at) {
        CHECK(out.TryAppend(at == 0 ? "\"" : ",\""));
        CHECK(out.TryAppend(LspTestModifiers[at]));
        CHECK(out.TryAppend("\""));
    }

    CHECK(out.TryAppend("]},\"full\":true}},\"serverInfo\":{\"name\":\"sparks-lsp\"}}}\n"));
    return out;
}

// 수명 오류 각본(작업 공간 없음 — 출력에 경로가 없다)과 그 출력 golden이다.
void LspLifecycleScript(LspScript& script) noexcept {
    script.Tokens(1, "untitled:x")
        .Open("untitled:y", 1, "fn F() {\n}\n")
        .Message("{\"jsonrpc\":\"2.0\",\"id\":\"s\",\"method\":\"initialize\",\"params\":5}")
        .Initialize(2, {})
        .Initialize(3, {})
        .Message("not json")
        .Message("{\"\xFF\"}")   // 잘못된 UTF-8 본문
        .Message("\"\xED\x95\"") // 길이가 글자를 반으로 자른 본문
        .Message("[1,2]")
        .Message("{\"jsonrpc\":\"2.0\",\"id\":true,\"method\":\"x\"}")
        .Message("{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":7}")
        .Message("{\"jsonrpc\":\"2.0\",\"id\":9,\"result\":null}")
        .Notify("$/cancelRequest", "{\"id\":1}")
        .Notify("workspace/didChangeConfiguration", "{}")
        .Call(5, "textDocument/references", "{}")
        .Call(6, "textDocument/semanticTokens/full", "{\"textDocument\":{}}")
        .Tokens(7, "untitled:x")
        .Initialize(8, {}) // 두 번째 initialize
        .Call(9, "shutdown")
        .Call(10, "textDocument/hover", "{}")
        .Open("untitled:z", 1, "fn G() {\n}\n")
        .Notify("exit");
}

String LspLifecycleGolden(Allocator& allocator) noexcept {
    String out(allocator);
    auto add = [&](const String& body) noexcept { CHECK(out.TryAppend(body.View())); };
    add(LspErrorBody(allocator, "1", -32002, "the server is not initialized"));
    add(LspErrorBody(allocator, "\"s\"", -32602, "'initialize' needs an object of parameters"));
    add(LspInitializeBody(allocator, "2"));
    add(LspErrorBody(allocator, "3", -32600, "the server is already initialized"));
    for (uint32 count = 0; count < 3; ++count) {
        add(LspErrorBody(allocator, "null", -32700, "the message is not JSON"));
    }

    add(LspErrorBody(allocator, "null", -32600,
                     "the message is not a JSON-RPC request or notification"));
    add(LspErrorBody(allocator, "null", -32600,
                     "the message is not a JSON-RPC request or notification"));
    add(LspErrorBody(allocator, "4", -32600,
                     "the message is not a JSON-RPC request or notification"));
    add(LspErrorBody(allocator, "5", -32601, "the method is not supported"));
    add(LspErrorBody(allocator, "6", -32602, "the request needs 'textDocument.uri'"));
    String tokens(allocator);
    CHECK(tokens.TryAssign("{\"jsonrpc\":\"2.0\",\"id\":7,\"result\":{\"data\":[]}}\n"));
    add(tokens);
    add(LspErrorBody(allocator, "8", -32600, "the server is already initialized"));
    String shutdown(allocator);
    CHECK(shutdown.TryAssign("{\"jsonrpc\":\"2.0\",\"id\":9,\"result\":null}\n"));
    add(shutdown);
    add(LspErrorBody(allocator, "10", -32600, "the server is shutting down"));
    return out;
}

} // namespace

TEST(SparksLsp_LifecycleGolden) {
    // 수명·JSON-RPC 오류 전부(10.2절·10.3절)를 한 각본으로: 출력 byte 전체가 golden과 같고 0으로
    // 끝난다(shutdown 뒤 exit). 초기화 전·shutdown 뒤 알림은 버린다(출력 없음).
    SystemAllocator allocator;
    {
        LspScript script(allocator);
        LspLifecycleScript(script);
        const StringView arguments[] = {"--stdio"};
        const LspRun run = LspRunScript(allocator, arguments, script.text.View());
        CHECK(run.code == LspTestClean);
        const String golden = LspLifecycleGolden(allocator);
        const String joined = LspJoined(allocator, run.output.View());
        LspExpectText(joined.View(), golden.View(), "lifecycle golden");
        LspExpectText(run.log.View(), LspTestNoWorkspaceLog, "lifecycle log");
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SparksLsp_ExitCodesAndUsage) {
    SystemAllocator allocator;
    {
        // exit만(shutdown 없음) — 1.
        LspScript exitOnly(allocator);
        exitOnly.Notify("exit");
        CHECK(LspRunScript(allocator, {}, exitOnly.text.View()).code == LspTestUnclean);

        // initialize 뒤 exit — 1.
        LspScript noShutdown(allocator);
        noShutdown.Initialize(1, {}).Notify("exit");
        const LspRun unclean = LspRunScript(allocator, {}, noShutdown.text.View());
        CHECK(unclean.code == LspTestUnclean);

        // 입력 끝(메시지 경계) — 1과 기록 한 줄.
        LspScript ended(allocator);
        ended.Initialize(1, {}).Call(2, "shutdown");
        const LspRun end = LspRunScript(allocator, {}, ended.text.View());
        CHECK(end.code == LspTestUnclean);
        String endLog(allocator);
        CHECK(endLog.TryAppend(LspTestNoWorkspaceLog));
        CHECK(endLog.TryAppend("sparks-lsp: the input ended without 'exit'\n"));
        LspExpectText(end.log.View(), endLog.View(), "end log");

        // 빈 입력 — 1.
        CHECK(LspRunScript(allocator, {}, "").code == LspTestUnclean);

        // 인자 오류 — 2와 사용법(입력을 읽지 않는다).
        const StringView bogus[] = {"--bogus"};
        const StringView schemaOnly[] = {"--stdio", "--schema"};
        const StringView positional[] = {"folder"};
        const Span<const StringView> cases[] = {bogus, schemaOnly, positional};
        for (const Span<const StringView> arguments : cases) {
            const LspRun usage = LspRunScript(allocator, arguments, exitOnly.text.View());
            CHECK(usage.code == LspTestUsage);
            CHECK(usage.output.IsEmpty());
            CHECK(usage.log.View().StartsWith("sparks-lsp: unknown argument '"));
            CHECK(
                usage.log.View().EndsWith("usage: sparks-lsp [--stdio] [--schema <folder>]...\n"));
        }

        // 입출력 오류 — 3.
        {
            LspScriptChannel channel(allocator, noShutdown.text.View(), LspTestWholeChunk);
            channel.failRead = 1;
            CHECK(RunLanguageServer(allocator, platform::PhysicalFileSystem(), {}, channel) ==
                  LspTestError);
        }

        {
            LspScriptChannel channel(allocator, noShutdown.text.View(), LspTestWholeChunk);
            channel.failWrite = 1;
            CHECK(RunLanguageServer(allocator, platform::PhysicalFileSystem(), {}, channel) ==
                  LspTestError);
        }
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SparksLsp_FramingChunksAndHeaders) {
    SystemAllocator allocator;
    {
        // 같은 각본을 조각 크기 1·2·7·전체로 나눠 줘도 출력·종료 코드가 같다.
        LspScript script(allocator);
        LspLifecycleScript(script);
        const String golden = LspLifecycleGolden(allocator);
        const usize chunks[] = {1, 2, 7, LspTestWholeChunk};
        for (const usize chunk : chunks) {
            const LspRun run = LspRunScript(allocator, {}, script.text.View(), chunk);
            CHECK(run.code == LspTestClean);
            const String joined = LspJoined(allocator, run.output.View());
            LspExpectText(joined.View(), golden.View(), "chunked lifecycle");
        }

        // 머리: 이름은 대소문자 무시, 값 앞뒤 공백, Content-Type·다른 머리는 무시, 머리 줄
        // 1,024 byte(상한 그대로)는 받는다.
        String pad(allocator);
        CHECK(pad.TryAppend("X-Pad: "));
        while (pad.ByteCount() < LspTestHeaderLimit) {
            CHECK(pad.TryAppend("a"));
        }

        constexpr StringView Shutdown = "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"shutdown\"}";
        constexpr StringView Exit = "{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}";
        LspScript headers(allocator);
        headers.Initialize(1, {});
        StringBuilder<2048> raw(allocator);
        raw.AppendFormat("content-length:   {}  \r\nContent-Type: application/vscode-jsonrpc; "
                         "charset=utf-8\r\n{}\r\n\r\n{}",
                         Shutdown.ByteCount(), pad.View(), Shutdown);
        raw.AppendFormat("CONTENT-LENGTH:{}\r\n\r\n{}", Exit.ByteCount(), Exit);
        CHECK(!raw.HasFailed());
        headers.Raw(raw.View());
        const LspRun accepted = LspRunScript(allocator, {}, headers.text.View(), 3);
        CHECK(accepted.code == LspTestClean);
        CHECK(accepted.log.View() == LspTestNoWorkspaceLog);

        // 깨진 틀 — 표준 오류 한 줄과 1. 앞의 메시지는 처리된 뒤다.
        String longLine(allocator);
        CHECK(longLine.TryAppend("X-Long: "));
        while (longLine.ByteCount() <= LspTestHeaderLimit) {
            CHECK(longLine.TryAppend("b"));
        }

        String unterminated(allocator); // 줄 끝 없이 상한을 넘는 머리
        while (unterminated.ByteCount() <= LspTestHeaderLimit + 1) {
            CHECK(unterminated.TryAppend("c"));
        }

        String block(allocator); // 짧은 줄들로 머리 전체 상한을 넘김
        while (block.ByteCount() <= LspTestHeaderBlockLimit + 8) {
            CHECK(block.TryAppend("X-Many: 1\r\n"));
        }

        CHECK(block.TryAppend("X-Many: 1"));
        String completeBlock(allocator); // 다 끝난 줄들만으로 머리 전체 상한을 넘김
        while (completeBlock.ByteCount() <= LspTestHeaderBlockLimit) {
            CHECK(completeBlock.TryAppend("X-Many: 1\r\n"));
        }

        CHECK(completeBlock.TryAppend("Content-Length: 2\r\n\r\n{}"));
        struct Broken {
            StringView bytes;
            StringView message;
        };

        String longHeader(allocator);
        CHECK(longHeader.TryAppend(longLine.View()));
        CHECK(longHeader.TryAppend("\r\n\r\n"));
        const Broken cases[] = {
            {"Content-Type: x\r\n\r\n{}", "a message has no Content-Length"},
            {"\r\n{}", "a message has no Content-Length"},
            {"Content-Length: 1x\r\n\r\n{}", "Content-Length is not a number up to 64 MiB"},
            {"Content-Length: \r\n\r\n{}", "Content-Length is not a number up to 64 MiB"},
            {"Content-Length: -1\r\n\r\n{}", "Content-Length is not a number up to 64 MiB"},
            {"Content-Length: 67108865\r\n\r\n{}", "Content-Length is not a number up to 64 MiB"},
            {"Content-Length: 99999999999999999999999\r\n\r\n",
             "Content-Length is not a number up to 64 MiB"},
            {"Content-Length: 2\r\nContent-Length: 2\r\n\r\n{}", "Content-Length is given twice"},
            {"NoColon\r\n\r\n{}", "a message header has no ':'"},
            {longHeader.View(), "a message header is too long"},
            {unterminated.View(), "a message header is too long"},
            {block.View(), "a message header is too long"},
            {completeBlock.View(), "a message header is too long"},
            {"Content-Length: 10\r\n\r\n{}", "the input ended inside a message"},
            {"Content-Length: 67108864\r\n\r\n{}", "the input ended inside a message"},
            {"Content-Len", "the input ended inside a message"},
            {"Content-Length: 2\r\n", "the input ended inside a message"},
        };
        for (const Broken& entry : cases) {
            const usize sizes[] = {1, LspTestWholeChunk};
            for (const usize chunk : sizes) {
                LspScript broken(allocator);
                broken.Initialize(1, {}).Raw(entry.bytes);
                const LspRun run = LspRunScript(allocator, {}, broken.text.View(), chunk);
                CHECK(run.code == LspTestUnclean);
                const String initialize = LspInitializeBody(allocator, "1");
                const String joined = LspJoined(allocator, run.output.View());
                CHECK(joined.View() == initialize.View());
                StringBuilder<256> expected(allocator);
                expected.AppendFormat("{}sparks-lsp: {}\n", LspTestNoWorkspaceLog, entry.message);
                LspExpectText(run.log.View(), expected.View(), entry.message);
            }
        }
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SparksLsp_AllocationFailure) {
    // 세 세션: (1) 열기·강조·바꿈·JSON 아님·닫기, (2) workspaceFolders·초기화 옵션 schema(뿌리
    // 기준 상대)·저장 전 새 파일(덮기 모듈 추가)·디스크 진단 덮어 지움·닫아 지움(발행 표에서 빼기),
    // (3) 없는 작업 공간(읽기 실패 기록)·잘못된 UTF-8 본문.
    SystemAllocator backing;
    LspFolder folder(backing);
    REQUIRE(folder.Write("Game/SparksPackage.json5", LspTestCoreManifest));
    REQUIRE(folder.Write("Game/Main.spark", "import Lib;\n\nfn F(): int {\n    return G();\n}\n"));
    REQUIRE(folder.Write("Game/Lib.spark", "fn G(): int {\n    return 1;\n}\n"));
    REQUIRE(folder.Write("Game/Bad.spark", "import Nope;\n"));
    const String root = folder.UriOf("");
    const String lib = folder.UriOf("Game/Lib.spark");
    const String bad = folder.UriOf("Game/Bad.spark");
    const String fresh = folder.UriOf("Game/Sub/New.spark");
    const StringView arguments[] = {"--stdio"};
    {
        LspScript script(backing);
        script.Initialize(1, root.View())
            .Notify("initialized")
            .Open(lib.View(), 1, "fn H(): int {\n    return 1;\n}\n")
            .Tokens(2, lib.View())
            .Change(lib.View(), 2, "fn G(): int {\n    return 2;\n}\n")
            .Message("not json")
            .Close(lib.View())
            .End(3);
        LspSweepAllocations(backing, arguments, script.text.View(), LspTestClean);
    }

    {
        REQUIRE(LspCopySchema(backing, folder, "Schemas"));
        String params(backing);
        CHECK(params.TryAppend("{\"capabilities\":{},\"workspaceFolders\":[{\"uri\":"));
        LspQuote(params, root.View());
        CHECK(params.TryAppend(
            ",\"name\":\"root\"}],\"initializationOptions\":{\"schemas\":[\"Schemas\"]}}"));
        LspScript script(backing);
        script.Call(1, "initialize", params.View())
            .Notify("initialized")
            .Open(fresh.View(), 1, "import Nope;\n")
            .Open(bad.View(), 1, "fn B(): int {\n    return 2;\n}\n")
            .Close(fresh.View())
            .Close(bad.View())
            .End(2);
        LspSweepAllocations(backing, arguments, script.text.View(), LspTestClean);
    }

    {
        const String missing = folder.UriOf("Missing");
        LspScript script(backing);
        script.Initialize(1, missing.View())
            .Notify("initialized")
            .Open(folder.UriOf("Missing/A.spark").View(), 1, "import Nope;\n")
            .Message("{\"\xFF\"}")
            .End(2);
        LspSweepAllocations(backing, arguments, script.text.View(), LspTestClean);
    }
}

} // namespace sparks_tools_lsp_protocol_tests_file
