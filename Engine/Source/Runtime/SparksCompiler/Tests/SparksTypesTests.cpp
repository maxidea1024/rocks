#include <Core/Math/RandomStream.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/StringBuilder.h>
#include <Platform/Fatal.h>
#include <SparksCompiler/Compilation.h>
#include <SparksCompiler/Diagnostic.h>
#include <SparksCompiler/Source.h>
#include <SparksCompiler/SyntaxTree.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

// 타입 검사 시험이다(Docs/Design/SparksLanguage 3.10절, S0326·S0401–S0425·S0501–S0509·S0109).
// 규칙별 양성·음성 사례와 위치, 식 타입은 corpus `Conformance/Types`(`.expect`·`.types`)가 맡고,
// 여기서는 API(ExpressionType·AppendTypeDump·단계), 진단 문장과 설명·수정 예, 결정성, 할당 실패,
// 무작위로 망가뜨린 원문에서의 견고함을 본다.
using namespace rocks;
using namespace rocks::sparks;
namespace sparks_types_tests_file {

namespace {

using C = DiagnosticCode;

class Fixture final {
public:
    explicit Fixture(Allocator& allocator) noexcept
        : allocator_(allocator), sources(allocator), diagnostics(allocator),
          compilation(allocator, sources) {
    }

    uint32 Package(StringView name) noexcept {
        StringBuilder<128> path(allocator_);
        path.Append(name).Append("/SparksPackage.json5");
        StringBuilder<128> manifest(allocator_);
        manifest.Append("{ name: \"").Append(name).Append("\", sparks: 1, profile: \"full\" }");
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

    // 덤프(또는 그 실패)를 기대값과 비교한다.
    bool Dumps(uint32 module, StringView expected) noexcept {
        StringBuilder<1024> dump(allocator_);
        if (!compilation.AppendTypeDump(dump, module) || dump.View() != expected) {
            platform::WriteDiagnostic(dump.View().Data(), dump.ByteCount());
            return false;
        }

        return true;
    }

    // 모듈 트리에서 이 종류의 노드 중 노드 번호 순으로 ordinal번째(0부터)다.
    uint32 FindNode(uint32 module, SyntaxKind kind, uint32 ordinal = 0) const noexcept {
        const SyntaxTree* tree = compilation.ModuleTree(module);
        for (uint32 node = 0; tree != nullptr && node < tree->NodeCount(); ++node) {
            if (tree->Node(node).kind == kind && ordinal-- == 0) {
                return node;
            }
        }

        return NoNode;
    }

    bool TypeText(uint32 type, StringView expected) noexcept {
        StringBuilder<256> text(allocator_);
        return compilation.AppendTypeText(text, type) && text.View() == expected;
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

// 초기값 깊이 상한(TypeChecker의 MaxInitializerDepth, 3.10절 S0427)이다.
constexpr uint32 MaxInitializerDepthForTest = 64;

// 시험용 엔진 schema다(한 줄에 타입 하나).
constexpr StringView EngineSchema =
    R"({ "format": 1, "package": "Rocks", "module": "Engine", "types": [
{ "name": "Actor", "id": "5a7e0000000000000000000000000001", "kind": "object", "scriptExtensible": true, "fields": [ { "name": "Location", "id": "5a7e0000000000000000000000000101", "type": "vec3", "access": "readWrite" } ], "functions": [ { "name": "Find", "id": "5a7e0000000000000000000000000102", "typeParameters": [ { "name": "T", "constraint": "Component" } ], "returns": "T?" }, { "name": "MoveTo", "id": "5a7e0000000000000000000000000103", "async": true, "parameters": [ { "name": "target", "type": "vec3" } ], "returns": "bool" } ], "events": [ { "name": "Destroyed", "id": "5a7e0000000000000000000000000104" } ] },
{ "name": "Pawn", "id": "5a7e0000000000000000000000000009", "kind": "object", "base": "Actor", "scriptExtensible": true },
{ "name": "Component", "id": "5a7e0000000000000000000000000002", "kind": "object", "fields": [ { "name": "Owner", "id": "5a7e0000000000000000000000000201", "type": "Actor" } ] },
{ "name": "ScriptableComponent", "id": "5a7e0000000000000000000000000003", "kind": "object", "base": "Component", "scriptExtensible": true },
{ "name": "WorldService", "id": "5a7e0000000000000000000000000004", "kind": "object", "scriptExtensible": true },
{ "name": "DataAsset", "id": "5a7e0000000000000000000000000005", "kind": "asset", "scriptExtensible": true },
{ "name": "World", "id": "5a7e0000000000000000000000000006", "kind": "object" },
{ "name": "Error", "id": "5a7e0000000000000000000000000008", "kind": "enum", "cases": [ "NotFound", "Busy" ] }
] })";

// 타입 검사의 대부분 경로를 지나는 프로그램이다(진단 넷: S0401, S0507, S0415, S0404).
constexpr StringView BusyProgram =
    "enum Mode { A, B }\n"
    "struct Point { var X: int; var Y: int = 2; fn Sum(): int { return X + Y; } }\n"
    "struct Req { var First: Tag; }\n"
    "const Limit: int = 10;\n"
    "open class Base : ScriptableComponent { var Health = 10; event Struck(amount: int); }\n"
    "class Door : Base {\n"
    "    var Items: Map<string, Array<int>> = [\"a\": [1, 2]];\n"
    "    var Target: Actor?;\n"
    "    let Range = 2.5;\n"
    "    fn Run(count: int, mode: Mode, soft: Soft<DataAsset>): Result<int> {\n"
    "        for (key, list in Items) { _ = \"${key} ${list.Count}\"; }\n"
    "        for (i in 0..<count) { Health += i % 3; }\n"
    "        if (let found = Target) { _ = found.Find<ScriptableComponent>()?.Owner; }\n"
    "        let p = Point(X: Math.Max(count, 1));\n"
    "        let s: string = count;\n"
    "        _ = count + Range;\n"
    "        switch (mode) { case Mode.A: { } }\n"
    "        _ = Req();\n"
    "        emit Struck(p.Sum());\n"
    "        _ = vec3(1.0, 2.0, 3.0) * Range;\n"
    "        _ = [Limit, 2, 3];\n"
    "        _ = Target as? Pawn;\n"
    "        _ = s;\n"
    "        if (count > Limit) { return Error.Busy; }\n"
    "        return Items[\"a\"]?.Count ?? 0;\n"
    "    }\n"
    "    async fn Wait(a: Actor) { let moved = await a.MoveTo(a.Location); _ = moved; "
    "await a.Destroyed; }\n"
    "}\n";

// 검증 리터럴·`as?`·`await`·`try`·switch·컬렉션·보간을 쓰는 프로그램이다(진단 없음).
constexpr StringView LiteralProgram =
    "enum Kind { Small, Large }\n"
    "struct Row2 { var Name: string = \"\"; }\n"
    "class Mover : ScriptableComponent {\n"
    "    var Scores: Map<Kind, float> = [];\n"
    "    async fn Load(soft: Soft<DataAsset>): Result<void> {\n"
    "        let asset = try await soft.Load();\n"
    "        let tag = Tag(\"Status.Fire\");\n"
    "        let text = Loc(\"Hud.Count\", Count: 3, Name: \"x\");\n"
    "        let row: Row<Row2>? = Row(\"Sword\");\n"
    "        let data: DataAsset? = Asset(\"/Game/Data\");\n"
    "        _ = asset; _ = tag; _ = text; _ = row; _ = data;\n"
    "        for (kind, score in Scores) {\n"
    "            switch (kind) {\n"
    "                case Kind.Small: { _ = \"${score * 2.0}\"; }\n"
    "                case Kind.Large: { _ = Math.Clamp(score, 0.0, 1.0); }\n"
    "            }\n"
    "        }\n"
    "        let owner = Owner as? Pawn;\n"
    "        _ = owner?.Location ?? vec3(0.0, 0.0, 0.0);\n"
    "        await Delay(0.5, Clock.Game);\n"
    "        return;\n"
    "    }\n"
    "}\n";

} // namespace

// 식 타입 덤프와 ExpressionType: 기대 타입에 맞춘 뒤의 타입, 바깥 식 먼저, 값이 아닌 식은 없다.
TEST(SparksTypes_TypeDumpAndExpressionType) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator);
        const uint32 game = fixture.Package("Game");
        const uint32 main = fixture.Module(game, "Game", "Main.spark",
                                           "fn F(a: int): float {\n"
                                           "    return float(a) * 2;\n"
                                           "}\n");
        REQUIRE(fixture.Check());
        CHECK(fixture.diagnostics.Count() == 0);
        CHECK(fixture.Dumps(main, "2:12-2:24 Binary float\n"
                                  "2:12-2:20 Call float\n"
                                  "2:18-2:19 Identifier int\n"
                                  "2:23-2:24 IntegerLiteral float\n"));
        // 노드 번호 순: 변환 대상 `float`(타입 이름 — 값이 아니다), 인자 `a`.
        const uint32 binary = fixture.FindNode(main, SyntaxKind::Binary);
        const uint32 callee = fixture.FindNode(main, SyntaxKind::Identifier, 0);
        const uint32 argument = fixture.FindNode(main, SyntaxKind::Identifier, 1);
        const uint32 parameter = fixture.FindNode(main, SyntaxKind::Parameter);
        REQUIRE(binary != NoNode);
        REQUIRE(argument != NoNode);
        REQUIRE(parameter != NoNode);
        CHECK(fixture.TypeText(fixture.compilation.ExpressionType(main, binary), "float"));
        CHECK(fixture.compilation.ExpressionType(main, callee) == NoType);
        CHECK(fixture.TypeText(fixture.compilation.ExpressionType(main, argument), "int"));
        CHECK(fixture.compilation.ExpressionType(main, parameter) == NoType);
        CHECK(fixture.compilation.ExpressionType(main, 1000000) == NoType);
        CHECK(fixture.compilation.ExpressionType(1000, 0) == NoType);
        StringBuilder<64> dump(allocator);
        auto dumped = fixture.compilation.AppendTypeDump(dump, 1000);
        CHECK(!dumped && dumped.Error() == ErrorCode::InvalidArgument);
        CHECK(dump.View().IsEmpty());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 단계: Names에서 멈추면 타입 검사를 하지 않는다(진단 없음, 덤프는 InvalidArgument, 식 타입 없음).
TEST(SparksTypes_Stages) {
    SystemAllocator allocator;
    constexpr StringView Source = "fn F(): int {\n    return \"a\";\n}\n";
    {
        Fixture fixture(allocator);
        const uint32 game = fixture.Package("Game");
        const uint32 main = fixture.Module(game, "Game", "Main.spark", Source);
        REQUIRE(fixture.compilation.SetLastStage(CompilationStage::Names));
        REQUIRE(fixture.Check());
        CHECK(fixture.diagnostics.Count() == 0);
        StringBuilder<64> dump(allocator);
        auto dumped = fixture.compilation.AppendTypeDump(dump, main);
        CHECK(!dumped && dumped.Error() == ErrorCode::InvalidArgument);
        const SyntaxTree* tree = fixture.compilation.ModuleTree(main);
        REQUIRE(tree != nullptr);
        for (uint32 node = 0; node < tree->NodeCount(); ++node) {
            CHECK(fixture.compilation.ExpressionType(main, node) == NoType);
        }
    }
    {
        Fixture fixture(allocator);
        const uint32 game = fixture.Package("Game");
        const uint32 main = fixture.Module(game, "Game", "Main.spark", Source);
        REQUIRE(fixture.compilation.SetLastStage(CompilationStage::Types));
        REQUIRE(fixture.Check());
        CHECK(fixture.Says(
            0, C::TypeMismatch, 2, 12, "A value of type 'string' cannot be used as 'int'.",
            "Use a value of that type.", "The value must have that type or be derived from it."));
        CHECK(fixture.Dumps(main, "2:12-2:15 String string\n"));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 진단 문장·설명·수정 예다(코드·위치와 함께). 타입은 선언 이름으로 보인다.
TEST(SparksTypes_Messages) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator);
        const uint32 game = fixture.Package("Game");
        fixture.Module(game, "Game", "Main.spark",
                       "enum Mode { A, B, C }\n"                                   // 1
                       "struct Req { var First: Tag; var Second: InputAction; }\n" // 2
                       "fn F(i: int, f: float, mode: Mode, opt: int?) {\n"         // 3
                       "    let a: string = i;\n"                                  // 4 S0401
                       "    _ = i + f;\n"                                          // 5 S0507
                       "    switch (mode) {\n"                                     // 6 S0415
                       "        case Mode.A: { }\n"                                // 7
                       "    }\n"                                                   // 8
                       "    _ = Req();\n"                                          // 9 S0404
                       "    _ = opt + 1;\n"                                        // 10 S0406
                       "    _ = 7 / 2;\n"                                          // 11 S0501
                       "    let b: uint8 = 300;\n"                                 // 12 S0109
                       "    _ = \"${opt}\";\n"                                     // 13 S0419
                       "    _ = Math.Div(i, 0);\n"                                 // 14 S0504
                       "    _ = mode.A;\n"                                         // 15 S0425
                       "    _ = a; _ = b;\n"                                       // 16
                       "}\n");
        REQUIRE(fixture.Check());
        REQUIRE(fixture.diagnostics.Count() == 10);
        CHECK(fixture.Says(0, C::TypeMismatch, 4, 21,
                           "A value of type 'int' cannot be used as 'string'.",
                           "Use a value of that type."));
        CHECK(fixture.Says(1, C::MixedNumericTypes, 5, 9, "'+' mixes 'int' and 'float'.",
                           "Convert one side, as in 'float(x)'.",
                           "Numbers of different types are never mixed silently."));
        CHECK(fixture.Says(2, C::SwitchCoverage, 6, 13, "The switch does not handle B, C.",
                           "Add the missing cases.", "A switch on an enum handles every case."));
        CHECK(fixture.Says(3, C::ArgumentForm, 9, 9, "Fields 'First', 'Second' must be given.",
                           "Pass it, as in 'Field: value'."));
        CHECK(fixture.Says(4, C::InvalidOperator, 10, 9, "'+' is not defined for 'int?' and 'int'.",
                           "Use '?? value' or 'if (let x = ...)' first.",
                           "A value whose type has '?' may be null."));
        CHECK(fixture.Says(5, C::IntegerDivision, 11, 9, "Integers are not divided with '/'.",
                           "Use 'Math.Div(a, b)' for the quotient or 'float(a) / float(b)'.",
                           "Integer division drops the fraction, which is easy to miss."));
        CHECK(fixture.Says(6, C::IntegerLiteralTooLarge, 12, 20,
                           "The constant does not fit in 'uint8'.",
                           "Use a value in range or a wider type.",
                           "A constant must fit in the type of its place."));
        CHECK(fixture.Says(7, C::InterpolationType, 13, 12,
                           "A value of type 'int?' cannot be interpolated.",
                           "Interpolate a field of it or check it for null first."));
        CHECK(fixture.Says(8, C::ConstantDivisionByZero, 14, 21, "The constant is divided by zero.",
                           "Use a value other than 0.", "Dividing by zero always fails."));
        CHECK(fixture.Says(9, C::StaticInstanceMismatch, 15, 9,
                           "'A' belongs to the type, not to a value.", "Write 'Mode.A'.",
                           "Enum cases and static functions are used through the type name."));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// schema struct가 값으로 서로를 담아도(생성된 schema에는 없는 손상) 기본값 판정이 끝나고 기본값이
// 있는 것으로 본다(연쇄 방지). 엔진 struct의 기본값은 schema 필드로 정한다(Tag 필드는 기본값이
// 없다).
TEST(SparksTypes_EngineStructDefaults) {
    SystemAllocator allocator;
    constexpr StringView Schema =
        R"({ "format": 1, "package": "Rocks", "module": "Engine", "types": [
{ "name": "Ring", "id": "5a7e0000000000000000000000000001", "kind": "struct", "fields": [ { "name": "Next", "id": "5a7e0000000000000000000000000101", "type": "Link" } ] },
{ "name": "Link", "id": "5a7e0000000000000000000000000002", "kind": "struct", "fields": [ { "name": "Back", "id": "5a7e0000000000000000000000000201", "type": "Ring" } ] },
{ "name": "Labeled", "id": "5a7e0000000000000000000000000003", "kind": "struct", "fields": [ { "name": "Label", "id": "5a7e0000000000000000000000000301", "type": "Tag" }, { "name": "Count", "id": "5a7e0000000000000000000000000302", "type": "int" } ] },
{ "name": "Wraps", "id": "5a7e0000000000000000000000000004", "kind": "struct", "fields": [ { "name": "Inner", "id": "5a7e0000000000000000000000000401", "type": "Labeled" } ] },
{ "name": "Error", "id": "5a7e0000000000000000000000000005", "kind": "enum", "cases": [ "NotFound" ] }
] })";
    {
        Fixture fixture(allocator);
        REQUIRE(fixture.Schema(Schema));
        const uint32 game = fixture.Package("Game");
        fixture.Module(game, "Game", "Main.spark",
                       "fn F() {\n"
                       "    _ = Ring();\n"
                       "    _ = Link();\n"
                       "    _ = Labeled(Count: 1);\n"
                       "    _ = Wraps();\n"
                       "}\n");
        REQUIRE(fixture.Check());
        REQUIRE(fixture.diagnostics.Count() == 2);
        CHECK(fixture.Says(0, C::ArgumentForm, 4, 9, "Field 'Label' must be given.",
                           "Pass it, as in 'Field: value'."));
        CHECK(fixture.Says(1, C::ArgumentForm, 5, 9, "Field 'Inner' must be given.",
                           "Pass it, as in 'Field: value'."));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 두 번째 시험 프로그램은 진단이 없다(퍼즈의 출발점이 올바른 프로그램이다).
TEST(SparksTypes_LiteralProgramIsClean) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator);
        REQUIRE(fixture.Schema(EngineSchema));
        const uint32 game = fixture.Package("Game");
        fixture.Module(game, "Game", "Main.spark", LiteralProgram);
        REQUIRE(fixture.Check());
        if (fixture.diagnostics.Count() != 0) {
            fixture.Print();
        }

        CHECK(fixture.diagnostics.Count() == 0);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// S0326은 비슷한 멤버 이름(편집 거리 2 이하)을 제시하고 없으면 이름을 확인하라고 한다.
TEST(SparksTypes_MemberSuggestions) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator);
        const uint32 game = fixture.Package("Game");
        fixture.Module(game, "Game", "Main.spark",
                       "struct Point { var Width: int; }\n" // 1
                       "fn F(v: vec3, p: Point) {\n"        // 2
                       "    _ = v.Lenght();\n"              // 3
                       "    _ = Math.Sqr(2.0);\n"           // 4
                       "    _ = p.Widht;\n"                 // 5
                       "    _ = p.Completely;\n"            // 6
                       "}\n");
        REQUIRE(fixture.Check());
        REQUIRE(fixture.diagnostics.Count() == 4);
        CHECK(fixture.Says(0, C::UnknownMember, 3, 9, "'vec3' has no member 'Lenght'.",
                           "Did you mean 'Length'?"));
        CHECK(fixture.Says(1, C::UnknownMember, 4, 9, "'Math' has no member 'Sqr'.",
                           "Did you mean 'Sqrt'?"));
        CHECK(fixture.Says(2, C::UnknownMember, 5, 9, "'Point' has no member 'Widht'.",
                           "Did you mean 'Width'?"));
        CHECK(fixture.Says(3, C::UnknownMember, 6, 9, "'Point' has no member 'Completely'.",
                           "Check the member name."));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 깊이 상한 근처의 긴 식과 긴 초기값 연쇄도 stack을 넘치지 않는다: 1000항 `+`(타입 있는 값·상수),
// 역순으로 선언한 const 5000개의 이름 연쇄(미리 정하므로 깊이를 쓰지 않는다 — S0427 없음).
TEST(SparksTypes_DeepChainsStayWithinTheStack) {
    SystemAllocator allocator;
    {
        constexpr uint32 Terms = 1000;
        StringBuilder<16384> source(allocator);
        source.Append("fn F(x: int): int {\n    return x");
        for (uint32 index = 1; index < Terms; ++index) {
            source.Append(" + x");
        }

        source.Append(";\n}\nconst K: int = 1");
        for (uint32 index = 1; index < Terms; ++index) {
            source.Append(" + 1");
        }

        source.Append(";\n");
        Fixture fixture(allocator);
        const uint32 game = fixture.Package("Game");
        fixture.Module(game, "Game", "Main.spark", source.View());
        REQUIRE(fixture.Check());
        if (fixture.diagnostics.Count() != 0) {
            fixture.Print();
        }

        CHECK(fixture.diagnostics.Count() == 0);
    }
    {
        // 왼쪽으로 깊은 그 밖의 연쇄: `&&`, 멤버·호출, 인덱스, `as?`(진단 수는 보지 않고 끝나는지
        // 본다).
        constexpr uint32 Steps = 480;
        StringBuilder<32768> source(allocator);
        source.Append("fn H(flag: bool, v: vec3, items: Array<int>, a: Actor) {\n    _ = flag");
        for (uint32 index = 0; index < 2 * Steps; ++index) {
            source.Append(" && flag");
        }

        source.Append(";\n    _ = v");
        for (uint32 index = 0; index < Steps; ++index) {
            source.Append(".Normalize()");
        }

        source.Append(";\n    _ = items");
        for (uint32 index = 0; index < 2 * Steps; ++index) {
            source.Append("[0]");
        }

        source.Append(";\n    _ = a");
        for (uint32 index = 0; index < 2 * Steps; ++index) {
            source.Append(" as? Pawn");
        }

        source.Append(";\n}\n");
        Fixture fixture(allocator);
        REQUIRE(fixture.Schema(EngineSchema));
        const uint32 game = fixture.Package("Game");
        fixture.Module(game, "Game", "Main.spark", source.View());
        REQUIRE(fixture.Check());
        for (usize index = 0; index < fixture.diagnostics.Count(); ++index) {
            CHECK(fixture.diagnostics[index].Code() != C::NestingTooDeep);
        }
    }
    {
        constexpr uint32 Constants = 5000;
        StringBuilder<16384> source(allocator);
        for (uint32 index = 0; index + 1 < Constants; ++index) {
            source.AppendFormat("const C{}: int = C{} + 1;\n", index, index + 1);
        }

        source.AppendFormat("const C{}: int = 0;\n", Constants - 1);
        source.Append("fn G(i: int) {\n    switch (i) {\n        case C0: { }\n"
                      "        case C1: { }\n        default: { }\n    }\n}\n");
        Fixture fixture(allocator);
        const uint32 game = fixture.Package("Game");
        fixture.Module(game, "Game", "Main.spark", source.View());
        REQUIRE(fixture.Check());
        if (fixture.diagnostics.Count() != 0) {
            fixture.Print();
        }

        CHECK(fixture.diagnostics.Count() == 0);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 이름이 아닌 길(값의 멤버)로 이어진 초기값 연쇄가 상한(64)을 넘으면 S0427 하나다.
TEST(SparksTypes_InitializerChainLimit) {
    SystemAllocator allocator;
    {
        constexpr uint32 Structs = MaxInitializerDepthForTest + 6;
        StringBuilder<16384> source(allocator);
        for (uint32 index = 0; index + 1 < Structs; ++index) {
            source.AppendFormat("struct S{} {{ var A = S{}().A; }}\n", index, index + 1);
        }

        source.AppendFormat("struct S{} {{ var A: int = 1; }}\n", Structs - 1);
        Fixture fixture(allocator);
        const uint32 game = fixture.Package("Game");
        fixture.Module(game, "Game", "Main.spark", source.View());
        // 타입 단계만 본다(멤버 접근 초기값은 LC2d에서 S0510도 받는다).
        REQUIRE(fixture.compilation.SetLastStage(CompilationStage::Types));
        REQUIRE(fixture.Check());
        REQUIRE(fixture.diagnostics.Count() == 1);
        CHECK(fixture.diagnostics[0].Code() == C::InitializerTooDeep);
        CHECK(fixture.diagnostics[0].Message() ==
              StringView("This initializer depends on a chain of more than 64 initializers."));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 같은 입력이면 진단·타입 덤프·타입 표가 바이트 단위로 같다.
TEST(SparksTypes_Deterministic) {
    SystemAllocator allocator;
    StringBuilder<8192> first(allocator);
    StringBuilder<8192> second(allocator);
    for (uint32 round = 0; round < 2; ++round) {
        Fixture fixture(allocator);
        REQUIRE(fixture.Schema(EngineSchema));
        const uint32 game = fixture.Package("Game");
        const uint32 main = fixture.Module(game, "Game", "Main.spark", BusyProgram);
        REQUIRE(fixture.Check());
        StringBuilder<8192>& out = round == 0 ? first : second;
        REQUIRE(fixture.compilation.AppendTypeDump(out, main));
        REQUIRE(AppendDiagnostics(out, fixture.sources, fixture.diagnostics));
        out.AppendFormat("{} {}\n", fixture.compilation.Symbols().Size(),
                         fixture.compilation.Types().Size());
        // 타입 진단 넷, 흐름 진단 둘(S0712 soft, await 뒤 `a.Destroyed`의 S0412), 기반 class
        // 이벤트의 emit(S0811)이다.
        if (round == 0 && fixture.diagnostics.Count() != 7) {
            fixture.Print();
        }

        CHECK(fixture.diagnostics.Count() == 7);
    }

    CHECK(first.View() == second.View());
    CHECK(first.View().Contains("Binary bool", StringComparison::CaseSensitive));
    CHECK(first.View().Contains("Await bool", StringComparison::CaseSensitive));
}

// Check 안의 할당 하나하나를 실패시킨다(깨끗한 실행의 할당 수만큼 모두). 실패는 OutOfMemory로만
// 돌아오고, 할당 실패를 삼켜 성공한 경우가 있으면 진단·덤프가 깨끗한 실행과 달라 드러난다.
TEST(SparksTypes_AllocationFailureEverywhere) {
    SystemAllocator backing;
    uint64 checkAttempts = 0;
    StringBuilder<16384> clean(backing);
    uint64 failures = 0;
    uint64 successes = 0;
    for (uint64 failAt = 0; failAt == 0 || failAt <= checkAttempts; ++failAt) {
        test::NFailAllocator allocator(backing);
        {
            allocator.DisableFailure();
            Fixture fixture(allocator);
            REQUIRE(fixture.Schema(EngineSchema));
            const uint32 game = fixture.Package("Game");
            const uint32 main = fixture.Module(game, "Game", "Main.spark", BusyProgram);
            if (failAt == 0) {
                allocator.DisableFailure(); // 깨끗한 실행: Check의 할당 수를 센다
            } else {
                allocator.FailOnAttempt(failAt);
            }

            auto checked = fixture.compilation.Check(fixture.diagnostics);
            const uint64 attempts = allocator.Attempts();
            allocator.DisableFailure(); // 덤프·진단 문장 만들기는 실패시키지 않는다
            if (failAt == 0) {
                REQUIRE(checked);
                checkAttempts = attempts;
                fixture.diagnostics.Sort(fixture.sources);
                REQUIRE(AppendDiagnostics(clean, fixture.sources, fixture.diagnostics));
                REQUIRE(fixture.compilation.AppendTypeDump(clean, main));
            } else if (checked) {
                ++successes;
                fixture.diagnostics.Sort(fixture.sources);
                StringBuilder<16384> text(backing);
                REQUIRE(AppendDiagnostics(text, fixture.sources, fixture.diagnostics));
                REQUIRE(fixture.compilation.AppendTypeDump(text, main));
                CHECK(text.View() == clean.View());
            } else {
                ++failures;
                CHECK(checked.Error() == ErrorCode::OutOfMemory);
            }
        }
        CHECK(backing.Stats().liveAllocations == 0);
    }

    if (checkAttempts < 200) {
        StringBuilder<64> count(backing);
        count.AppendFormat("check attempts: {}\n", checkAttempts);
        platform::WriteDiagnostic(count.View().Data(), count.ByteCount());
    }

    CHECK(checkAttempts >= 200);
    CHECK(failures + successes == checkAttempts);
    CHECK(failures >= checkAttempts / 2);
}

// 올바른 프로그램 둘에서 byte 구간을 지우거나 조각을 끼워 넣기를 1–3번 해서 타입 검사까지 돌린다:
// 실패 없이 끝나고(할당 실패가 아니면 Check는 성공), 같은 원문이면 진단·덤프가 같다.
TEST(SparksTypes_RandomMutations) {
    SystemAllocator allocator;
    {
        constexpr StringView Pieces[] = {
            "?",       "null",   "1.5",    "\"a\"",  "this",        "as? Actor", "[",     "]",
            "(",       ")",      ".",      "+",      "==",          "Missing",   "<int>", "await ",
            "try ",    "?? ",    "?.",     "-",      "/",           "%",         "!",     "Mode.A",
            "0..<",    "${x}",   "Point(", "X: ",    "vec3(",       "int(",      ",",     ": int",
            "Result<", "Array<", "void",   "return", "emit Struck", "Math.Min(", "{",     "}",
        };
        constexpr usize PieceCount = sizeof(Pieces) / sizeof(Pieces[0]);
        constexpr usize RunsPerProgram = 800;
        constexpr uint32 MaxCut = 8;
        constexpr uint32 MaxEdits = 3;
        RandomStream random(20261121);
        const StringView programs[] = {BusyProgram, LiteralProgram};
        for (usize run = 0; run < RunsPerProgram * 2; ++run) {
            StringBuilder<4096> source(allocator);
            source.Append(programs[run % 2]);
            const uint32 edits = 1 + random.NextUInt32() % MaxEdits;
            for (uint32 edit = 0; edit < edits; ++edit) {
                StringBuilder<4096> next(allocator);
                const StringView current = source.View();
                const usize at = random.NextUInt32() % (current.ByteCount() + 1);
                next.Append(StringView(current.Data(), at));
                usize resume = at;
                if ((random.NextUInt32() & 1u) == 0) {
                    const usize cut = 1 + random.NextUInt32() % MaxCut;
                    resume = at + cut < current.ByteCount() ? at + cut : current.ByteCount();
                } else {
                    next.Append(" ").Append(Pieces[random.NextUInt32() % PieceCount]).Append(" ");
                }

                next.Append(StringView(current.Data() + resume, current.ByteCount() - resume));
                source.Clear();
                source.Append(next.View());
            }

            StringBuilder<8192> texts[2] = {StringBuilder<8192>(allocator),
                                            StringBuilder<8192>(allocator)};
            for (StringBuilder<8192>& text : texts) {
                Fixture fixture(allocator);
                REQUIRE(fixture.Schema(EngineSchema));
                const uint32 game = fixture.Package("Game");
                const uint32 main = fixture.Module(game, "Game", "Main.spark", source.View());
                REQUIRE(main != NoModule);
                REQUIRE(fixture.Check());
                REQUIRE(AppendDiagnostics(text, fixture.sources, fixture.diagnostics));
                StringBuilder<8192> dump(allocator);
                auto dumped = fixture.compilation.AppendTypeDump(dump, main);
                CHECK(dumped || dumped.Error() == ErrorCode::InvalidArgument);
                text.Append(dump.View());
            }

            if (texts[0].View() != texts[1].View()) {
                platform::WriteDiagnostic(source.View().Data(), source.ByteCount());
                CHECK(false);
                break;
            }
        }
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

} // namespace sparks_types_tests_file
