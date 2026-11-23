#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/StringBuilder.h>
#include <Platform/Fatal.h>
#include <SparksCompiler/Compilation.h>
#include <SparksCompiler/Diagnostic.h>
#include <SparksCompiler/Source.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

// 흐름 분석 시험이다(Docs/Design/SparksLanguage 3.11절, S0412·S0710–S0712·S0714). 규칙별
// 양성·음성 사례와 위치·읽기 타입은 corpus `Conformance/Flow`가 맡고, 여기서는 진단 문장과 수정 예,
// 단계(Types까지면 흐름 진단이 없다), word 경계를 넘는 지역 이름·대입 값 수, 깊은 중첩 반복,
// 결정성, 할당 실패를 본다.
using namespace rocks;
using namespace rocks::sparks;
namespace sparks_flow_tests_file {

namespace {

using C = DiagnosticCode;

class Fixture final {
public:
    explicit Fixture(Allocator& allocator) noexcept
        : allocator_(allocator), sources(allocator), diagnostics(allocator),
          compilation(allocator, sources) {
        // 흐름 단계까지 본다(문맥·수명 규칙 — 최상위 함수의 대기 등 — 은 SparksRulesTests).
        static_cast<void>(compilation.SetLastStage(CompilationStage::Flow));
    }

    // dependencies는 manifest의 dependencies 배열 안쪽 글자다(`"Lib"`).
    uint32 Package(StringView name, StringView dependencies = StringView()) noexcept {
        StringBuilder<128> path(allocator_);
        path.Append(name).Append("/SparksPackage.json5");
        StringBuilder<256> manifest(allocator_);
        manifest.Append("{ name: \"").Append(name).Append("\", sparks: 1, profile: \"full\"");
        if (!dependencies.IsEmpty()) {
            manifest.Append(", dependencies: [").Append(dependencies).Append("]");
        }

        manifest.Append(" }");
        auto id = sources.TryAdd(path.View(), manifest.View(), diagnostics);
        if (!id) {
            return NoPackage;
        }

        auto package = compilation.AddPackage(id.Value());
        return package ? package.Value() : NoPackage;
    }

    uint32 Module(uint32 package, StringView folder, StringView path, StringView text) noexcept {
        StringBuilder<128> display(allocator_);
        display.Append(folder).Append("/").Append(path);
        auto id = sources.TryAdd(display.View(), text, diagnostics);
        if (!id) {
            return NoModule;
        }

        auto module = compilation.AddModule(package, id.Value(), path);
        return module ? module.Value() : NoModule;
    }

    bool Schema(StringView text) noexcept {
        auto id = sources.TryAdd("Schema/Engine.sparkbind.json", text, diagnostics);
        return id && compilation.AddBindingSchema(id.Value());
    }

    bool Check() noexcept {
        if (!compilation.Check(diagnostics)) {
            return false;
        }

        diagnostics.Sort(sources);
        return true;
    }

    // index번째 진단이 이 코드·위치·메시지·수정 예인지 본다(빈 explanation은 비교하지 않는다).
    bool Says(usize index, DiagnosticCode code, uint32 line, uint32 column, StringView message,
              StringView fix, StringView explanation = StringView()) noexcept {
        bool same = index < diagnostics.Count();
        if (same) {
            const Diagnostic& actual = diagnostics[index];
            const sparks::SourceLocation at =
                sources.Get(actual.Source()).Locate(actual.Span().begin);
            same = actual.Code() == code && actual.HasSpan() && at.line == line &&
                   at.column == column && actual.Message() == message && actual.Fix() == fix &&
                   (explanation.IsEmpty() || actual.Explanation() == explanation);
        }

        if (!same) {
            Print();
        }

        return same;
    }

    // code 진단의 수다.
    usize CountOf(DiagnosticCode code) const noexcept {
        usize count = 0;
        for (usize index = 0; index < diagnostics.Count(); ++index) {
            count += diagnostics[index].Code() == code ? 1 : 0;
        }

        return count;
    }

    void Print() const noexcept {
        StringBuilder<4096> out(allocator_);
        static_cast<void>(AppendDiagnostics(out, sources, diagnostics));
        platform::WriteDiagnostic(out.View().Data(), out.ByteCount());
    }

private:
    Allocator& allocator_;

public:
    SourceSet sources;
    DiagnosticList diagnostics;
    Compilation compilation;
};

// 시험용 엔진 schema다(한 줄에 타입 하나).
constexpr StringView EngineSchema =
    R"({ "format": 1, "package": "Rocks", "module": "Engine", "types": [
{ "name": "Actor", "id": "5a7e0000000000000000000000000001", "kind": "object", "scriptExtensible": true, "fields": [ { "name": "Location", "id": "5a7e0000000000000000000000000101", "type": "vec3", "access": "readWrite" } ], "events": [ { "name": "Destroyed", "id": "5a7e0000000000000000000000000104" } ] },
{ "name": "Component", "id": "5a7e0000000000000000000000000002", "kind": "object", "fields": [ { "name": "Owner", "id": "5a7e0000000000000000000000000201", "type": "Actor" } ] },
{ "name": "ScriptableComponent", "id": "5a7e0000000000000000000000000003", "kind": "object", "base": "Component", "scriptExtensible": true },
{ "name": "World", "id": "5a7e0000000000000000000000000006", "kind": "object" },
{ "name": "Error", "id": "5a7e0000000000000000000000000008", "kind": "enum", "cases": [ "NotFound", "Busy" ] }
] })";

// 흐름 진단의 문장마다 하나씩 내는 프로그램이다(진단 아홉).
constexpr StringView MessageProgram = "class Door : ScriptableComponent {\n"      // 1
                                      "    fn Open() {\n"                         // 2
                                      "    }\n"                                   // 3
                                      "}\n"                                       // 4
                                      "\n"                                        // 5
                                      "async fn Stale(d: Door) {\n"               // 6
                                      "    await NextTick();\n"                   // 7
                                      "    (d).Open();\n"                         // 8 S0412
                                      "}\n"                                       // 9
                                      "\n"                                        // 10
                                      "fn Locals(items: Array<int>) {\n"          // 11
                                      "    for (item in items) {\n"               // 12 S0710
                                      "    }\n"                                   // 13
                                      "\n"                                        // 14
                                      "    if (let found = Find()) {\n"           // 15 S0710
                                      "    }\n"                                   // 16
                                      "\n"                                        // 17
                                      "    var unused = 1;\n"                     // 18 S0710
                                      "    var copy = vec3(0.0, 0.0, 0.0);\n"     // 19
                                      "    _ = copy;\n"                           // 20
                                      "    copy.X = 1.0;\n"                       // 21 S0711 사본
                                      "    var whole = 1;\n"                      // 22
                                      "    _ = whole;\n"                          // 23
                                      "    whole = 2;\n"                          // 24 S0711
                                      "}\n"                                       // 25
                                      "\n"                                        // 26
                                      "fn Find(): Actor? {\n"                     // 27
                                      "    return null;\n"                        // 28
                                      "}\n"                                       // 29
                                      "\n"                                        // 30
                                      "fn Params(value: int, _: int) {\n"         // 31 S0712 둘
                                      "}\n"                                       // 32
                                      "\n"                                        // 33
                                      "open class Base : ScriptableComponent {\n" // 34
                                      "    open fn Step(size: int) {\n"           // 35 S0712 고정
                                      "    }\n"                                   // 36
                                      "}\n";                                      // 37

constexpr usize MessageProgramDiagnostics = 9;

// 지역 이름·대입 값이 한 word(64개)를 넘는 함수다: 인자 70개를 await 뒤에 읽고(S0412 70개), 지역
// 이름 하나에 대입 70번 중 마지막만 읽는다(S0711 69개).
constexpr uint32 WideCount = 70;

// 깊이 30으로 중첩한 while의 가장 안쪽에서 인자를 읽고 기다린다(되돌아가는 경로로 S0412 하나).
constexpr uint32 DeepLoops = 30;

} // namespace

// 진단 문장·수정 예·위치다(S0412는 괄호 안의 이름을 쓴다).
TEST(SparksFlow_Messages) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator);
        REQUIRE(fixture.Schema(EngineSchema));
        const uint32 game = fixture.Package("Game");
        fixture.Module(game, "Game", "Main.spark", MessageProgram);
        REQUIRE(fixture.Check());
        REQUIRE(fixture.diagnostics.Count() == MessageProgramDiagnostics);
        CHECK(fixture.Says(0, C::MayBeNull, 8, 5,
                           "'d' may have been destroyed while waiting, so its members cannot be "
                           "used directly.",
                           "Check it again with 'if (let x = d) { ... }', or use '?.'.",
                           "After 'await', a local name that refers to an object has a type with "
                           "'?': the object can be destroyed while the function waits."));
        CHECK(fixture.Says(1, C::UnusedLocal, 12, 10, "'item' is never read.",
                           "Name the loop variable '_'.",
                           "A name that is declared but never used is usually a mistake."));
        CHECK(fixture.Says(2, C::UnusedLocal, 15, 13, "'found' is never read.",
                           "Test it with 'value != null' instead."));
        CHECK(fixture.Says(3, C::UnusedLocal, 18, 9, "'unused' is never read.",
                           "Remove it, or use the value."));
        CHECK(fixture.Says(4, C::UnreadStore, 21, 5, "The change to 'copy' is never read.",
                           "Write the copy back, as in 'Hinge.RelativeLocation = p;', or remove "
                           "the change.",
                           "A local value is a copy: changing it does not change where it came "
                           "from."));
        CHECK(fixture.Says(5, C::UnreadStore, 24, 5, "The value written to 'whole' is never read.",
                           "Remove the assignment, or use the value.",
                           "No path reads it before it is overwritten or the function ends."));
        CHECK(fixture.Says(6, C::UnusedParameter, 31, 11, "Parameter 'value' is never read.",
                           "Remove the parameter, or use it.",
                           "A parameter that is never used is usually a mistake."));
        CHECK(fixture.Says(7, C::UnusedParameter, 31, 23,
                           "A parameter named '_' is only for a fixed signature.",
                           "Remove the parameter.",
                           "'_' marks a parameter that an override, an interface method or an "
                           "event handler must have."));
        CHECK(fixture.Says(8, C::UnusedParameter, 35, 18, "Parameter 'size' is never read.",
                           "Name the parameter '_'.",
                           "The signature is fixed by an override, an interface method or an "
                           "event."));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// S0714는 원문에 쓴 경로(의존 패키지 이름 포함)로 알리고, 별칭 import는 경로 자리다.
TEST(SparksFlow_UnusedImportMessages) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator);
        REQUIRE(fixture.Schema(EngineSchema));
        const uint32 game = fixture.Package("Game", "\"Lib\"");
        const uint32 lib = fixture.Package("Lib");
        fixture.Module(lib, "Lib", "Spare.spark", "fn Spare() {\n}\n");
        fixture.Module(game, "Game", "Util.spark", "fn Help() {\n}\n");
        fixture.Module(game, "Game", "Main.spark", "import Lib.Spare;\nimport Util as U;\n");
        REQUIRE(fixture.Check());
        REQUIRE(fixture.diagnostics.Count() == 2);
        CHECK(fixture.Says(0, C::UnusedImport, 1, 8,
                           "Module 'Lib.Spare' is imported but never used.", "Remove the import.",
                           "None of the declarations of the module are used here."));
        CHECK(fixture.Says(1, C::UnusedImport, 2, 8, "Module 'Util' is imported but never used.",
                           "Remove the import."));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// Types 단계까지면 흐름 진단(S0412 포함)이 없다.
TEST(SparksFlow_TypesStageHasNoFlowDiagnostics) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator);
        REQUIRE(fixture.Schema(EngineSchema));
        const uint32 game = fixture.Package("Game");
        fixture.Module(game, "Game", "Main.spark", MessageProgram);
        REQUIRE(fixture.compilation.SetLastStage(CompilationStage::Types));
        REQUIRE(fixture.Check());
        if (fixture.diagnostics.Count() != 0) {
            fixture.Print();
        }

        CHECK(fixture.diagnostics.Count() == 0);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 지역 이름·대입 값 비트가 여러 word에 걸친다.
TEST(SparksFlow_WideFunctions) {
    SystemAllocator allocator;
    {
        StringBuilder<8192> source(allocator);
        source.Append("async fn Wide(");
        for (uint32 index = 0; index < WideCount; ++index) {
            source.AppendFormat("{}a{}: Actor", index == 0 ? "" : ", ", index);
        }

        source.Append(") {\n    var x = 0;\n");
        for (uint32 index = 1; index <= WideCount; ++index) {
            source.AppendFormat("    x = {};\n", index);
        }

        source.Append("    _ = x;\n    await NextTick();\n");
        for (uint32 index = 0; index < WideCount; ++index) {
            source.AppendFormat("    _ = a{}.Location;\n", index);
        }

        source.Append("}\n");
        Fixture fixture(allocator);
        REQUIRE(fixture.Schema(EngineSchema));
        const uint32 game = fixture.Package("Game");
        fixture.Module(game, "Game", "Main.spark", source.View());
        REQUIRE(fixture.Check());
        CHECK(fixture.CountOf(C::MayBeNull) == WideCount);
        CHECK(fixture.CountOf(C::UnreadStore) == WideCount - 1);
        CHECK(fixture.diagnostics.Count() == WideCount * 2 - 1);
        // 마지막 대입(x = 70)은 읽는다.
        const uint32 lastStoreLine = 2 + WideCount;
        bool lastReported = false;
        for (usize index = 0; index < fixture.diagnostics.Count(); ++index) {
            const Diagnostic& entry = fixture.diagnostics[index];
            lastReported = lastReported ||
                           (entry.Code() == C::UnreadStore &&
                            fixture.sources.Get(entry.Source()).Locate(entry.Span().begin).line ==
                                lastStoreLine);
        }

        CHECK(!lastReported);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 깊게 중첩한 반복도 고정점을 구한다(단계 그래프와 worklist — 반복마다 안쪽을 다시 돌지 않는다).
TEST(SparksFlow_DeepLoops) {
    SystemAllocator allocator;
    {
        StringBuilder<4096> source(allocator);
        source.Append("class Door : ScriptableComponent {\n    var Flag = false;\n\n"
                      "    async fn Deep(d: Door) {\n");
        for (uint32 depth = 0; depth < DeepLoops; ++depth) {
            source.Append("while (Flag) {\n");
        }

        source.Append("_ = d.Flag;\nawait NextTick();\n");
        for (uint32 depth = 0; depth < DeepLoops; ++depth) {
            source.Append("}\n");
        }

        source.Append("    }\n}\n");
        Fixture fixture(allocator);
        REQUIRE(fixture.Schema(EngineSchema));
        const uint32 game = fixture.Package("Game");
        fixture.Module(game, "Game", "Main.spark", source.View());
        // 흐름 단계만 본다(바깥 반복은 회차마다 기다리지 않는다 — S0603은 Rules 단계).
        REQUIRE(fixture.compilation.SetLastStage(CompilationStage::Flow));
        REQUIRE(fixture.Check());
        REQUIRE(fixture.diagnostics.Count() == 1);
        CHECK(fixture.diagnostics[0].Code() == C::MayBeNull);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 같은 입력이면 진단이 바이트 단위로 같다.
TEST(SparksFlow_Deterministic) {
    SystemAllocator allocator;
    StringBuilder<8192> first(allocator);
    StringBuilder<8192> second(allocator);
    for (uint32 round = 0; round < 2; ++round) {
        Fixture fixture(allocator);
        REQUIRE(fixture.Schema(EngineSchema));
        const uint32 game = fixture.Package("Game");
        fixture.Module(game, "Game", "Main.spark", MessageProgram);
        REQUIRE(fixture.Check());
        REQUIRE(
            AppendDiagnostics(round == 0 ? first : second, fixture.sources, fixture.diagnostics));
    }

    CHECK(first.View() == second.View());
}

// Check 안의 할당 하나하나를 실패시킨다. 실패는 OutOfMemory로만 돌아오고, 성공하면 진단이 깨끗한
// 실행과 같다.
TEST(SparksFlow_AllocationFailureEverywhere) {
    SystemAllocator backing;
    uint64 checkAttempts = 0;
    StringBuilder<8192> clean(backing);
    uint64 failures = 0;
    for (uint64 failAt = 0; failAt == 0 || failAt <= checkAttempts; ++failAt) {
        test::NFailAllocator allocator(backing);
        {
            allocator.DisableFailure();
            Fixture fixture(allocator);
            REQUIRE(fixture.Schema(EngineSchema));
            const uint32 game = fixture.Package("Game");
            fixture.Module(game, "Game", "Main.spark", MessageProgram);
            fixture.Module(game, "Game", "Util.spark", "import Main;\n");
            if (failAt != 0) {
                allocator.FailOnAttempt(failAt);
            }

            auto checked = fixture.compilation.Check(fixture.diagnostics);
            const uint64 attempts = allocator.Attempts();
            allocator.DisableFailure(); // 진단 문장 만들기는 실패시키지 않는다
            if (failAt == 0) {
                REQUIRE(checked);
                checkAttempts = attempts;
                fixture.diagnostics.Sort(fixture.sources);
                REQUIRE(fixture.diagnostics.Count() == MessageProgramDiagnostics + 1);
                REQUIRE(AppendDiagnostics(clean, fixture.sources, fixture.diagnostics));
            } else if (checked) {
                fixture.diagnostics.Sort(fixture.sources);
                StringBuilder<8192> text(backing);
                REQUIRE(AppendDiagnostics(text, fixture.sources, fixture.diagnostics));
                CHECK(text.View() == clean.View());
            } else {
                ++failures;
                CHECK(checked.Error() == ErrorCode::OutOfMemory);
            }
        }
        CHECK(backing.Stats().liveAllocations == 0);
    }

    CHECK(failures > 0);
}

} // namespace sparks_flow_tests_file
