#include "SparksToolsLspTestSupport.h"

// 언어 서버의 위치 질의 시험이다(Docs/Design/SparksTools.ko.md 11.2–11.5절·11.8절 — LC8f1): hover
// golden(종류마다 — 문서 주석·추론 타입·엔진 schema 위치), 정의 이동, 서식, 위치 역변환, 할당 실패.
using namespace rocks;
namespace sparks_tools_lsp_query_tests_file {

using namespace rocks::test::sparks_lsp;

namespace {

// hover·정의 이동 사례 원문이다(ASCII — LspPlaceOf가 열을 바로 센다).
constexpr StringView LspQuerySample = R"(import Lib;
import Tools.Kit as Kit;

/// A door that opens.
/// Second line.
class Door : ScriptableComponent, Greeter {
    /// How far it swings.
    var Swing: float = 1.0;
    let Label: string = "door";
    var Next: Door?;
    var Marks: Array<int> = [];
    event Opened(by: Actor, _: int);

    async fn Open(speed: float): int {
        let total = speed + Swing;
        await Delay(total, Clock.Game);
        emit Opened(Owner, 1);
        Log.Info(Label);
        _ = Owner.GetForwardVector();
        _ = CollisionChannel.Camera;
        _ = World.Now();
        return G();
    }

    fn Greet(): int {
        return Kit.K();
    }

    fn Pick(d: Door): Mood {
        Swing = d.Swing * 2.0;
        return Mood.Calm;
    }
}

struct Point {
    var X: int = 0;
}

interface Greeter {
    fn Greet(): int;
}

enum Mood {
    Calm,
    Angry
}

const Limit: int = 3;

fn Top(p: int): int {
    return p + Limit;
}
)";

struct LspProbe {
    StringView label;
    StringView needle;
    uint32 occurrence;
    uint32 shift;
};

// 사례 원문의 이름마다(첫 글자 다음 자리 — 토큰 안쪽).
constexpr LspProbe LspQueryProbes[] = {
    {"import", "Lib;", 0, 1},
    {"import path", "Tools.", 0, 1},
    {"import module", "Kit as", 0, 1},
    {"alias decl", "Kit;", 0, 1},
    {"alias use", "Kit.K", 0, 1},
    {"interface base", ", Greeter", 0, 3},
    {"optional field", "Next:", 0, 1},
    {"generic type", "Array<", 0, 1},
    {"builtin type", "int>", 0, 1},
    {"engine fn", "GetForwardVector", 0, 1},
    {"engine enum", "CollisionChannel", 0, 1},
    {"engine case", "Camera", 0, 1},
    {"context name", "World.", 0, 1},
    {"discard param event", "Opened(by", 0, 1},
    {"struct decl", "Point {", 0, 1},
    {"interface decl", "interface Greeter", 0, 11},
    {"interface method", "Greet(): int;", 0, 1},
    {"class decl", "Door :", 0, 1},
    {"engine base", "ScriptableComponent", 0, 1},
    {"field decl", "Swing:", 0, 1},
    {"let field", "Label:", 0, 1},
    {"event decl", "Opened(by", 0, 1},
    {"event param", "by:", 0, 1},
    {"method decl", "Open(speed", 0, 1},
    {"param use", "speed +", 0, 1},
    {"local decl", "total =", 0, 1},
    {"local use", "total,", 0, 1},
    {"field use", "Swing;", 0, 1},
    {"prelude fn", "Delay(", 0, 1},
    {"prelude type", "Clock.", 0, 1},
    {"prelude case", "Game)", 0, 1},
    {"engine field", "Owner,", 0, 1},
    {"prelude value", "Log.", 0, 1},
    {"language fn", "Info(", 0, 1},
    {"imported fn", "G()", 0, 0},
    {"enum decl", "enum Mood", 0, 6},
    {"case decl", "Calm,", 0, 1},
    {"const decl", "Limit:", 0, 1},
    {"top fn", "Top(", 0, 1},
    {"type use", "Door)", 0, 1},
    {"member use", ".Swing *", 0, 2},
    {"const use", "Limit;", 0, 1},
    {"case use", "Calm;", 0, 1},
    {"name end", "Limit;", 0, 5},
    {"keyword", "return Mood", 0, 1},
    {"string", "\"door\"", 0, 2},
};

// hover·정의 이동 golden이다(검토한 출력 — 종류마다 선언 모양·문서 주석·추론 타입·선언 위치).
constexpr StringView LspHoverGolden = R"(== import
definition Game/Lib.spark 0:0-0:0
hover 0:7-0:10 markdown
```sparks
import Lib
```

Declared at `Game/Lib.spark:1`.
== import path
definition Game/Tools/Kit.spark 0:0-0:0
hover 1:7-1:12 markdown
```sparks
import Tools.Kit
```

Declared at `Game/Tools/Kit.spark:1`.
== import module
definition Game/Tools/Kit.spark 0:0-0:0
hover 1:13-1:16 markdown
```sparks
import Tools.Kit
```

Declared at `Game/Tools/Kit.spark:1`.
== alias decl
definition Game/Tools/Kit.spark 0:0-0:0
hover 1:20-1:23 markdown
```sparks
import Tools.Kit
```

Declared at `Game/Tools/Kit.spark:1`.
== alias use
definition Game/Tools/Kit.spark 0:0-0:0
hover 25:15-25:18 markdown
```sparks
import Tools.Kit
```

Declared at `Game/Tools/Kit.spark:1`.
== interface base
definition Game/Main.spark 38:10-38:17
hover 5:34-5:41 markdown
```sparks
interface Greeter
```

Declared at `Game/Main.spark:39`.
== optional field
definition Game/Main.spark 9:8-9:12
hover 9:8-9:12 markdown
```sparks
var Door.Next: Door?
```

Declared at `Game/Main.spark:10`.
== generic type
definition null
hover 10:15-10:20 markdown
```sparks
type Array<T>
```
== builtin type
definition null
hover 10:21-10:24 markdown
```sparks
type int
```
== engine fn
definition Engine.sparkbind.json 46:18-46:36
hover 18:18-18:34 markdown
```sparks
fn Rocks.Actor.GetForwardVector(): vec3
```

Declared in the engine schema at `Engine.sparkbind.json:47`.
== engine enum
definition Engine.sparkbind.json 475:14-475:32
hover 19:12-19:28 markdown
```sparks
enum Rocks.CollisionChannel
```

Declared in the engine schema at `Engine.sparkbind.json:476`.
== engine case
definition Engine.sparkbind.json 480:8-480:16
hover 19:29-19:35 markdown
```sparks
Rocks.CollisionChannel.Camera
```

Declared in the engine schema at `Engine.sparkbind.json:481`.
== context name
definition null
hover 20:12-20:17 markdown
```sparks
World: World
```
== discard param event
definition Game/Main.spark 11:10-11:16
hover 11:10-11:16 markdown
```sparks
event Door.Opened(by: Actor, _: int)
```

Declared at `Game/Main.spark:12`.
== struct decl
definition Game/Main.spark 34:7-34:12
hover 34:7-34:12 markdown
```sparks
struct Point
```

Declared at `Game/Main.spark:35`.
== interface decl
definition Game/Main.spark 38:10-38:17
hover 38:10-38:17 markdown
```sparks
interface Greeter
```

Declared at `Game/Main.spark:39`.
== interface method
definition Game/Main.spark 39:7-39:12
hover 39:7-39:12 markdown
```sparks
fn Greeter.Greet(): int
```

Declared at `Game/Main.spark:40`.
== class decl
definition Game/Main.spark 5:6-5:10
hover 5:6-5:10 markdown
```sparks
class Door : ScriptableComponent, Greeter
```

A door that opens.
Second line.

Declared at `Game/Main.spark:6`.
== engine base
definition Engine.sparkbind.json 212:14-212:35
hover 5:13-5:32 markdown
```sparks
class Rocks.ScriptableComponent : Rocks.Component
```

Declared in the engine schema at `Engine.sparkbind.json:213`.
== field decl
definition Game/Main.spark 7:8-7:13
hover 7:8-7:13 markdown
```sparks
var Door.Swing: float
```

How far it swings.

Declared at `Game/Main.spark:8`.
== let field
definition Game/Main.spark 8:8-8:13
hover 8:8-8:13 markdown
```sparks
let Door.Label: string
```

Declared at `Game/Main.spark:9`.
== event decl
definition Game/Main.spark 11:10-11:16
hover 11:10-11:16 markdown
```sparks
event Door.Opened(by: Actor, _: int)
```

Declared at `Game/Main.spark:12`.
== event param
definition Game/Main.spark 11:17-11:19
hover 11:17-11:19 markdown
```sparks
param by: Actor
```

Declared at `Game/Main.spark:12`.
== method decl
definition Game/Main.spark 13:13-13:17
hover 13:13-13:17 markdown
```sparks
async fn Door.Open(speed: float): int
```

Declared at `Game/Main.spark:14`.
== param use
definition Game/Main.spark 13:18-13:23
hover 14:20-14:25 markdown
```sparks
param speed: float
```

Declared at `Game/Main.spark:14`.
== local decl
definition Game/Main.spark 14:12-14:17
hover 14:12-14:17 markdown
```sparks
let total: float
```

Declared at `Game/Main.spark:15`.
== local use
definition Game/Main.spark 14:12-14:17
hover 15:20-15:25 markdown
```sparks
let total: float
```

Declared at `Game/Main.spark:15`.
== field use
definition Game/Main.spark 7:8-7:13
hover 14:28-14:33 markdown
```sparks
var Door.Swing: float
```

How far it swings.

Declared at `Game/Main.spark:8`.
== prelude fn
definition null
hover 15:14-15:19 markdown
```sparks
async fn Delay(float, Clock)
```
== prelude type
definition null
hover 15:27-15:32 markdown
```sparks
type Clock
```
== prelude case
definition null
hover 15:33-15:37 markdown
```sparks
Clock.Game
```
== engine field
definition Engine.sparkbind.json 184:18-184:25
hover 16:20-16:25 markdown
```sparks
let Rocks.Component.Owner: Actor
```

Declared in the engine schema at `Engine.sparkbind.json:185`.
== prelude value
definition null
hover 17:8-17:11 markdown
```sparks
Log
```
== language fn
definition null
hover 17:12-17:16 markdown
```sparks
fn Log.Info(string)
```
== imported fn
definition Game/Lib.spark 0:3-0:4
hover 21:15-21:16 markdown
```sparks
fn G(): int
```

Declared at `Game/Lib.spark:1`.
== enum decl
definition Game/Main.spark 42:5-42:9
hover 42:5-42:9 markdown
```sparks
enum Mood
```

Declared at `Game/Main.spark:43`.
== case decl
definition Game/Main.spark 43:4-43:8
hover 43:4-43:8 markdown
```sparks
Mood.Calm
```

Declared at `Game/Main.spark:44`.
== const decl
definition Game/Main.spark 47:6-47:11
hover 47:6-47:11 markdown
```sparks
const Limit: int
```

Declared at `Game/Main.spark:48`.
== top fn
definition Game/Main.spark 49:3-49:6
hover 49:3-49:6 markdown
```sparks
fn Top(p: int): int
```

Declared at `Game/Main.spark:50`.
== type use
definition Game/Main.spark 5:6-5:10
hover 28:15-28:19 markdown
```sparks
class Door : ScriptableComponent, Greeter
```

A door that opens.
Second line.

Declared at `Game/Main.spark:6`.
== member use
definition Game/Main.spark 7:8-7:13
hover 29:18-29:23 markdown
```sparks
var Door.Swing: float
```

How far it swings.

Declared at `Game/Main.spark:8`.
== const use
definition Game/Main.spark 47:6-47:11
hover 50:15-50:20 markdown
```sparks
const Limit: int
```

Declared at `Game/Main.spark:48`.
== case use
definition Game/Main.spark 43:4-43:8
hover 30:20-30:24 markdown
```sparks
Mood.Calm
```

Declared at `Game/Main.spark:44`.
== name end
definition Game/Main.spark 47:6-47:11
hover 50:15-50:20 markdown
```sparks
const Limit: int
```

Declared at `Game/Main.spark:48`.
== keyword
definition null
hover null
== string
definition null
hover null
)";

constexpr StringView LspQueryLib = "fn G(): int {\n    return 1;\n}\n";
constexpr StringView LspQueryKit = "fn K(): int {\n    return 2;\n}\n";

// hover 결과를 읽을 수 있는 글로: `null` 또는 range 줄 + value 원문.
void LspAppendHoverText(Allocator& allocator, StringView result, String& out) noexcept {
    if (result == "null" || result.IsEmpty()) {
        CHECK(out.TryAppend(result.IsEmpty() ? "(none)\n" : "null\n"));
        return;
    }

    JsonDocument document(allocator);
    REQUIRE(document.Parse(result).Succeeded());
    const JsonValue root = document.Root();
    const JsonValue range = root.Find("range");
    LspAppendPosition(out, range.Find("start"), allocator);
    CHECK(out.TryAppend("-"));
    LspAppendPosition(out, range.Find("end"), allocator);
    CHECK(out.TryAppend(" "));
    CHECK(out.TryAppend(root.Find("contents").Find("kind").GetString()));
    CHECK(out.TryAppend("\n"));
    CHECK(out.TryAppend(root.Find("contents").Find("value").GetString()));
    CHECK(out.TryAppend("\n"));
}

// 정의 결과를 `<uri 줄임> l:c-l:c` 또는 `null`로. 사례 뿌리 URI는 떼고 schema 파일은 이름만.
void LspAppendLocationText(Allocator& allocator, StringView result, StringView rootUri,
                           String& out) noexcept {
    if (result == "null" || result.IsEmpty()) {
        CHECK(out.TryAppend(result.IsEmpty() ? "(none)" : "null"));
        return;
    }

    JsonDocument document(allocator);
    REQUIRE(document.Parse(result).Succeeded());
    const JsonValue root = document.Root();
    StringView uri = root.Find("uri").GetString();
    if (uri.StartsWith(rootUri, StringComparison::CaseSensitive)) {
        uri = StringView(uri.Data() + rootUri.ByteCount() + 1,
                         uri.ByteCount() - rootUri.ByteCount() - 1);
    } else {
        usize slash = uri.ByteCount();
        while (slash > 0 && uri.Data()[slash - 1] != '/') {
            --slash;
        }

        uri = StringView(uri.Data() + slash, uri.ByteCount() - slash);
    }

    CHECK(out.TryAppend(uri));
    CHECK(out.TryAppend(" "));
    const JsonValue range = root.Find("range");
    LspAppendPosition(out, range.Find("start"), allocator);
    CHECK(out.TryAppend("-"));
    LspAppendPosition(out, range.Find("end"), allocator);
}

} // namespace

TEST(SparksLsp_HoverAndDefinitionGolden) {
    SystemAllocator allocator;
    {
        LspFolder folder(allocator);
        REQUIRE(folder.Write("Game/SparksPackage.json5", LspTestFullManifest));
        REQUIRE(folder.Write("Game/Lib.spark", LspQueryLib));
        REQUIRE(folder.Write("Game/Tools/Kit.spark", LspQueryKit));
        const String schema = LspSchemaFolder(allocator);
        const String root = folder.UriOf("");
        const String main = folder.UriOf("Game/Main.spark");
        LspScript script(allocator);
        script.Initialize(1, root.View()).Open(main.View(), 1, LspQuerySample);
        int64 id = 100;
        for (const LspProbe& probe : LspQueryProbes) {
            const LspTestPlace place =
                LspPlaceOf(LspQuerySample, probe.needle, probe.occurrence, probe.shift);
            script.At(id, "textDocument/hover", main.View(), place.line, place.character);
            script.At(id + 1, "textDocument/definition", main.View(), place.line, place.character);
            id += 2;
        }

        script.End(2);
        const StringView arguments[] = {"--schema", schema.View()};
        const LspRun run = LspRunScript(allocator, arguments, script.text.View());
        CHECK(run.code == LspTestClean);
        LspExpectText(LspPublishText(allocator, run.output.View(), root.View()).View(), "",
                      "query sample is clean");
        String text(allocator);
        id = 100;
        for (const LspProbe& probe : LspQueryProbes) {
            CHECK(text.TryAppend("== "));
            CHECK(text.TryAppend(probe.label));
            CHECK(text.TryAppend("\ndefinition "));
            const String definition = LspResultText(allocator, run.output.View(), id + 1);
            LspAppendLocationText(allocator, definition.View(), root.View(), text);
            CHECK(text.TryAppend("\nhover "));
            const String hover = LspResultText(allocator, run.output.View(), id);
            LspAppendHoverText(allocator, hover.View(), text);
            id += 2;
        }

        LspExpectText(text.View(), LspHoverGolden, "hover golden");
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SparksLsp_Formatting) {
    // 서식(11.5절): 바뀐 문서는 전체 교체 편집 하나, 같으면 [], 구문 오류는 [], CRLF는 LF로, 열지
    // 않은 문서·manifest는 null.
    SystemAllocator allocator;
    {
        LspFolder folder(allocator);
        REQUIRE(folder.Write("Game/SparksPackage.json5", LspTestCoreManifest));
        const String root = folder.UriOf("");
        const String messy = folder.UriOf("Game/Messy.spark");
        const String tidy = folder.UriOf("Game/Tidy.spark");
        const String broken = folder.UriOf("Game/Broken.spark");
        const String crlf = folder.UriOf("Game/Crlf.spark");
        const String closed = folder.UriOf("Game/Closed.spark");
        const String manifest = folder.UriOf("Game/SparksPackage.json5");
        const String bom = folder.UriOf("Game/Bom.spark");
        constexpr StringView Tidy = "fn F(): int {\n    return 1;\n}\n";
        LspScript script(allocator);
        script.Initialize(1, root.View())
            .Open(messy.View(), 1, "fn  F( ): int {\nreturn 1;}\n")
            .Open(tidy.View(), 1, Tidy)
            .Open(broken.View(), 1, "fn F( {\n")
            .Open(crlf.View(), 1, "fn F(): int {\r\n    return 1;\r\n}\r\n")
            .Open(manifest.View(), 1, LspTestCoreManifest)
            .Format(10, messy.View())
            .Format(11, tidy.View())
            .Format(12, broken.View())
            .Format(13, crlf.View())
            .Format(14, closed.View())
            .Format(15, manifest.View())
            .Open(bom.View(), 1,
                  "\xEF\xBB\xBF"
                  "fn F(): int {\n    return 1;\n}\n")
            .Format(16, bom.View())
            .End(2);
        const LspRun run = LspRunScript(allocator, {}, script.text.View());
        CHECK(run.code == LspTestClean);
        const StringView expected[] = {
            "[{\"range\":{\"start\":{\"line\":0,\"character\":0},\"end\":{\"line\":2,"
            "\"character\":0}},\"newText\":\"fn F(): int {\\n    return 1;\\n}\\n\"}]",
            "[]",
            "[]",
            "[{\"range\":{\"start\":{\"line\":0,\"character\":0},\"end\":{\"line\":3,"
            "\"character\":0}},\"newText\":\"fn F(): int {\\n    return 1;\\n}\\n\"}]",
            "null",
            "null",
            "[]", // BOM 뒤 내용이 이미 서식대로
        };
        for (int64 at = 0; at < 7; ++at) {
            const String result = LspResultText(allocator, run.output.View(), 10 + at);
            LspExpectText(result.View(), expected[at], "formatting");
        }
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SparksLsp_PositionsToOffsets) {
    // LSP 위치 → byte 위치(11.2절): BOM 뒤 1줄, CRLF, 이모지 뒤 열, 서로게이트 가운데, 줄 끝 넘음,
    // 마지막 줄 넘음, 이름 끝 자리.
    SystemAllocator allocator;
    {
        LspFolder folder(allocator);
        REQUIRE(folder.Write("Game/SparksPackage.json5", LspTestCoreManifest));
        const String root = folder.UriOf("");
        const String main = folder.UriOf("Game/Main.spark");
        String text(allocator);
        CHECK(text.TryAppend("\xEF\xBB\xBF"
                             "fn F(): int {\r\n    let s = \""));
        CHECK(text.TryAppend(LspTestEmoji));
        CHECK(text.TryAppend(
            "\"; let t = s\r\n    ;\r\n    let a = 1;\rlet b = a;\r\n    return b;\r\n}\r\n"));
        struct Case {
            LspTestPlace place;
            StringView expected;
        };

        const Case cases[] = {
            {{0, 3}, "Game/Main.spark 0:3-0:4"},   // BOM 뒤 `F`
            {{0, 0}, "null"},                      // `fn` 키워드
            {{1, 26}, "Game/Main.spark 1:8-1:9"},  // 이모지(2) 뒤 `s`
            {{1, 27}, "Game/Main.spark 1:8-1:9"},  // `s` 끝 자리
            {{1, 14}, "null"},                     // 서로게이트 가운데 — 문자열 안
            {{1, 200}, "Game/Main.spark 1:8-1:9"}, // 줄 끝 넘음 — CR 앞(`s` 끝 자리)
            {{2, 200}, "null"},                    // CRLF 줄 끝 — `;` 뒤
            {{3, 23}, "Game/Main.spark 3:8-3:9"},  // 외톨이 CR은 줄 안의 글자(1)
            {{99, 0}, "null"},                     // 마지막 줄 넘음
        };
        LspScript script(allocator);
        script.Initialize(1, root.View()).Open(main.View(), 1, text.View());
        int64 id = 10;
        for (const Case& entry : cases) {
            script.At(id++, "textDocument/definition", main.View(), entry.place.line,
                      entry.place.character);
        }

        script.End(2);
        const LspRun run = LspRunScript(allocator, {}, script.text.View());
        CHECK(run.code == LspTestClean);
        id = 10;
        for (const Case& entry : cases) {
            String location(allocator);
            const String result = LspResultText(allocator, run.output.View(), id++);
            LspAppendLocationText(allocator, result.View(), root.View(), location);
            LspExpectText(location.View(), entry.expected, "position to offset");
        }

        // 위치 인자 틀림 — -32602.
        LspScript bad(allocator);
        bad.Initialize(1, root.View())
            .Call(3, "textDocument/hover", "{\"textDocument\":{\"uri\":\"file:///x\"}}")
            .Call(4, "textDocument/definition",
                  "{\"textDocument\":{\"uri\":\"file:///x\"},\"position\":{\"line\":-1,"
                  "\"character\":0}}")
            .Call(5, "textDocument/hover",
                  "{\"textDocument\":{\"uri\":\"file:///x\"},\"position\":{\"line\":"
                  "4294967296,\"character\":0}}")
            .Call(6, "textDocument/formatting", "{}")
            .Call(7, "textDocument/definition",
                  "{\"textDocument\":{\"uri\":\"file:///x\"},\"position\":{\"line\":1.5,"
                  "\"character\":0}}")
            .End(2);
        const LspRun refused = LspRunScript(allocator, {}, bad.text.View());
        for (int64 at = 3; at <= 7; ++at) {
            StringBuilder<64> needle(allocator);
            needle.AppendFormat("\"id\":{},\"error\":{{\"code\":-32602", at);
            CHECK(refused.output.View().Contains(needle.View(), StringComparison::CaseSensitive));
        }
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SparksLsp_QueryAllocationFailure) {
    // hover·정의 이동(사용자·엔진·import)·서식 세션의 모든 할당 실패 — 3이거나 같은 출력.
    SystemAllocator backing;
    LspFolder folder(backing);
    REQUIRE(folder.Write("Game/SparksPackage.json5", LspTestFullManifest));
    REQUIRE(folder.Write("Game/Lib.spark", LspQueryLib));
    REQUIRE(folder.Write("Game/Tools/Kit.spark", LspQueryKit));
    const String schema = LspSchemaFolder(backing);
    const String root = folder.UriOf("");
    const String main = folder.UriOf("Game/Main.spark");
    LspScript script(backing);
    script.Initialize(1, root.View()).Open(main.View(), 1, LspQuerySample);
    const StringView probes[] = {"Lib;",   "Door :",           "Owner,", "Swing;",
                                 "total,", "GetForwardVector", "Camera", "Pick("};
    int64 id = 10;
    for (const StringView needle : probes) {
        const LspTestPlace place = LspPlaceOf(LspQuerySample, needle, 0, 1);
        script.At(id++, "textDocument/hover", main.View(), place.line, place.character);
        script.At(id++, "textDocument/definition", main.View(), place.line, place.character);
    }

    script.Format(id, main.View()).End(2);
    const StringView arguments[] = {"--schema", schema.View()};
    LspSweepAllocations(backing, arguments, script.text.View(), LspTestClean);
}

} // namespace sparks_tools_lsp_query_tests_file
