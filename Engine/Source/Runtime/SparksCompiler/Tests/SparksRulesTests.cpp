#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/StringBuilder.h>
#include <Platform/Fatal.h>
#include <SparksCompiler/Compilation.h>
#include <SparksCompiler/Diagnostic.h>
#include <SparksCompiler/Source.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

// 문맥·수명 규칙 시험이다(Docs/Design/SparksLanguage.ko.md 3.12절, S0413·S08xx). 규칙별 양성·음성
// 사례와 위치는 corpus `Conformance/Rules`가 맡고, 여기서는 진단 문장과 수정 예, 단계(Flow까지면
// 규칙 진단이 없다), 결정성, 할당 실패를 본다.
using namespace rocks;
using namespace rocks::sparks;
namespace sparks_rules_tests_file {

namespace {

using C = DiagnosticCode;

class Fixture final {
public:
    explicit Fixture(Allocator& allocator) noexcept
        : allocator_(allocator), sources(allocator), diagnostics(allocator),
          compilation(allocator, sources) {
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
{ "name": "LevelScript", "id": "5a7e0000000000000000000000000009", "kind": "object", "base": "Actor", "scriptExtensible": true },
{ "name": "Component", "id": "5a7e0000000000000000000000000002", "kind": "object", "fields": [ { "name": "Owner", "id": "5a7e0000000000000000000000000201", "type": "Actor" } ] },
{ "name": "ScriptableComponent", "id": "5a7e0000000000000000000000000003", "kind": "object", "base": "Component", "scriptExtensible": true },
{ "name": "World", "id": "5a7e0000000000000000000000000006", "kind": "object" },
{ "name": "Error", "id": "5a7e0000000000000000000000000008", "kind": "enum", "cases": [ "NotFound", "Busy" ] }
] })";

// 규칙 진단의 문장마다 하나씩 내는 프로그램이다(진단 여섯).
constexpr StringView RulesProgram = "event Actor.Pushed(by: Actor);\n"     // 1
                                    "\n"                                   // 2
                                    "class Door : ScriptableComponent {\n" // 3
                                    "    on Strat() {\n"                   // 4 S0803
                                    "    }\n"                              // 5
                                    "\n"                                   // 6
                                    "    on Owner.Destroyed() {\n"         // 7 S0816
                                    "        await NextTick();\n"          // 8
                                    "    }\n"                              // 9
                                    "\n"                                   // 10
                                    "    fn Ring(a: Actor) {\n"            // 11
                                    "        a.Destroyed();\n"             // 12 S0812
                                    "        emit a.Pushed(a);\n"          // 13
                                    "    }\n"                              // 14
                                    "\n"                                   // 15
                                    "    on Owner.Pushed(_: int) {\n"      // 16 S0810
                                    "    }\n"                              // 17
                                    "}\n"                                  // 18
                                    "\n"                                   // 19
                                    "class Stage : LevelScript {\n"        // 20
                                    "    @edit var Gate: Actor;\n"         // 21 S0413
                                    "\n"                                   // 22
                                    "    on Gate.Destroyed() {\n"          // 23
                                    "    }\n"                              // 24
                                    "}\n"                                  // 25
                                    "\n"                                   // 26
                                    "@restart\n"                           // 27 S0817
                                    "fn Top() {\n"                         // 28
                                    "}\n";                                 // 29

constexpr usize RulesProgramDiagnostics = 6;

} // namespace

// 진단 문장·수정 예·위치다.
TEST(SparksRules_Messages) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator);
        REQUIRE(fixture.Schema(EngineSchema));
        const uint32 game = fixture.Package("Game");
        fixture.Module(game, "Game", "Main.spark", RulesProgram);
        REQUIRE(fixture.Check());
        REQUIRE(fixture.diagnostics.Count() == RulesProgramDiagnostics);
        CHECK(fixture.Says(0, C::UnknownHook, 4, 8, "'Strat' is not a lifecycle hook.",
                           "Write 'on Start(...)'.",
                           "'on X()' with one name is a lifecycle hook; events are handled as "
                           "'on Source.Event()'."));
        CHECK(fixture.Says(1, C::MissingReentrancyPolicy, 7, 8,
                           "A handler that waits needs a reentrancy policy.",
                           "Add '@skipWhileBusy' to drop those events, or '@restart' to start "
                           "again.",
                           "While it waits, the same source can raise the event again."));
        CHECK(fixture.Says(2, C::EventCall, 12, 9, "An event is raised with 'emit', not called.",
                           "Write 'emit Source.Event(...)', or 'await Source.Event' to wait for "
                           "it."));
        CHECK(fixture.Says(3, C::HandlerSignature, 16, 24,
                           "Parameter 1 of 'Pushed' has the type 'Actor'.", "Write 'Actor'.",
                           "Handler parameter types match the arguments exactly."));
        CHECK(fixture.Says(4, C::LevelScriptRequiredObject, 21, 21,
                           "Object fields of a LevelScript must be optional.",
                           "Declare it as 'Actor?'.",
                           "Level actors can be destroyed during play, so a LevelScript reads "
                           "them through 'T?'."));
        CHECK(fixture.Says(5, C::MisplacedReentrancyPolicy, 27, 1,
                           "A reentrancy policy is only for event handlers and physics hooks.",
                           "Remove the attribute."));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// S0815는 선언한 모듈의 import를 제시한다.
TEST(SparksRules_ExtensionEventImportMessage) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator);
        REQUIRE(fixture.Schema(EngineSchema));
        const uint32 game = fixture.Package("Game");
        fixture.Module(game, "Game", "Combat/Signals.spark", "event Actor.Pushed(by: Actor);\n");
        fixture.Module(game, "Game", "Main.spark",
                       "class Bell : ScriptableComponent {\n    fn Ring(a: Actor) {\n        emit "
                       "a.Pushed(a);\n    }\n}\n");
        REQUIRE(fixture.Check());
        REQUIRE(fixture.diagnostics.Count() == 1);
        CHECK(fixture.Says(0, C::ExtensionEventNotImported, 3, 16,
                           "Extension event 'Pushed' is declared in module 'Combat.Signals', which "
                           "is not imported here.",
                           "Add 'import Combat.Signals;'."));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// Flow 단계까지면 규칙 진단이 없다(이벤트 호출은 그 단계에서 S0422로 본다).
TEST(SparksRules_FlowStageHasNoRuleDiagnostics) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator);
        REQUIRE(fixture.Schema(EngineSchema));
        const uint32 game = fixture.Package("Game");
        fixture.Module(game, "Game", "Main.spark", RulesProgram);
        REQUIRE(fixture.compilation.SetLastStage(CompilationStage::Flow));
        REQUIRE(fixture.Check());
        for (usize index = 0; index < fixture.diagnostics.Count(); ++index) {
            CHECK(static_cast<uint32>(fixture.diagnostics[index].Code()) < 800);
            CHECK(fixture.diagnostics[index].Code() != C::LevelScriptRequiredObject);
        }
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 같은 입력이면 진단이 바이트 단위로 같다.
TEST(SparksRules_Deterministic) {
    SystemAllocator allocator;
    StringBuilder<8192> first(allocator);
    StringBuilder<8192> second(allocator);
    for (uint32 round = 0; round < 2; ++round) {
        Fixture fixture(allocator);
        REQUIRE(fixture.Schema(EngineSchema));
        const uint32 game = fixture.Package("Game");
        fixture.Module(game, "Game", "Main.spark", RulesProgram);
        REQUIRE(fixture.Check());
        REQUIRE(
            AppendDiagnostics(round == 0 ? first : second, fixture.sources, fixture.diagnostics));
    }

    CHECK(first.View() == second.View());
}

// Check 안의 할당 하나하나를 실패시킨다. 실패는 OutOfMemory로만 돌아오고, 성공하면 진단이 깨끗한
// 실행과 같다.
TEST(SparksRules_AllocationFailureEverywhere) {
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
            fixture.Module(game, "Game", "Main.spark", RulesProgram);
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
                REQUIRE(fixture.diagnostics.Count() == RulesProgramDiagnostics);
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

} // namespace sparks_rules_tests_file
