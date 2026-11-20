#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/StringBuilder.h>
#include <Platform/Fatal.h>
#include <SparksCompiler/Compilation.h>
#include <SparksCompiler/Diagnostic.h>
#include <SparksCompiler/Source.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

// 이름 해석 시험이다(Docs/Design/SparksLanguage.ko.md 3.9절, S0306–S0325, schema S1120의 이름
// 부분). 규칙별 양성·음성 사례와 위치는 corpus `Conformance/Names`가 맡고, 여기서는 API(심볼·타입·
// 시그니처·노드 기록·덤프·단계), 진단 문장과 수정 예, schema 이름 해석, 결정성, 할당 실패를 본다.
using namespace rocks;
using namespace rocks::sparks;
namespace sparks_names_tests_file {

namespace {

using C = DiagnosticCode;

struct Expected {
    DiagnosticCode code;
    StringView path;
    uint32 line = 0;
    uint32 column = 0;
};

class Fixture final {
public:
    explicit Fixture(Allocator& allocator) noexcept
        : allocator_(allocator), sources(allocator), diagnostics(allocator),
          compilation(allocator, sources) {
    }

    uint32 Package(StringView folder, StringView manifest) noexcept {
        StringBuilder<128> path(allocator_);
        path.Append(folder).Append("/SparksPackage.json5");
        auto id = sources.TryAdd(path.View(), manifest, diagnostics);
        if (!id) {
            return NoPackage;
        }

        auto package = compilation.AddPackage(id.Value());
        return package ? package.Value() : NoPackage;
    }

    uint32 SimplePackage(StringView name, StringView dependencies = StringView()) noexcept {
        StringBuilder<256> manifest(allocator_);
        manifest.Append("{ name: \"").Append(name).Append("\", sparks: 1, profile: \"full\"");
        if (!dependencies.IsEmpty()) {
            manifest.Append(", dependencies: [").Append(dependencies).Append("]");
        }

        manifest.Append(" }");
        return Package(name, manifest.View());
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

    bool Schema(StringView path, StringView text) noexcept {
        auto id = sources.TryAdd(path, text, diagnostics);
        return id && compilation.AddBindingSchema(id.Value());
    }

    bool Check() noexcept {
        if (!compilation.Check(diagnostics)) {
            return false;
        }

        diagnostics.Sort(sources);
        return true;
    }

    bool Reports(Span<const Expected> expected) noexcept {
        bool same = diagnostics.Count() == expected.Size();
        for (usize index = 0; same && index < expected.Size(); ++index) {
            const Diagnostic& actual = diagnostics[index];
            const SourceFile& file = sources.Get(actual.Source());
            same = actual.Code() == expected[index].code && file.Path() == expected[index].path;
            if (same && expected[index].line == 0) {
                same = !actual.HasSpan();
            } else if (same) {
                const SourceLocation location = file.Locate(actual.Span().begin);
                same = actual.HasSpan() && location.line == expected[index].line &&
                       location.column == expected[index].column;
            }
        }

        if (!same) {
            Print();
        }

        return same;
    }

    bool Reports(DiagnosticCode code, StringView path, uint32 line, uint32 column) noexcept {
        const Expected expected[] = {{code, path, line, column}};
        return Reports(expected);
    }

    bool Clean() noexcept {
        return Reports(Span<const Expected>());
    }

    // index번째 진단의 메시지·설명·수정 예가 이것인지 본다(빈 explanation은 비교하지 않는다).
    bool Says(usize index, StringView message, StringView fix,
              StringView explanation = StringView()) noexcept {
        if (index >= diagnostics.Count()) {
            Print();
            return false;
        }

        const Diagnostic& actual = diagnostics[index];
        const bool same = actual.Message() == message && actual.Fix() == fix &&
                          (explanation.IsEmpty() || actual.Explanation() == explanation);
        if (!same) {
            Print();
        }

        return same;
    }

    // 사용자 선언 심볼을 이름과 종류로 찾는다(처음 것).
    uint32 Find(StringView name, SymbolKind kind) const noexcept {
        const Span<const Symbol> symbols = compilation.Symbols();
        for (uint32 index = 0; index < symbols.Size(); ++index) {
            if (symbols[index].name == name && symbols[index].kind == kind) {
                return index;
            }
        }

        return NoSymbol;
    }

    bool TypeText(uint32 type, StringView expected) noexcept {
        StringBuilder<256> text(allocator_);
        const bool written = static_cast<bool>(compilation.AppendTypeText(text, type));
        if (!written || text.View() != expected) {
            platform::WriteDiagnostic(text.View().Data(), text.ByteCount());
            platform::WriteDiagnostic("\n", 1);
            return false;
        }

        return true;
    }

    void Print() const noexcept {
        StringBuilder<2048> out(allocator_);
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
{ "name": "Actor", "id": "5a7e0000000000000000000000000001", "kind": "object", "scriptExtensible": true, "fields": [ { "name": "Location", "id": "5a7e0000000000000000000000000101", "type": "vec3", "access": "readWrite" } ], "functions": [ { "name": "Find", "id": "5a7e0000000000000000000000000102", "typeParameters": [ { "name": "T", "constraint": "Component" } ], "returns": "T?" }, { "name": "Jump", "id": "5a7e0000000000000000000000000103" } ] },
{ "name": "Component", "id": "5a7e0000000000000000000000000002", "kind": "object", "fields": [ { "name": "Owner", "id": "5a7e0000000000000000000000000201", "type": "Actor" } ] },
{ "name": "ScriptComponent", "id": "5a7e0000000000000000000000000003", "kind": "object", "base": "Component", "scriptExtensible": true, "functions": [ { "name": "DescribeState", "id": "5a7e0000000000000000000000000301", "overridable": true, "parameters": [ { "name": "detailed", "type": "bool" } ], "returns": "string" } ] },
{ "name": "WorldService", "id": "5a7e0000000000000000000000000004", "kind": "object", "scriptExtensible": true },
{ "name": "DataAsset", "id": "5a7e0000000000000000000000000005", "kind": "asset", "scriptExtensible": true },
{ "name": "World", "id": "5a7e0000000000000000000000000006", "kind": "object" },
{ "name": "Usable", "id": "5a7e0000000000000000000000000007", "kind": "interface", "functions": [ { "name": "Use", "id": "5a7e0000000000000000000000000701" } ] },
{ "name": "Error", "id": "5a7e0000000000000000000000000008", "kind": "enum", "cases": [ "NotFound", "Busy" ] }
] })";

} // namespace

// 선언·참조의 노드 기록과 심볼(종류·타입·시그니처·기반·flag)이다.
TEST(SparksNames_SymbolsAndNodes) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator);
        REQUIRE(fixture.Schema("Schema/Engine.sparkbind.json", EngineSchema));
        const uint32 game = fixture.SimplePackage("Game");
        const uint32 main = fixture.Module(game, "Game", "Main.spark",
                                           "class Door : ScriptComponent, Usable {\n"
                                           "    var Speed: int = 1;\n"
                                           "    fn Use() { }\n"
                                           "    override fn DescribeState(_: bool): string {\n"
                                           "        let next = Speed;\n"
                                           "        _ = next;\n"
                                           "        return \"\";\n"
                                           "    }\n"
                                           "}\n"
                                           "class Settings2 : DataAsset { }\n");
        REQUIRE(fixture.Check());
        CHECK(fixture.Clean());
        const Compilation& c = fixture.compilation;
        const Span<const Symbol> symbols = c.Symbols();
        const uint32 door = fixture.Find("Door", SymbolKind::Class);
        const uint32 speed = fixture.Find("Speed", SymbolKind::Field);
        const uint32 next = fixture.Find("next", SymbolKind::Local);
        const uint32 describe = fixture.Find("DescribeState", SymbolKind::Method);
        const uint32 engineDescribe = fixture.Find("DescribeState", SymbolKind::EngineFunction);
        const uint32 script = fixture.Find("ScriptComponent", SymbolKind::EngineType);
        const uint32 usable = fixture.Find("Usable", SymbolKind::EngineType);
        const uint32 settings = fixture.Find("Settings2", SymbolKind::Class);
        REQUIRE(door != NoSymbol && speed != NoSymbol && next != NoSymbol);
        REQUIRE(describe != NoSymbol && engineDescribe != NoSymbol && script != NoSymbol);
        // 선언 이름 노드는 그 선언을 가리킨다.
        CHECK(c.NodeSymbol(main, symbols[door].nameNode) == door);
        CHECK(c.NodeSymbol(main, symbols[next].nameNode) == next);
        CHECK(symbols[door].module == main);
        CHECK(symbols[speed].owner == door);
        // 기반: 첫 기반과 해석한 기반 전체.
        CHECK(symbols[door].base == script);
        REQUIRE(symbols[door].baseCount == 2);
        CHECK(c.SymbolBases()[symbols[door].firstBase] == script);
        CHECK(c.SymbolBases()[symbols[door].firstBase + 1] == usable);
        CHECK((symbols[door].flags & SymbolFlags::Lifecycle) != 0);
        CHECK((symbols[settings].flags & SymbolFlags::Lifecycle) == 0);
        CHECK((symbols[describe].flags & SymbolFlags::Override) != 0);
        CHECK((symbols[engineDescribe].flags & SymbolFlags::Overridable) != 0);
        // 필드 타입은 그 Type 노드의 타입이다.
        const SyntaxTree& tree = *c.ModuleTree(main);
        CHECK(c.NodeType(main, tree.Child(symbols[speed].node, 3)) == symbols[speed].type);
        CHECK(fixture.TypeText(symbols[speed].type, "int"));
        // 시그니처: 같은 매개변수·반환 타입이다(override 검사가 같다고 본 것).
        const Signature& own = c.Signatures()[symbols[describe].signature];
        const Signature& base = c.Signatures()[symbols[engineDescribe].signature];
        REQUIRE(own.parameterCount == 1);
        CHECK(own.returnType == base.returnType);
        CHECK(c.SignatureParameters()[own.firstParameter] ==
              c.SignatureParameters()[base.firstParameter]);
        CHECK(fixture.TypeText(own.returnType, "string"));
        // 범위 밖은 없음이다.
        CHECK(c.NodeSymbol(main, tree.NodeCount()) == NoSymbol);
        CHECK(c.NodeType(main + 1, 0) == NoType);
        CHECK(c.NodeSymbol(NoModule, 0) == NoSymbol);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 같은 타입은 같은 번호다(`int`=`int64`, 타입 인자·`?` 포함). 정규 표기.
TEST(SparksNames_TypeInterningAndText) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator);
        REQUIRE(fixture.Schema("Schema/Engine.sparkbind.json", EngineSchema));
        const uint32 game = fixture.SimplePackage("Game");
        fixture.Module(game, "Game", "Combat/Main.spark",
                       "struct Bag {\n"
                       "    var A: int;\n"
                       "    var B: int64;\n"
                       "    var C: Array<int>?;\n"
                       "    var D: Array<int64>?;\n"
                       "    var E: Map<string, Array<Bag>>;\n"
                       "    var F: Actor?;\n"
                       "    var G: float64;\n"
                       "    var H: Nope;\n"
                       "    var I: Array<Nope>;\n"
                       "}\n");
        REQUIRE(fixture.Check());
        const Span<const Symbol> symbols = fixture.compilation.Symbols();
        auto type = [&](StringView name) noexcept {
            return symbols[fixture.Find(name, SymbolKind::Field)].type;
        };
        CHECK(type("A") == type("B"));
        CHECK(type("C") == type("D"));
        CHECK(type("A") != type("C"));
        CHECK(fixture.TypeText(type("B"), "int"));
        CHECK(fixture.TypeText(type("D"), "Array<int>?"));
        CHECK(fixture.TypeText(type("E"), "Map<string, Array<Game:Combat.Main.Bag>>"));
        CHECK(fixture.TypeText(type("F"), "Rocks.Actor?"));
        CHECK(fixture.TypeText(type("G"), "float"));
        // 해석하지 못한 타입(과 그것이 낀 타입)은 NoType이고 표기는 `?`다.
        CHECK(type("H") == NoType);
        CHECK(type("I") == NoType);
        CHECK(fixture.TypeText(NoType, "?"));
        CHECK(fixture.compilation.Types()[0].symbol == NoSymbol);
        // 같은 입력의 타입 표는 결정적이다(인자 구간을 공유하지 않아도 같은 번호).
        const TypeEntry& entry = fixture.compilation.Types()[type("C")];
        CHECK(entry.optional);
        CHECK(entry.argumentCount == 1);
        CHECK(fixture.compilation.TypeArguments()[entry.firstArgument] == type("A"));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 엔진 schema의 멤버·타입 매개변수·제약·시그니처다.
TEST(SparksNames_EngineSchemaSymbols) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator);
        REQUIRE(fixture.Schema("Schema/Engine.sparkbind.json", EngineSchema));
        REQUIRE(fixture.Check());
        CHECK(fixture.Clean());
        const Span<const Symbol> symbols = fixture.compilation.Symbols();
        const uint32 find = fixture.Find("Find", SymbolKind::EngineFunction);
        const uint32 parameter = fixture.Find("T", SymbolKind::EngineTypeParameter);
        const uint32 component = fixture.Find("Component", SymbolKind::EngineType);
        const uint32 script = fixture.Find("ScriptComponent", SymbolKind::EngineType);
        const uint32 location = fixture.Find("Location", SymbolKind::EngineField);
        const uint32 notFound = fixture.Find("NotFound", SymbolKind::EnumCase);
        REQUIRE(find != NoSymbol && parameter != NoSymbol && location != NoSymbol);
        CHECK(symbols[parameter].owner == find);
        CHECK(symbols[parameter].base == component); // 제약
        CHECK(symbols[script].base == component);
        CHECK(symbols[notFound].owner == fixture.Find("Error", SymbolKind::EngineType));
        CHECK(fixture.TypeText(symbols[location].type, "vec3"));
        const Signature& signature = fixture.compilation.Signatures()[symbols[find].signature];
        CHECK(signature.parameterCount == 0);
        CHECK(fixture.TypeText(signature.returnType, "T?"));
        CHECK(fixture.compilation.Types()[signature.returnType].symbol == parameter);
        const uint32 jump = fixture.Find("Jump", SymbolKind::EngineFunction);
        CHECK(fixture.TypeText(fixture.compilation.Signatures()[symbols[jump].signature].returnType,
                               "void"));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// schema의 이름 오류(S1120): 표기의 모르는 타입·타입 인자 수·한정 이름, 기반(모름·kind 다름·순환),
// 제약. 오류가 있어도 파일은 계속 쓴다.
TEST(SparksNames_SchemaNameErrors) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator);
        REQUIRE(fixture.Schema("Schema/Bad.sparkbind.json",
                               R"({ "format": 1, "package": "Rocks", "module": "Bad", "types": [
{ "name": "A", "id": "5a7e0000000000000000000000000001", "kind": "object", "fields": [ { "name": "X", "id": "5a7e0000000000000000000000000002", "type": "Nope" } ] },
{ "name": "B", "id": "5a7e0000000000000000000000000003", "kind": "object", "fields": [ { "name": "X", "id": "5a7e0000000000000000000000000004", "type": "Array<int, int>" } ] },
{ "name": "C", "id": "5a7e0000000000000000000000000005", "kind": "object", "fields": [ { "name": "X", "id": "5a7e0000000000000000000000000006", "type": "Rocks.A" } ] },
{ "name": "D", "id": "5a7e0000000000000000000000000007", "kind": "object", "base": "Missing" },
{ "name": "E", "id": "5a7e0000000000000000000000000008", "kind": "object", "base": "S" },
{ "name": "S", "id": "5a7e0000000000000000000000000009", "kind": "struct", "fields": [ { "name": "X", "id": "5a7e000000000000000000000000000a", "type": "Map<int, Array<Nope>>" } ] },
{ "name": "F", "id": "5a7e000000000000000000000000000b", "kind": "object", "base": "G" },
{ "name": "G", "id": "5a7e000000000000000000000000000c", "kind": "object", "base": "F" },
{ "name": "H", "id": "5a7e000000000000000000000000000d", "kind": "object", "functions": [ { "name": "Get", "id": "5a7e000000000000000000000000000e", "typeParameters": [ { "name": "T", "constraint": "S" } ], "parameters": [ { "name": "value", "type": "T" } ], "returns": "Log" } ] }
] })"));
        REQUIRE(fixture.Check());
        constexpr StringView Path = "Schema/Bad.sparkbind.json";
        const Expected expected[] = {
            {C::InvalidBindingSchema, Path, 2, 153}, {C::InvalidBindingSchema, Path, 3, 153},
            {C::InvalidBindingSchema, Path, 4, 153}, {C::InvalidBindingSchema, Path, 5, 84},
            {C::InvalidBindingSchema, Path, 6, 84},  {C::InvalidBindingSchema, Path, 7, 153},
            {C::InvalidBindingSchema, Path, 8, 84},  {C::InvalidBindingSchema, Path, 10, 199},
            {C::InvalidBindingSchema, Path, 10, 271}};
        CHECK(fixture.Reports(expected));
        CHECK(fixture.Says(0, "Type 'Nope' is not known to the binding schemas.",
                           "Regenerate the binding schema; do not edit it by hand."));
        CHECK(fixture.Says(1, "Type 'Array' takes 1 type arguments.",
                           "Regenerate the binding schema; do not edit it by hand."));
        CHECK(fixture.Says(2, "Type 'Rocks.A' is not known to the binding schemas.",
                           "Regenerate the binding schema; do not edit it by hand."));
        CHECK(fixture.Says(3, "Base type 'Missing' is not a binding schema type of the same kind.",
                           "Regenerate the binding schema; do not edit it by hand."));
        CHECK(fixture.Says(6, "The base types form a cycle.",
                           "Regenerate the binding schema; do not edit it by hand."));
        CHECK(fixture.Says(7,
                           "Constraint 'S' is not an object or interface type of the binding "
                           "schemas.",
                           "Regenerate the binding schema; do not edit it by hand."));
        CHECK(fixture.Says(8, "Type 'Log' is not known to the binding schemas.",
                           "Regenerate the binding schema; do not edit it by hand."));
        CHECK(fixture.compilation.SchemaIsValid(0));
        // 해석하지 못한 표기의 타입은 NoType, 순환은 끊긴다.
        const Span<const Symbol> symbols = fixture.compilation.Symbols();
        CHECK(symbols[fixture.Find("X", SymbolKind::EngineField)].type == NoType);
        const uint32 f = fixture.Find("F", SymbolKind::EngineType);
        const uint32 g = fixture.Find("G", SymbolKind::EngineType);
        CHECK(symbols[f].base == NoSymbol || symbols[g].base == NoSymbol);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 진단 문장과 수정 예다(코드마다 하나 이상).
TEST(SparksNames_DeclarationMessages) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator);
        REQUIRE(fixture.Schema("Schema/Engine.sparkbind.json", EngineSchema));
        const uint32 game = fixture.SimplePackage("Game");
        fixture.Module(game, "Game", "Main.spark",
                       "class door : ScriptComponent {\n"  // 1 S0306
                       "    fn Update() { }\n"             // 2 S0308
                       "    fn Run(If: int) { _ = If; }\n" // 3 S0306 (키워드 철자)
                       "    var World: int;\n"             // 4 S0307
                       "    var Location2: int;\n"         // 5
                       "    var LOCATION2: int;\n"         // 6 S0309
                       "    fn DescribeState(detailed: bool): string { return \"\"; }\n" // 7 S0313
                       "    override fn Jump() { }\n" // 8 S0314 대상 없음
                       "}\n"
                       "class Mover : Actor {\n"      // 10
                       "    override fn Jump() { }\n" // 11 S0314 재정의 불가
                       "}\n");
        REQUIRE(fixture.Check());
        constexpr StringView Path = "Game/Main.spark";
        const Expected expected[] = {{C::NamingRule, Path, 1, 7},
                                     {C::HookName, Path, 2, 8},
                                     {C::NamingRule, Path, 3, 12},
                                     {C::ReservedName, Path, 4, 9},
                                     {C::DuplicateDeclaration, Path, 6, 9},
                                     {C::HidesInheritedMember, Path, 7, 8},
                                     {C::OverrideWithoutTarget, Path, 8, 17},
                                     {C::OverrideWithoutTarget, Path, 11, 17}};
        REQUIRE(fixture.Reports(expected));
        CHECK(fixture.Says(0, "Type name 'door' must start with an uppercase letter.",
                           "Rename it to 'Door'.",
                           "Types, members, enum cases and top-level declarations use "
                           "PascalCase."));
        CHECK(fixture.Says(1, "'Update' is a lifecycle hook name.",
                           "Handle the hook with 'on Tick() { ... }'."));
        CHECK(fixture.Says(2, "Parameter name 'If' must start with a lowercase letter.",
                           "Rename it so that it starts with a lowercase letter.",
                           "Parameters and local names use lowerCamel."));
        CHECK(fixture.Says(3, "'World' is a reserved name.", "Choose another name."));
        CHECK(fixture.Says(4, "'LOCATION2' is declared twice.", "Rename one of them.",
                           "The first declaration 'Location2' is at line 5; names in one place "
                           "are compared ignoring letter case."));
        CHECK(fixture.Says(5,
                           "'DescribeState' hides the member 'DescribeState' of the base type "
                           "'ScriptComponent'.",
                           "Write 'override fn DescribeState(...)' to override it."));
        CHECK(fixture.Says(6, "'Jump' overrides nothing: no base type has a function 'Jump'.",
                           "Remove 'override' or fix the name."));
        CHECK(fixture.Says(7, "'Actor.Jump' cannot be overridden.",
                           "Remove 'override' and rename the function."));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 찾지 못한 이름의 수정 예: 비슷한 이름, 같은 패키지·의존 패키지의 import, 별칭 이름. 둘 이상의
// 모듈이 선언한 이름은 import를 제안하지 않는다.
TEST(SparksNames_UnknownNameSuggestions) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator);
        REQUIRE(fixture.Schema("Schema/Engine.sparkbind.json", EngineSchema));
        const uint32 game = fixture.SimplePackage("Game", "\"Weapons\"");
        const uint32 weapons = fixture.SimplePackage("Weapons");
        fixture.Module(game, "Game", "Tools.spark", "struct Hammer { }\nstruct Twice { }\n");
        fixture.Module(game, "Game", "Other.spark", "struct Twice { }\n");
        fixture.Module(weapons, "Weapons", "Sword.spark", "struct Sword { }\n");
        fixture.Module(weapons, "Weapons", "Bow.spark", "struct Bow { }\n");
        fixture.Module(
            game, "Game", "Main.spark",
            "import Weapons.Bow as Bows;\n"    // 1
            "class Door : ScriptComponent {\n" // 2
            "    var Speed: int;\n"            // 3
            "    fn Run() {\n"                 // 4
            "        _ = Sped;\n"              // 5 비슷한 멤버
            "        _ = Hammer;\n"            // 6 같은 패키지 import
            "        _ = Sword;\n"             // 7 의존 패키지 import
            "        _ = Bow;\n"               // 8 별칭
            "        _ = Twice;\n"             // 9 두 모듈 — 제안 없음
            "        _ = Wrld;\n"              // 10 문맥 이름 제안
            "    }\n"
            "}\n"
            "struct Holder { var Item: Hamer; }\n"); // 13 import하지 않은 이름은 후보가 아니다
        REQUIRE(fixture.Check());
        constexpr StringView Path = "Game/Main.spark";
        const Expected expected[] = {{C::UnknownName, Path, 5, 13}, {C::UnknownName, Path, 6, 13},
                                     {C::UnknownName, Path, 7, 13}, {C::UnknownName, Path, 8, 13},
                                     {C::UnknownName, Path, 9, 13}, {C::UnknownName, Path, 10, 13},
                                     {C::UnknownType, Path, 13, 27}};
        REQUIRE(fixture.Reports(expected));
        CHECK(fixture.Says(0, "Unknown name 'Sped'.", "Did you mean 'Speed'?",
                           "Names come from this file, its imports, the members of the type, "
                           "the engine and the prelude."));
        CHECK(fixture.Says(1, "Unknown name 'Hammer'.", "Add 'import Tools;'."));
        CHECK(fixture.Says(2, "Unknown name 'Sword'.", "Add 'import Weapons.Sword;'."));
        CHECK(fixture.Says(3, "Unknown name 'Bow'.",
                           "Write 'Bows.Bow'; the module is imported with an alias."));
        // 둘 이상의 모듈이 선언했으므로 import를 제안하지 않고 비슷한 보이는 이름으로 넘어간다.
        CHECK(fixture.Says(4, "Unknown name 'Twice'.", "Did you mean 'Time'?"));
        CHECK(fixture.Says(5, "Unknown name 'Wrld'.", "Did you mean 'World'?"));
        CHECK(fixture.Says(6, "Unknown type 'Hamer'.",
                           "Declare the type or import the module that declares it."));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 문맥 이름·기반·interface·services의 문장이다.
TEST(SparksNames_OtherMessages) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator);
        REQUIRE(fixture.Schema("Schema/Engine.sparkbind.json", EngineSchema));
        const uint32 game = fixture.Package(
            "Game", "{ name: \"Game\", sparks: 1, profile: \"full\", services: [\"Main.Nope\"] }");
        fixture.Module(game, "Game", "Main.spark",
                       "struct Data { fn Get() { _ = World; _ = Caller; } }\n" // 1 S0321 ×2
                       "class A : DataAsset, Data { }\n"                       // 2 S0322
                       "class B : ScriptComponent, Usable { }\n"               // 3 S0324
                       "const C: Array<int, int> = 1;\n"                       // 4 S0320
                       "const D: Map<int> = 1;\n"                              // 5 S0320
                       "const E: Session = 1;\n");                             // 6 S0319
        REQUIRE(fixture.Check());
        constexpr StringView Path = "Game/Main.spark";
        const Expected expected[] = {{C::ContextNameUnavailable, Path, 1, 30},
                                     {C::ContextNameUnavailable, Path, 1, 41},
                                     {C::InvalidBase, Path, 2, 22},
                                     {C::InterfaceNotImplemented, Path, 3, 7},
                                     {C::TypeArgumentCount, Path, 4, 10},
                                     {C::TypeArgumentCount, Path, 5, 10},
                                     {C::NotAType, Path, 6, 10},
                                     {C::InvalidService, "Game/SparksPackage.json5", 1, 56}};
        REQUIRE(fixture.Reports(expected));
        CHECK(fixture.Says(7, "Service 'Main.Nope': module 'Main' has no class 'Nope'.",
                           "Write the module path and the class name, as in "
                           "'Combat.Score.ScoreKeeper'."));
        CHECK(fixture.Says(0, "The context name 'World' is not available here.",
                           "Move the code into a function of a class that has this context "
                           "name, or pass the value in."));
        CHECK(fixture.Says(1, "The context name 'Caller' is not available here.",
                           "Use 'Caller' in an '@rpc(Server)' function.",
                           "'Caller' exists only in the body of a server RPC function of a "
                           "class that has context names."));
        CHECK(fixture.Says(2, "'Data' is not an interface; only the first base is a class.",
                           "Fix the base list, as in 'class Door : ScriptComponent'."));
        CHECK(fixture.Says(3, "Class 'B' does not implement 'Usable.Use'.", "Add the function."));
        CHECK(
            fixture.Says(4, "Type 'Array' takes 1 type argument.", "Write it as in 'Array<int>'."));
        CHECK(fixture.Says(5, "Type 'Map' takes 2 type arguments.",
                           "Write it as in 'Map<string, int>'."));
        CHECK(fixture.Says(6, "'Session' is not a type.", "Write the type of the value instead."));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// Check의 마지막 단계(13.1절 `stage`): Syntax는 import·이름 해석을, Imports는 이름 해석을 하지
// 않는다. Check 뒤에는 바꿀 수 없다.
TEST(SparksNames_Stages) {
    SystemAllocator allocator;
    constexpr StringView Source = "import Nope;\nfn F(): int { return Unknown; }\n";
    {
        Fixture fixture(allocator);
        const uint32 game = fixture.SimplePackage("Game");
        const uint32 main = fixture.Module(game, "Game", "Main.spark", Source);
        REQUIRE(fixture.compilation.SetLastStage(CompilationStage::Syntax));
        REQUIRE(fixture.Check());
        CHECK(fixture.Clean());
        CHECK(fixture.compilation.ModuleTree(main) != nullptr);
        CHECK(fixture.compilation.ModuleImports(main).IsEmpty());
        CHECK(fixture.compilation.Symbols().IsEmpty());
        StringBuilder<64> dump(allocator);
        auto dumped = fixture.compilation.AppendNameDump(dump, main);
        CHECK(!dumped && dumped.Error() == ErrorCode::InvalidArgument);
        CHECK(fixture.compilation.SetLastStage(CompilationStage::All).Error() ==
              ErrorCode::InvalidArgument);
    }
    {
        Fixture fixture(allocator);
        const uint32 game = fixture.SimplePackage("Game");
        const uint32 main = fixture.Module(game, "Game", "Main.spark", Source);
        REQUIRE(fixture.compilation.SetLastStage(CompilationStage::Imports));
        REQUIRE(fixture.Check());
        CHECK(fixture.Reports(C::UnknownModule, "Game/Main.spark", 1, 8));
        CHECK(fixture.compilation.ModuleImports(main).Size() == 1);
        CHECK(fixture.compilation.Symbols().IsEmpty());
    }
    {
        // 기본은 전체 단계다(해석하지 못한 import가 있어 S0317은 내지 않는다).
        Fixture fixture(allocator);
        const uint32 game = fixture.SimplePackage("Game");
        fixture.Module(game, "Game", "Main.spark", Source);
        REQUIRE(fixture.Check());
        CHECK(fixture.Reports(C::UnknownModule, "Game/Main.spark", 1, 8));
        CHECK(!fixture.compilation.Symbols().IsEmpty());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 깊이 상한(S0215)을 넘은 모듈은 이름 해석으로 넘기지 않고, 그 모듈을 import한 모듈은 불완전하다.
TEST(SparksNames_TooDeepModuleIsNotBound) {
    SystemAllocator allocator;
    {
        StringBuilder<4096> deep(allocator);
        deep.Append("const X: int = ");
        for (uint32 index = 0; index <= MaxSyntaxNesting; ++index) {
            deep.Append("(");
        }

        deep.Append("1");
        for (uint32 index = 0; index <= MaxSyntaxNesting; ++index) {
            deep.Append(")");
        }

        deep.Append(";\n");
        Fixture fixture(allocator);
        const uint32 game = fixture.SimplePackage("Game");
        const uint32 deepModule = fixture.Module(game, "Game", "Deep.spark", deep.View());
        fixture.Module(game, "Game", "Main.spark", "import Deep;\nfn F(): int { return X; }\n");
        REQUIRE(fixture.Check());
        REQUIRE(fixture.diagnostics.Count() == 1);
        CHECK(fixture.diagnostics[0].Code() == C::NestingTooDeep);
        StringBuilder<64> dump(allocator);
        auto dumped = fixture.compilation.AppendNameDump(dump, deepModule);
        CHECK(!dumped && dumped.Error() == ErrorCode::InvalidArgument);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 같은 입력이면 심볼·타입·덤프가 바이트 단위로 같다.
TEST(SparksNames_Deterministic) {
    SystemAllocator allocator;
    constexpr StringView Source = "import Tools;\n"
                                  "class Door : ScriptComponent {\n"
                                  "    var Items: Map<string, Array<Hammer?>>;\n"
                                  "    fn Run(count: int) {\n"
                                  "        for (key, list in Items) { _ = key; _ = list; }\n"
                                  "        _ = Nope;\n"
                                  "    }\n"
                                  "}\n";
    StringBuilder<4096> first(allocator);
    StringBuilder<4096> second(allocator);
    for (uint32 round = 0; round < 2; ++round) {
        Fixture fixture(allocator);
        REQUIRE(fixture.Schema("Schema/Engine.sparkbind.json", EngineSchema));
        const uint32 game = fixture.SimplePackage("Game");
        fixture.Module(game, "Game", "Tools.spark", "struct Hammer { }\n");
        const uint32 main = fixture.Module(game, "Game", "Main.spark", Source);
        REQUIRE(fixture.Check());
        StringBuilder<4096>& out = round == 0 ? first : second;
        REQUIRE(fixture.compilation.AppendNameDump(out, main));
        REQUIRE(AppendDiagnostics(out, fixture.sources, fixture.diagnostics));
        out.AppendFormat("{} {}\n", fixture.compilation.Symbols().Size(),
                         fixture.compilation.Types().Size());
    }

    CHECK(first.View() == second.View());
    CHECK(first.View().Contains("Hammer -> ", StringComparison::CaseSensitive) ||
          first.View().Contains("type Array<Game:Tools.Hammer?>", StringComparison::CaseSensitive));
}

// Check 안의 할당 하나하나를 실패시킨다(깨끗한 실행의 할당 수만큼 모두). 실패는 OutOfMemory로만
// 돌아오고, 할당 실패를 삼켜 성공한 경우가 있으면 진단이 깨끗한 실행과 달라 드러난다.
TEST(SparksNames_AllocationFailureEverywhere) {
    SystemAllocator backing;
    uint64 checkAttempts = 0;
    StringBuilder<8192> clean(backing);
    uint64 failures = 0;
    uint64 successes = 0;
    for (uint64 failAt = 0; failAt == 0 || failAt <= checkAttempts; ++failAt) {
        test::NFailAllocator allocator(backing);
        {
            allocator.DisableFailure();
            Fixture fixture(allocator);
            REQUIRE(fixture.Schema("Schema/Engine.sparkbind.json", EngineSchema));
            const uint32 game = fixture.Package(
                "Game", "{ name: \"Game\", sparks: 1, profile: \"full\", services: [\"Main.Keep\", "
                        "\"Main.Door\"], dependencies: [\"Weapons\"] }");
            const uint32 weapons = fixture.SimplePackage("Weapons");
            fixture.Module(weapons, "Weapons", "Sword.spark", "struct Sword { }\n");
            fixture.Module(game, "Game", "Tools.spark", "struct Hammer { }\nfn Helper() { }\n");
            fixture.Module(game, "Game", "Main.spark",
                           "import Tools;\n"
                           "import Weapons.Sword as Blades;\n"
                           "enum Mode { On, Off, OFF }\n"
                           "interface Usable2 { fn Use(); }\n"
                           "open class Base : ScriptComponent { open fn Step(count: int) { } }\n"
                           "class Door : Base, Usable2 {\n"
                           "    var Items: Map<string, Array<Hammer?>>;\n"
                           "    var Blade: Blades.Sword?;\n"
                           "    var Helper: int;\n"
                           "    override fn Step(amount: int) { }\n"
                           "    fn Update() { }\n"
                           "    fn Run(world: int) {\n"
                           "        for (key, list in Items) { _ = key; _ = list; }\n"
                           "        if (let found = Blade) { _ = found as? Actor; }\n"
                           "        _ = Nope;\n"
                           "        _ = Sword;\n"
                           "        _ = World;\n"
                           "    }\n"
                           "}\n"
                           "class Keep : WorldService { fn Run() { _ = Input; } }\n");
            // 이름 해석만 본다(타입 검사의 할당 실패는 SparksTypes 시험이 본다).
            REQUIRE(fixture.compilation.SetLastStage(CompilationStage::Names));
            if (failAt == 0) {
                allocator.DisableFailure(); // 깨끗한 실행: Check의 할당 수를 센다
            } else {
                allocator.FailOnAttempt(failAt);
            }

            auto checked = fixture.compilation.Check(fixture.diagnostics);
            if (failAt == 0) {
                REQUIRE(checked);
                checkAttempts = allocator.Attempts();
                fixture.diagnostics.Sort(fixture.sources);
                REQUIRE(AppendDiagnostics(clean, fixture.sources, fixture.diagnostics));
                if (fixture.diagnostics.Count() != 9) {
                    fixture.Print();
                }

                CHECK(fixture.diagnostics.Count() == 9);
            } else if (checked) {
                ++successes;
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

    CHECK(checkAttempts >= 100);
    CHECK(failures + successes == checkAttempts);
    CHECK(failures >= checkAttempts / 2);
}

} // namespace sparks_names_tests_file
