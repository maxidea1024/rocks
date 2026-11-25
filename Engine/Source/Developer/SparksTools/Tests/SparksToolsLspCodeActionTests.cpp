#include "SparksToolsLspTestSupport.h"

// 언어 서버 빠른 수정 시험이다(Docs/Design/SparksTools.ko.md 11.7절·11.8절 — LC8f3): 코드마다 편집
// golden(title·범위·새 글자), 편집을 적용한 원문을 다시 분석하면 그 코드 진단이 없다, CRLF 문서,
// 범위 밖 진단·`context.only`·열지 않은 문서·위치 인자, 할당 실패.
using namespace rocks;
namespace sparks_tools_lsp_code_action_tests_file {

using namespace rocks::test::sparks_lsp;

namespace {

constexpr StringView LspFixGameManifest =
    "{ name: \"Game\", sparks: 1, profile: \"full\", dependencies: [\"Shared\"] }\n";
constexpr StringView LspFixSharedManifest = "{ name: \"Shared\", sparks: 1, profile: \"full\" }\n";
constexpr StringView LspFixLib = "fn G(): int {\n    return 1;\n}\n";
constexpr StringView LspFixKit = "struct Tool {\n    var A: int = 0;\n}\n\nfn Beep(): int {\n    "
                                 "return 1;\n}\n\nfn Clash(): int {\n    return 2;\n}\n";
constexpr StringView LspFixSignals = "event Actor.Rang();\n\nfn Beep(): int {\n    return "
                                     "3;\n}\n\nstruct Gear {\n    var B: int = 0;\n}\n";
// 의존하지 않는 패키지의 확장 이벤트(import할 수 없다 — S0302).
constexpr StringView LspFixOtherManifest = "{ name: \"Other\", sparks: 1, profile: \"full\" }\n";
constexpr StringView LspFixHum = "event Actor.Hum();\n";
constexpr StringView LspFixBeat = "fn Beat(): int {\n    return 1;\n}\n";

// 사례 하나: Game/Main.spark 원문(ASCII), 요청 자리(needle의 occurrence번째 시작), 대상 코드.
struct LspFixCase {
    StringView label;
    StringView text;
    StringView needle;
    StringView code;
    uint32 occurrence = 0;
};

constexpr LspFixCase LspFixCases[] = {
    {"Q1 object field without '?'", "class Door : ScriptableComponent {\n    var Next: Door;\n}\n",
     "Door;", "S0428"},
    {"Q2 value that may be null",
     "class Door : ScriptableComponent {\n    var Next: Door?;\n\n    fn Close() {\n    }\n\n"
     "    on Start() {\n        let other = Next;\n        other.Close();\n    }\n}\n",
     "other.Close", "S0412"},
    {"Q3 unknown name (same package)", "fn Top(): int {\n    return G();\n}\n", "G()", "S0317"},
    {"Q3 unknown name (dependency)", "fn Top(): int {\n    return Beat();\n}\n", "Beat()", "S0317"},
    {"Q3 unknown type after an import",
     "import Lib;\n\nfn Top(t: Tool): int {\n    return G() + t.A;\n}\n", "Tool)", "S0318"},
    {"Q4 extension event not imported",
     "class Bell : Character {\n    on this.Rang() {\n    }\n}\n", "Rang", "S0815"},
    {"Q5 missing reentrancy policy",
     "class Door : ScriptableComponent {\n    on Owner.Damaged() {\n        await Delay(1.0, "
     "Clock.Game);\n    }\n}\n",
     "Damaged", "S0816"},
    {"Q6 misplaced reentrancy policy",
     "class Door : ScriptableComponent {\n    @skipWhileBusy\n    on Owner.Damaged() {\n    }\n}\n",
     "@skip", "S0817"},
    {"Q6 duplicate attribute",
     "class Door : ScriptableComponent {\n    @edit @edit var Speed: float = 1.0;\n}\n", "@edit",
     "S0622", 1},
    {"Q7 unused import", "import Lib;\n\nfn Top(): int {\n    return 1;\n}\n", "Lib", "S0714"},
    {"Q7 duplicate import", "import Lib;\nimport Lib;\n\nfn Top(): int {\n    return G();\n}\n",
     "Lib", "S0303", 1},
    {"Q7 self import", "import Main;\n\nfn Top(): int {\n    return 1;\n}\n", "Main", "S0304"},
    {"Q7 Rocks import", "import Rocks.Engine;\n\nfn Top(): int {\n    return 1;\n}\n", "Rocks",
     "S0305"},
    {"Q8 discarded result",
     "class Door : ScriptableComponent {\n    fn Look() {\n        Owner.GetForwardVector();\n    "
     "}\n}\n",
     "Owner.Get", "S0713"},
    {"Q9 await outside async",
     "class Door : ScriptableComponent {\n    fn Wait() {\n        await Delay(1.0, "
     "Clock.Game);\n    }\n}\n",
     "await", "S0601"},
    {"Q9 top-level async", "async fn Top() {\n}\n", "Top", "S0601"},
    {"Q10 unknown hook", "class Door : ScriptableComponent {\n    on Strat() {\n    }\n}\n",
     "Strat", "S0803"},
    {"Q10 unknown attribute",
     "class Door : ScriptableComponent {\n    @sav var Score: int = 0;\n}\n", "@sav", "S0620"},
    {"Q7 unused import (CRLF)", "import Lib;\r\n\r\nfn Top(): int {\r\n    return 1;\r\n}\r\n",
     "Lib", "S0714"},
    {"Q1 LevelScript object field", "class Lv : LevelScript {\n    @edit var Target: Actor;\n}\n",
     "Actor;", "S0413"},
    {"Q2 no '?? return' in a function with a value",
     "class Door : ScriptableComponent {\n    var Next: Door?;\n    var Swing: float = 1.0;\n\n"
     "    fn Speed(): float {\n        let other = Next;\n        return other.Swing;\n    }\n}\n",
     "other.Swing", "S0412"},
    {"Q2 'var' local (only '?.')",
     "class Door : ScriptableComponent {\n    var Next: Door?;\n\n    fn Close() {\n    }\n\n"
     "    on Start() {\n        var other = Next;\n        other.Close();\n    }\n}\n",
     "other.Close", "S0412"},
    {"Q2 typed local (only '?.')",
     "class Door : ScriptableComponent {\n    var Next: Door?;\n\n    fn Close() {\n    }\n\n"
     "    on Start() {\n        let other: Door? = Next;\n        other.Close();\n    }\n}\n",
     "other.Close", "S0412"},
    {"Q2 use in a nested block",
     "class Door : ScriptableComponent {\n    var Next: Door?;\n    var Swing: float = 1.0;\n\n"
     "    fn Close() {\n    }\n\n    on Start() {\n        let other = Next;\n        if (Swing > "
     "1.0) {\n            other.Close();\n        }\n    }\n}\n",
     "other.Close", "S0412"},
    {"Q2 assignment target (no '?.')",
     "class Door : ScriptableComponent {\n    var Target: Actor?;\n\n    on Start() {\n"
     "        let other = Target;\n        other.Location = Owner.Location;\n    }\n}\n",
     "other.Location", "S0412"},
    {"Q2 another use handles null (only '?.')",
     "class Door : ScriptableComponent {\n    var Next: Door?;\n\n    fn Close() {\n    }\n\n"
     "    on Start() {\n        let other = Next;\n        other.Close();\n        "
     "other?.Close();\n"
     "    }\n}\n",
     "other.Close", "S0412"},
    {"Q2 read after 'await' (only '?.')",
     "class Door : ScriptableComponent {\n    var Next: Door?;\n\n    fn Close() {\n    }\n\n"
     "    @skipWhileBusy\n    on Owner.Damaged() {\n        let other = Next ?? return;\n"
     "        await Delay(1.0, Clock.Game);\n        other.Close();\n    }\n}\n",
     "other.Close", "S0412"},
    {"Q3 import into a file with a BOM",
     "\xEF\xBB\xBF"
     "fn Top(): int {\n    return G();\n}\n",
     "G()", "S0317"},
    {"Q3 import after an import (CRLF)",
     "import Lib;\r\n\r\nfn Top(t: Tool): int {\r\n    return G() + t.A;\r\n}\r\n", "Tool)",
     "S0318"},
    {"Q3 import after an alias import",
     "import Tools.Kit as K;\n\nfn Top(): int {\n    return K.Beep() + G();\n}\n", "G()", "S0317"},
    {"Q3 import after a line comment",
     "import Lib; // the library\n\nfn Top(t: Tool): int {\n    return G() + t.A;\n}\n", "Tool)",
     "S0318"},
    {"Q3 import before code on the same line",
     "import Lib; fn Top(t: Tool): int {\n    return G() + t.A;\n}\n", "Tool)", "S0318"},
    {"Q3 import before a block comment",
     "import Lib; /* note\n */\nfn Top(t: Tool): int {\n    return G() + t.A;\n}\n", "Tool)",
     "S0318"},
    {"Q3 several modules", "fn Top(): int {\n    return Beep();\n}\n", "Beep()", "S0317"},
    {"Q3 module whose names clash", "fn Clash(): int {\n    return Beep();\n}\n", "Beep()",
     "S0317"},
    {"Q3 alias-qualified type (no fix)",
     "import Tools.Kit as K;\n\nfn Top(t: K.Gear): int {\n    return K.Beep();\n}\n", "Gear",
     "S0318"},
    {"Q4 event of another target (no fix)",
     "class Bell : Character {\n    on World.Rang() {\n    }\n}\n", "Rang", "S0808"},
    {"Q4 package that is not a dependency (no fix)",
     "class Bell : Character {\n    on this.Hum() {\n    }\n}\n", "Hum", "S0815"},
    {"Q6 tab-indented attribute line",
     "class Door : ScriptableComponent {\n\t@skipWhileBusy\n\ton Owner.Damaged() {\n\t}\n}\n",
     "@skip", "S0817"},
    {"Q6 attribute on the declaration line",
     "class Door : ScriptableComponent {\n    @skipWhileBusy on Owner.Damaged() {\n    }\n}\n",
     "@skip", "S0817"},
    {"Q7 import on the last line without a line end", "import Lib;", "Lib", "S0714"},
    {"Q7 duplicate import with an alias (no fix)",
     "import Lib;\nimport Lib as L;\n\nfn Top(): int {\n    return L.G();\n}\n", "Lib", "S0303", 1},
    {"Q9 override keeps its signature (no fix)",
     "open class Panel : ScriptableComponent {\n    open fn Act() {\n    }\n}\n\nclass Gate : "
     "Panel "
     "{\n    override fn Act() {\n        await Delay(1.0, Clock.Game);\n    }\n}\n",
     "await", "S0601"},
    {"Q9 top-level async that waits (no fix)",
     "async fn Top() {\n    await Delay(1.0, Clock.Game);\n}\n", "Top", "S0601"},
    {"Q9 '@test' function", "@test\nfn Check() {\n    await Delay(1.0, Clock.Game);\n}\n", "await",
     "S0601"},
    {"Q10 own event used as a hook (no fix)",
     "class Door : ScriptableComponent {\n    event Strat();\n\n    on Strat() {\n    }\n}\n",
     "Strat", "S0803", 1},
    {"Q10 hook with parameters (no fix)",
     "class Door : ScriptableComponent {\n    on Tik(dt: float) {\n    }\n}\n", "Tik", "S0803"},
    {"Q10 hook already declared (no fix)",
     "class Door : ScriptableComponent {\n    on Start() {\n    }\n\n    on Strat() {\n    }\n}\n",
     "Strat", "S0803"},
    {"Q8 UTF-16 columns after Hangul",
     "class Door : ScriptableComponent {\n    fn Look() {\n        Log.Info(\"\xED\x95\x9C\xEA\xB8"
     "\x80\"); Owner.GetForwardVector();\n    }\n}\n",
     "Owner.Get", "S0713"},
    {"Q5 missing reentrancy policy (CRLF)",
     "class Door : ScriptableComponent {\r\n    on Owner.Damaged() {\r\n        await Delay(1.0, "
     "Clock.Game);\r\n    }\r\n}\r\n",
     "Damaged", "S0816"},
};

// 시험 작업 공간: Game(Lib·Tools/Kit·Tools/Signals — Shared에 의존), Shared(Util/Beat), Other(Hum —
// Game이 의존하지 않는다).
struct LspFixWorkspace {
    explicit LspFixWorkspace(Allocator& allocator) noexcept
        : folder(allocator), schema(LspSchemaFolder(allocator)), root(folder.UriOf("")),
          main(folder.UriOf("Game/Main.spark")) {
        CHECK(folder.Write("Game/SparksPackage.json5", LspFixGameManifest));
        CHECK(folder.Write("Game/Lib.spark", LspFixLib));
        CHECK(folder.Write("Game/Tools/Kit.spark", LspFixKit));
        CHECK(folder.Write("Game/Tools/Signals.spark", LspFixSignals));
        CHECK(folder.Write("Shared/SparksPackage.json5", LspFixSharedManifest));
        CHECK(folder.Write("Shared/Util/Beat.spark", LspFixBeat));
        CHECK(folder.Write("Other/SparksPackage.json5", LspFixOtherManifest));
        CHECK(folder.Write("Other/Hum.spark", LspFixHum));
    }

    LspFolder folder;
    String schema;
    String root;
    String main;
};

// UTF-8 글자 하나의 byte 수와 UTF-16 code unit 수다(시험 원문은 올바른 UTF-8).
void LspFixCharacter(StringView text, usize at, usize& bytes, uint32& units) noexcept {
    const uint8 lead = static_cast<uint8>(text.Data()[at]);
    bytes = lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
    units = bytes == 4 ? 2 : 1;
}

usize LspFixContentBegin(StringView text) noexcept {
    return text.StartsWith("\xEF\xBB\xBF", StringComparison::CaseSensitive) ? 3 : 0;
}

// LSP 위치 → byte 위치다(1줄은 BOM 뒤, 열은 UTF-16, CRLF의 CR은 줄 끝).
usize LspFixOffsetOf(StringView text, uint32 line, uint32 character) noexcept {
    usize at = LspFixContentBegin(text);
    for (uint32 count = 0; count < line && at < text.ByteCount(); ++at) {
        count += text.Data()[at] == '\n' ? 1 : 0;
    }

    for (uint32 column = 0; column < character && at < text.ByteCount();) {
        usize bytes = 1;
        uint32 units = 1;
        LspFixCharacter(text, at, bytes, units);
        at += bytes;
        column += units;
    }

    return at;
}

// needle(occurrence번째) 자리의 LSP 위치다.
LspTestPlace LspFixPlaceOf(StringView text, StringView needle, uint32 occurrence) noexcept {
    usize from = 0;
    usize found = StringView::NotFound;
    for (uint32 count = 0; count <= occurrence; ++count) {
        found = text.Find(needle, StringComparison::CaseSensitive, StringSearchDirection::FromStart,
                          from);
        CHECK(found != StringView::NotFound);
        if (found == StringView::NotFound) {
            return LspTestPlace{};
        }

        from = found + 1;
    }

    LspTestPlace place;
    for (usize at = LspFixContentBegin(text); at < found;) {
        usize bytes = 1;
        uint32 units = 1;
        LspFixCharacter(text, at, bytes, units);
        if (text.Data()[at] == '\n') {
            ++place.line;
            place.character = 0;
        } else if (text.Data()[at] != '\r') {
            place.character += units;
        }

        at += bytes;
    }

    return place;
}

// 각본 출력에서 uri의 마지막 발행 진단 코드들이다(공백으로 이음 — 발행이 없으면 빈 글자).
String LspFixCodesOf(Allocator& allocator, StringView output, StringView uri) noexcept {
    Array<String> bodies(allocator);
    CHECK(LspBodies(output, bodies, allocator));
    String codes(allocator);
    for (const String& body : bodies) {
        JsonDocument document(allocator);
        if (!document.Parse(body.View()).Succeeded()) {
            CHECK(false);
            continue;
        }

        const JsonValue root = document.Root();
        const JsonValue method = root.Find("method");
        if (!method.IsValid() || method.GetString() != "textDocument/publishDiagnostics" ||
            root.Find("params").Find("uri").GetString() != uri) {
            continue;
        }

        codes.Clear();
        const JsonValue diagnostics = root.Find("params").Find("diagnostics");
        for (usize at = 0; at < diagnostics.Size(); ++at) {
            CHECK(codes.TryAppend(at == 0 ? "" : " "));
            CHECK(codes.TryAppend(diagnostics.At(at).Find("code").GetString()));
        }
    }

    return codes;
}

// 글자의 CR·LF를 `\r`·`\n`으로 보인다.
void LspFixAppendEscaped(String& out, StringView text) noexcept {
    for (const char value : text) {
        if (value == '\n') {
            CHECK(out.TryAppend("\\n"));
        } else if (value == '\r') {
            CHECK(out.TryAppend("\\r"));
        } else {
            CHECK(out.TryAppend(StringView(&value, 1)));
        }
    }
}

constexpr StringView LspFixGolden = R"(== Q1 object field without '?'
Add '?' to the type | 1:18-1:18 | "?" | preferred
  after: clean
== Q2 value that may be null
Add '?? return' | 7:24-7:24 | " ?? return"
  after: clean
Use '?.' | 8:13-8:14 | "?."
  after: clean
== Q3 unknown name (same package)
Add 'import Lib;' | 0:0-0:0 | "import Lib;\n\n" | preferred
  after: clean
== Q3 unknown name (dependency)
Add 'import Shared.Util.Beat;' | 0:0-0:0 | "import Shared.Util.Beat;\n\n" | preferred
  after: clean
== Q3 unknown type after an import
Add 'import Tools.Kit;' | 1:0-1:0 | "import Tools.Kit;\n" | preferred
  after: clean
== Q4 extension event not imported
Add 'import Tools.Signals;' | 0:0-0:0 | "import Tools.Signals;\n\n" | preferred
  after: clean
== Q5 missing reentrancy policy
Add '@skipWhileBusy' | 1:4-1:4 | "@skipWhileBusy\n    "
  after: clean
Add '@restart' | 1:4-1:4 | "@restart\n    "
  after: clean
== Q6 misplaced reentrancy policy
Remove the attribute | 1:0-2:0 | "" | preferred
  after: clean
== Q6 duplicate attribute
Remove the attribute | 1:10-1:16 | "" | preferred
  after: clean
== Q7 unused import
Remove the import | 0:0-1:0 | "" | preferred
  after: clean
== Q7 duplicate import
Remove the import | 1:0-2:0 | "" | preferred
  after: clean
== Q7 self import
Remove the import | 0:0-1:0 | "" | preferred
  after: clean
== Q7 Rocks import
Remove the import | 0:0-1:0 | "" | preferred
  after: clean
== Q8 discarded result
Discard the result with '_ =' | 2:8-2:8 | "_ = " | preferred
  after: clean
== Q9 await outside async
Declare the function 'async' | 1:4-1:4 | "async " | preferred
  after: clean
== Q9 top-level async
Remove 'async' | 0:0-0:6 | "" | preferred
  after: clean
== Q10 unknown hook
Change to 'Start' | 1:7-1:12 | "Start" | preferred
  after: clean
== Q10 unknown attribute
Change to '@save' | 1:5-1:8 | "save" | preferred
  after: clean
== Q7 unused import (CRLF)
Remove the import | 0:0-1:0 | "" | preferred
  after: clean
== Q1 LevelScript object field
Add '?' to the type | 1:27-1:27 | "?" | preferred
  after: clean
== Q2 no '?? return' in a function with a value
(no fixes)
== Q2 'var' local (only '?.')
Use '?.' | 8:13-8:14 | "?." | preferred
  after: clean
== Q2 typed local (only '?.')
Use '?.' | 8:13-8:14 | "?." | preferred
  after: clean
== Q2 use in a nested block
Add '?? return' | 8:24-8:24 | " ?? return"
  after: clean
Use '?.' | 10:17-10:18 | "?."
  after: clean
== Q2 assignment target (no '?.')
Add '?? return' | 4:26-4:26 | " ?? return" | preferred
  after: clean
== Q2 another use handles null (only '?.')
Use '?.' | 8:13-8:14 | "?." | preferred
  after: clean
== Q2 read after 'await' (only '?.')
Use '?.' | 10:13-10:14 | "?." | preferred
  after: clean
== Q3 import into a file with a BOM
Add 'import Lib;' | 0:0-0:0 | "import Lib;\n\n" | preferred
  after: clean
== Q3 import after an import (CRLF)
Add 'import Tools.Kit;' | 1:0-1:0 | "import Tools.Kit;\r\n" | preferred
  after: clean
== Q3 import after an alias import
Add 'import Lib;' | 1:0-1:0 | "import Lib;\n" | preferred
  after: clean
== Q3 import after a line comment
Add 'import Tools.Kit;' | 1:0-1:0 | "import Tools.Kit;\n" | preferred
  after: clean
== Q3 import before code on the same line
Add 'import Tools.Kit;' | 0:11-0:11 | "\nimport Tools.Kit;" | preferred
  after: clean
== Q3 import before a block comment
Add 'import Tools.Kit;' | 0:11-0:11 | "\nimport Tools.Kit;" | preferred
  after: clean
== Q3 several modules
Add 'import Tools.Kit;' | 0:0-0:0 | "import Tools.Kit;\n\n"
  after: clean
Add 'import Tools.Signals;' | 0:0-0:0 | "import Tools.Signals;\n\n"
  after: clean
== Q3 module whose names clash
Add 'import Tools.Signals;' | 0:0-0:0 | "import Tools.Signals;\n\n" | preferred
  after: clean
== Q3 alias-qualified type (no fix)
(no fixes)
== Q4 event of another target (no fix)
(no fixes)
== Q4 package that is not a dependency (no fix)
(no fixes)
== Q6 tab-indented attribute line
Remove the attribute | 1:0-2:0 | "" | preferred
  after: clean
== Q6 attribute on the declaration line
Remove the attribute | 1:4-1:19 | "" | preferred
  after: clean
== Q7 import on the last line without a line end
Remove the import | 0:0-0:11 | "" | preferred
  after: clean
== Q7 duplicate import with an alias (no fix)
(no fixes)
== Q9 override keeps its signature (no fix)
(no fixes)
== Q9 top-level async that waits (no fix)
(no fixes)
== Q9 '@test' function
Declare the function 'async' | 1:0-1:0 | "async " | preferred
  after: clean
== Q10 own event used as a hook (no fix)
(no fixes)
== Q10 hook with parameters (no fix)
(no fixes)
== Q10 hook already declared (no fix)
(no fixes)
== Q8 UTF-16 columns after Hangul
Discard the result with '_ =' | 2:24-2:24 | "_ = " | preferred
  after: clean
== Q5 missing reentrancy policy (CRLF)
Add '@skipWhileBusy' | 1:4-1:4 | "@skipWhileBusy\r\n    "
  after: clean
Add '@restart' | 1:4-1:4 | "@restart\r\n    "
  after: clean
)";

} // namespace

TEST(SparksLsp_CodeActionGolden) {
    SystemAllocator allocator;
    {
        LspFixWorkspace workspace(allocator);
        const StringView arguments[] = {"--schema", workspace.schema.View()};
        // 1회차: 사례마다 원문을 열고 그 자리의 빠른 수정을 묻는다.
        LspScript script(allocator);
        script.Initialize(1, workspace.root.View());
        int64 id = 100;
        int64 version = 1;
        for (const LspFixCase& entry : LspFixCases) {
            if (version == 1) {
                script.Open(workspace.main.View(), version++, entry.text);
            } else {
                script.Change(workspace.main.View(), version++, entry.text);
            }

            const LspTestPlace place = LspFixPlaceOf(entry.text, entry.needle, entry.occurrence);
            String params(allocator);
            StringBuilder<192> range(allocator);
            range.AppendFormat(
                "\"range\":{{\"start\":{{\"line\":{},\"character\":{}}},\"end\":{{"
                "\"line\":{},\"character\":{}}}}},\"context\":{{\"diagnostics\":[]}}}}",
                place.line, place.character, place.line, place.character);
            CHECK(!range.HasFailed());
            CHECK(params.TryAppend("{\"textDocument\":{\"uri\":\""));
            CHECK(params.TryAppend(workspace.main.View()));
            CHECK(params.TryAppend("\"},"));
            CHECK(params.TryAppend(range.View()));
            script.Call(id++, "textDocument/codeAction", params.View());
        }

        script.End(2);
        const LspRun run = LspRunScript(allocator, arguments, script.text.View());
        CHECK(run.code == LspTestClean);

        // 2회차: 사례 원문과 수정마다 적용한 원문을 새 서버로 열어 진단 코드를 본다 — 대상 코드는
        // 없어지고 다른 코드는 늘지 않는다.
        auto codesOf = [&](StringView source) noexcept {
            LspScript check(allocator);
            check.Initialize(1, workspace.root.View())
                .Open(workspace.main.View(), 1, source)
                .End(2);
            const LspRun after = LspRunScript(allocator, arguments, check.text.View());
            CHECK(after.code == LspTestClean);
            return LspFixCodesOf(allocator, after.output.View(), workspace.main.View());
        };
        auto countOf = [](StringView codes, StringView code) noexcept {
            usize count = 0;
            for (usize at = 0; at + code.ByteCount() <= codes.ByteCount(); ++at) {
                count += StringView(codes.Data() + at, code.ByteCount()) == code ? 1 : 0;
            }

            return count;
        };

        String text(allocator);
        id = 100;
        for (const LspFixCase& entry : LspFixCases) {
            CHECK(text.TryAppend("== "));
            CHECK(text.TryAppend(entry.label));
            CHECK(text.TryAppend("\n"));
            const String before = codesOf(entry.text);
            CHECK(before.View().Contains(entry.code, StringComparison::CaseSensitive));
            const String result = LspResultText(allocator, run.output.View(), id++);
            JsonDocument document(allocator);
            REQUIRE(document.Parse(result.View()).Succeeded());
            const JsonValue actions = document.Root();
            if (actions.Size() == 0) {
                CHECK(text.TryAppend("(no fixes)\n"));
            }

            for (usize at = 0; at < actions.Size(); ++at) {
                const JsonValue action = actions.At(at);
                CHECK(action.Find("kind").GetString() == "quickfix");
                CHECK(action.Find("diagnostics").At(0).Find("code").GetString() == entry.code);
                const JsonValue edits =
                    action.Find("edit").Find("changes").Find(workspace.main.View());
                REQUIRE(edits.IsValid() && edits.Size() == 1);
                const JsonValue edit = edits.At(0);
                const JsonValue start = edit.Find("range").Find("start");
                const JsonValue end = edit.Find("range").Find("end");
                const uint32 sl = static_cast<uint32>(start.Find("line").TryGetInt64().Value());
                const uint32 sc =
                    static_cast<uint32>(start.Find("character").TryGetInt64().Value());
                const uint32 el = static_cast<uint32>(end.Find("line").TryGetInt64().Value());
                const uint32 ec = static_cast<uint32>(end.Find("character").TryGetInt64().Value());
                const StringView inserted = edit.Find("newText").GetString();
                StringBuilder<192> line(allocator);
                line.AppendFormat("{} | {}:{}-{}:{} | \"", action.Find("title").GetString(), sl, sc,
                                  el, ec);
                CHECK(!line.HasFailed());
                CHECK(text.TryAppend(line.View()));
                LspFixAppendEscaped(text, inserted);
                CHECK(text.TryAppend(action.Find("isPreferred").GetBool() ? "\" | preferred\n"
                                                                          : "\"\n"));

                // 적용(BOM·UTF-16·CRLF는 LspFixOffsetOf가 센다).
                const usize begin = LspFixOffsetOf(entry.text, sl, sc);
                const usize finish = LspFixOffsetOf(entry.text, el, ec);
                REQUIRE(begin <= finish && finish <= entry.text.ByteCount());
                String applied(allocator);
                CHECK(applied.TryAppend(StringView(entry.text.Data(), begin)));
                CHECK(applied.TryAppend(inserted));
                CHECK(applied.TryAppend(
                    StringView(entry.text.Data() + finish, entry.text.ByteCount() - finish)));
                const String codes = codesOf(applied.View());
                CHECK(!codes.View().Contains(entry.code, StringComparison::CaseSensitive));
                for (usize from = 0; from < codes.ByteCount();) {
                    usize to = from;
                    while (to < codes.ByteCount() && codes.View().Data()[to] != ' ') {
                        ++to;
                    }

                    const StringView code(codes.View().Data() + from, to - from);
                    CHECK(countOf(codes.View(), code) <= countOf(before.View(), code));
                    from = to + 1;
                }

                CHECK(text.TryAppend("  after: "));
                CHECK(text.TryAppend(codes.IsEmpty() ? StringView("clean") : codes.View()));
                CHECK(text.TryAppend("\n"));
            }
        }

        LspExpectText(text.View(), LspFixGolden, "code action golden");
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SparksLsp_CodeActionEdges) {
    // 범위 밖 진단은 대상이 아니다, `context.only`에 quickfix(또는 빈 글자 — 모든 kind)가 없으면
    // [], 열지 않은 문서·manifest는 [], 시작이 끝보다 뒤인 범위는 바꿔 본다, range가 없으면 -32602.
    SystemAllocator allocator;
    {
        LspFixWorkspace workspace(allocator);
        const String manifest = workspace.folder.UriOf("Game/SparksPackage.json5");
        const String lib = workspace.folder.UriOf("Game/Lib.spark");
        constexpr StringView Text =
            "class Door : ScriptableComponent {\n    var Next: Door;\n    var Ok: int = 1;\n}\n";
        auto request = [&](StringView uri, uint32 line, StringView extra) noexcept {
            String params(allocator);
            StringBuilder<192> range(allocator);
            range.AppendFormat("\"range\":{{\"start\":{{\"line\":{},\"character\":0}},\"end\":{{"
                               "\"line\":{},\"character\":80}}}}",
                               line, line);
            CHECK(!range.HasFailed());
            CHECK(params.TryAppend("{\"textDocument\":{\"uri\":\""));
            CHECK(params.TryAppend(uri));
            CHECK(params.TryAppend("\"},"));
            CHECK(params.TryAppend(range.View()));
            CHECK(params.TryAppend(extra));
            CHECK(params.TryAppend("}"));
            return params;
        };

        LspScript script(allocator);
        script.Initialize(1, workspace.root.View()).Open(workspace.main.View(), 1, Text);
        script.Call(10, "textDocument/codeAction", request(workspace.main.View(), 2, "").View());
        script.Call(11, "textDocument/codeAction",
                    request(workspace.main.View(), 1,
                            ",\"context\":{\"diagnostics\":[],\"only\":[\"refactor\"]}")
                        .View());
        script.Call(12, "textDocument/codeAction", request(lib.View(), 0, "").View());
        script.Open(manifest.View(), 1, LspFixGameManifest);
        script.Call(13, "textDocument/codeAction", request(manifest.View(), 0, "").View());
        script.Call(14, "textDocument/codeAction",
                    request(workspace.main.View(), 1,
                            ",\"context\":{\"diagnostics\":[],\"only\":[\"quickfix\"]}")
                        .View());
        script.Call(15, "textDocument/codeAction", "{\"textDocument\":{\"uri\":\"file:///x\"}}");
        script.Call(
            16, "textDocument/codeAction",
            request(workspace.main.View(), 1, ",\"context\":{\"diagnostics\":[],\"only\":[\"\"]}")
                .View());
        String reversed(allocator);
        CHECK(reversed.TryAppend("{\"textDocument\":{\"uri\":\""));
        CHECK(reversed.TryAppend(workspace.main.View()));
        CHECK(reversed.TryAppend("\"},\"range\":{\"start\":{\"line\":1,\"character\":80},"
                                 "\"end\":{\"line\":1,\"character\":0}}}"));
        script.Call(17, "textDocument/codeAction", reversed.View());
        script.End(2);
        const StringView arguments[] = {"--schema", workspace.schema.View()};
        const LspRun run = LspRunScript(allocator, arguments, script.text.View());
        CHECK(run.code == LspTestClean);
        for (int64 at = 10; at <= 13; ++at) {
            CHECK(LspResultText(allocator, run.output.View(), at).View() == "[]");
        }

        for (int64 at = 14; at <= 17; at += at == 14 ? 2 : 1) {
            CHECK(LspResultText(allocator, run.output.View(), at)
                      .View()
                      .Contains("\"title\":\"Add '?' to the type\"",
                                StringComparison::CaseSensitive));
        }
        CHECK(LspJoined(allocator, run.output.View())
                  .View()
                  .Contains("{\"jsonrpc\":\"2.0\",\"id\":15,\"error\":{\"code\":-32602,",
                            StringComparison::CaseSensitive));
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SparksLsp_CodeActionAllocationFailure) {
    // import 추가·attribute 삽입 세션의 모든 할당 실패 — 3이거나 같은 출력.
    SystemAllocator backing;
    LspFolder folder(backing);
    REQUIRE(folder.Write("Game/SparksPackage.json5", LspTestFullManifest));
    REQUIRE(folder.Write("Game/Lib.spark", LspFixLib));
    constexpr StringView Text =
        "class Door : ScriptableComponent {\n    on Owner.Damaged() {\n"
        "        await Delay(1.0, Clock.Game);\n        _ = G();\n    }\n}\n";
    const String schema = LspSchemaFolder(backing);
    const String root = folder.UriOf("");
    const String main = folder.UriOf("Game/Main.spark");
    LspScript script(backing);
    script.Initialize(1, root.View()).Open(main.View(), 1, Text);
    String params(backing);
    REQUIRE(params.TryAppend("{\"textDocument\":{\"uri\":\""));
    REQUIRE(params.TryAppend(main.View()));
    REQUIRE(params.TryAppend("\"},\"range\":{\"start\":{\"line\":0,\"character\":0},\"end\":{"
                             "\"line\":5,\"character\":1}},\"context\":{\"diagnostics\":[]}}"));
    script.Call(10, "textDocument/codeAction", params.View()).End(2);
    const StringView arguments[] = {"--schema", schema.View()};
    LspSweepAllocations(backing, arguments, script.text.View(), LspTestClean);
}

} // namespace sparks_tools_lsp_code_action_tests_file
