#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/StringBuilder.h>
#include <Platform/Fatal.h>
#include <SparksCompiler/Compilation.h>
#include <SparksCompiler/Diagnostic.h>
#include <SparksCompiler/Source.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

// 컴파일 단위 시험이다(Docs/Design/SparksLanguage 3.4절 모듈 경로·import 해석·패키지 의존,
// 10.2절 schema, S0301–S0305, S1106–S1112, S1120). 진단 경로는 "<패키지>/<모듈 경로>"와
// "<패키지>/SparksPackage.json5"다.
using namespace rocks;
using namespace rocks::sparks;
namespace sparks_compilation_tests_file {

namespace {

using C = DiagnosticCode;

struct Expected {
    DiagnosticCode code;
    StringView path;
    uint32 line = 0; // 0이면 파일 전체 진단
    uint32 column = 0;
};

class Fixture final {
public:
    explicit Fixture(Allocator& allocator) noexcept
        : allocator_(allocator), sources(allocator), diagnostics(allocator),
          compilation(allocator, sources) {
    }

    // name 패키지를 manifest 원문으로 더한다.
    uint32 Package(StringView name, StringView manifest) noexcept {
        StringBuilder<128> path(allocator_);
        path.Append(name).Append("/SparksPackage.json5");
        auto id = sources.TryAdd(path.View(), manifest, diagnostics);
        if (!id) {
            return NoPackage;
        }

        auto package = compilation.AddPackage(id.Value());
        return package ? package.Value() : NoPackage;
    }

    // 흔한 manifest(이름·core·의존 목록)다.
    uint32 SimplePackage(StringView name, StringView dependencies = StringView()) noexcept {
        StringBuilder<256> manifest(allocator_);
        manifest.Append("{ name: \"").Append(name).Append("\", sparks: 1, profile: \"core\"");
        if (!dependencies.IsEmpty()) {
            manifest.Append(", dependencies: [").Append(dependencies).Append("]");
        }

        manifest.Append(" }");
        return Package(name, manifest.View());
    }

    uint32 Module(uint32 package, StringView packageName, StringView path,
                  StringView text = StringView()) noexcept {
        StringBuilder<128> display(allocator_);
        display.Append(packageName).Append("/").Append(path);
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

    // 진단이 정확히 이것뿐인지(순서·코드·경로·위치) 본다.
    bool Reports(Span<const Expected> expected) noexcept {
        bool same = diagnostics.Count() == expected.Size();
        for (usize index = 0; same && index < expected.Size(); ++index) {
            const Diagnostic& actual = diagnostics[index];
            const SourceFile& file = sources.Get(actual.Source());
            same = actual.Code() == expected[index].code && file.Path() == expected[index].path;
            if (same && expected[index].line == 0) {
                same = !actual.HasSpan();
            } else if (same) {
                const sparks::SourceLocation location = file.Locate(actual.Span().begin);
                same = actual.HasSpan() && location.line == expected[index].line &&
                       location.column == expected[index].column;
            }
        }

        if (!same) {
            Print();
        }

        return same;
    }

    bool Clean() noexcept {
        return Reports(Span<const Expected>());
    }

    bool Reports(DiagnosticCode code, StringView path, uint32 line = 0,
                 uint32 column = 0) noexcept {
        const Expected expected[] = {{code, path, line, column}};
        return Reports(expected);
    }

    StringView Message(usize index) const noexcept {
        return diagnostics[index].Message();
    }

    StringView Fix(usize index) const noexcept {
        return diagnostics[index].Fix();
    }

    void Print() const noexcept {
        StringBuilder<1024> out(allocator_);
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

constexpr StringView EngineSchema = R"({ "format": 1, "package": "Rocks", "module": "Engine",
  "types": [ { "name": "Actor", "id": "5a7e0000000000000000000000000001", "kind": "object" } ] })";

} // namespace

TEST(SparksCompilation_ResolvesImportsAcrossPackages) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator);
        REQUIRE(fixture.Schema("Schema/Engine.sparkbind.json", EngineSchema));
        const uint32 game = fixture.SimplePackage("Game", "\"Weapons\"");
        const uint32 weapons = fixture.SimplePackage("Weapons");
        const uint32 main = fixture.Module(game, "Game", "Main.spark",
                                           "import Combat.Score;\nimport Weapons.Sword as Blades;\n"
                                           "import Weapons.Arms.Bow;\n");
        const uint32 score = fixture.Module(game, "Game", "Combat/Score.spark", "import Main;\n");
        const uint32 sword = fixture.Module(weapons, "Weapons", "Sword.spark");
        const uint32 bow = fixture.Module(weapons, "Weapons", "Arms/Bow.spark");
        // import 단계만 본다(쓰지 않는 import S0714는 Flow 단계 — Conformance/Flow).
        REQUIRE(fixture.compilation.SetLastStage(CompilationStage::Imports));
        REQUIRE(fixture.Check());
        CHECK(fixture.Clean());
        CHECK(fixture.compilation.SchemaCount() == 1);
        CHECK(fixture.compilation.SchemaIsValid(0));
        CHECK(fixture.compilation.PackageCount() == 2);
        CHECK(fixture.compilation.PackageIsValid(game));
        CHECK(fixture.compilation.FindPackage("Weapons") == weapons);
        CHECK(fixture.compilation.Manifest(weapons).Name() == StringView("Weapons"));
        CHECK(fixture.compilation.ModuleSource(sword).index == 5);
        CHECK(fixture.compilation.FindPackage("Missing") == NoPackage);
        CHECK(fixture.compilation.ModuleImportPath(score) == StringView("Combat.Score"));
        CHECK(fixture.compilation.ModulePath(score) == StringView("Combat/Score.spark"));
        CHECK(fixture.compilation.ModulePackage(bow) == weapons);
        CHECK(fixture.compilation.FindModule(weapons, "Arms.Bow") == bow);
        CHECK(fixture.compilation.FindModule(weapons, "Arms") == NoModule);
        CHECK(fixture.compilation.FindModule(game, "Sword") == NoModule);
        REQUIRE(fixture.compilation.ModuleTree(main) != nullptr);
        const Span<const ImportResolution> imports = fixture.compilation.ModuleImports(main);
        REQUIRE(imports.Size() == 3);
        CHECK(imports[0].module == score);
        CHECK(imports[1].module == sword);
        CHECK(imports[2].module == bow);
        CHECK(fixture.compilation.ModuleTree(main)->Node(imports[1].node).kind ==
              SyntaxKind::Import);
        // 파일 사이 순환 import는 허용한다(3.4절).
        REQUIRE(fixture.compilation.ModuleImports(score).Size() == 1);
        CHECK(fixture.compilation.ModuleImports(score)[0].module == main);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SparksCompilation_ModulePathsS1108) {
    constexpr StringView Invalid[] = {"main.spark",
                                      "Combat/score.spark",
                                      "Combat/Sc_ore.spark",
                                      "Score.txt",
                                      "Score",
                                      ".spark",
                                      "/Score.spark",
                                      "Combat//Score.spark",
                                      "Score.SPARK",
                                      "Score.spark/",
                                      "Com bat/Score.spark",
                                      "Combat\\Score.spark",
                                      "1Score.spark",
                                      "Score.spark.spark",
                                      "Combat/.spark",
                                      "\xC3\x9C"
                                      "ber.spark"};
    for (const StringView path : Invalid) {
        SystemAllocator allocator;
        {
            Fixture fixture(allocator);
            const uint32 game = fixture.SimplePackage("Game");
            const uint32 module = fixture.Module(game, "Game", path);
            REQUIRE(fixture.Check());
            StringBuilder<128> display(allocator);
            display.Append("Game/").Append(path);
            CHECK(fixture.Reports(C::InvalidModulePath, display.View()));
            CHECK(fixture.compilation.ModuleImportPath(module).IsEmpty());
            // 경로가 틀려도 구문 분석은 한다.
            CHECK(fixture.compilation.ModuleTree(module) != nullptr);
        }
        CHECK(allocator.Stats().liveAllocations == 0);
    }

    SystemAllocator allocator;
    {
        Fixture fixture(allocator);
        const uint32 game = fixture.SimplePackage("Game");
        fixture.Module(game, "Game", "A.spark");
        fixture.Module(game, "Game", "A1/B2/C3.spark");
        fixture.Module(game, "Game", "Combat/sCore.spark");
        fixture.Module(game, "Game",
                       "\xC3\x9C"
                       "ber.spark");
        REQUIRE(fixture.Check());
        const Expected expected[] = {{C::InvalidModulePath, "Game/Combat/sCore.spark"},
                                     {C::InvalidModulePath, "Game/\xC3\x9C"
                                                            "ber.spark"}};
        CHECK(fixture.Reports(expected));
        CHECK(fixture.Message(0) == StringView("Module path 'Combat/sCore.spark' is not valid."));
        CHECK(fixture.Message(1) == StringView("The module path is not valid."));
        CHECK(fixture.Fix(0) == StringView("Rename it, as in 'Combat/Score.spark'."));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 대소문자만 다른 경로는 byte 순으로 처음인 철자가 아닌 쪽의 파일에 낸다(폴더·모듈 이름 모두).
TEST(SparksCompilation_PathCaseS1109) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator);
        const uint32 game = fixture.SimplePackage("Game");
        fixture.Module(game, "Game", "CombAt/A.spark");
        fixture.Module(game, "Game", "Combat/B.spark");
        fixture.Module(game, "Game", "Combat/C.spark");
        fixture.Module(game, "Game", "Score.spark");
        fixture.Module(game, "Game", "SCore.spark");
        fixture.Module(game, "Game", "Door.spark");
        fixture.Module(game, "Game", "DOor/X.spark");
        // 폴더와 파일 이름이 함께 겹쳐도 그 모듈에 하나다.
        fixture.Module(game, "Game", "KIT/Part.spark");
        fixture.Module(game, "Game", "Kit/Part.spark");
        REQUIRE(fixture.Check());
        const Expected expected[] = {{C::ModulePathCaseConflict, "Game/Combat/B.spark"},
                                     {C::ModulePathCaseConflict, "Game/Combat/C.spark"},
                                     {C::ModulePathCaseConflict, "Game/Door.spark"},
                                     {C::ModulePathCaseConflict, "Game/Kit/Part.spark"},
                                     {C::ModulePathCaseConflict, "Game/Score.spark"}};
        CHECK(fixture.Reports(expected));
        CHECK(fixture.Message(0) ==
              StringView("Path 'Combat' differs from 'CombAt' only in letter case."));
        CHECK(fixture.Message(2) ==
              StringView("Path 'Door' differs from 'DOor' only in letter case."));
        CHECK(fixture.Message(3) ==
              StringView("Path 'Kit' differs from 'KIT' only in letter case."));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SparksCompilation_ModuleFolderS1110) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator);
        const uint32 game = fixture.SimplePackage("Game");
        fixture.Module(game, "Game", "Combat.spark");
        fixture.Module(game, "Game", "Combat/Score.spark");
        fixture.Module(game, "Game", "A/B.spark");
        fixture.Module(game, "Game", "A/B/C.spark");
        // 이름이 접두일 뿐인 것은 겹치지 않는다.
        fixture.Module(game, "Game", "Door.spark");
        fixture.Module(game, "Game", "DoorKit/X.spark");
        fixture.Module(game, "Game", "Door0/X.spark");
        REQUIRE(fixture.Check());
        const Expected expected[] = {{C::ModuleFolderConflict, "Game/A/B.spark"},
                                     {C::ModuleFolderConflict, "Game/Combat.spark"}};
        CHECK(fixture.Reports(expected));
        CHECK(fixture.Message(1) ==
              StringView("File 'Combat.spark' has the same name as the folder 'Combat/'."));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SparksCompilation_DependencyNamesS1106AndS1111) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator);
        const uint32 game =
            fixture.Package("Game", "{ name: \"Game\", sparks: 1, profile: \"core\",\n"
                                    "  dependencies: [\"Weapons\", \"Missing\", \"Ui\"] }");
        const uint32 weapons = fixture.SimplePackage("Weapons");
        fixture.SimplePackage("Ui");
        fixture.Module(game, "Game", "Weapons/Local.spark");
        fixture.Module(game, "Game", "Ui.spark");
        fixture.Module(game, "Game", "Rocks/A.spark");
        fixture.Module(game, "Game", "Rocks/B.spark");
        fixture.Module(weapons, "Weapons", "Sword.spark");
        REQUIRE(fixture.Check());
        const Expected expected[] = {{C::PackageFolderConflict, "Game/Rocks/A.spark"},
                                     {C::PackageFolderConflict, "Game/SparksPackage.json5", 2, 18},
                                     {C::MissingDependency, "Game/SparksPackage.json5", 2, 29},
                                     {C::PackageFolderConflict, "Game/SparksPackage.json5", 2, 40}};
        CHECK(fixture.Reports(expected));
        CHECK(fixture.Message(0) ==
              StringView("The top-level name 'Rocks' is the engine package name."));
        CHECK(fixture.Message(2) == StringView("Package 'Missing' is not in this compilation."));
        CHECK(fixture.Message(1) == StringView("Dependency 'Weapons' has the same name as a "
                                               "top-level folder or module of this package."));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 순환 하나에 진단 하나: 이름 byte 순으로 처음인 패키지의 manifest, 순환 안을 가리키는 첫 의존
// 항목.
TEST(SparksCompilation_DependencyCycleS1107) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator);
        fixture.Package("B",
                        "{ name: \"B\", sparks: 1, profile: \"core\", dependencies: [\"A\"] }");
        fixture.Package(
            "A", "{ name: \"A\", sparks: 1, profile: \"core\", dependencies: [\"X\", \"B\"] }");
        fixture.Package("X", "{ name: \"X\", sparks: 1, profile: \"core\" }");
        REQUIRE(fixture.Check());
        CHECK(fixture.Reports(C::DependencyCycle, "A/SparksPackage.json5", 1, 62));
        CHECK(fixture.Message(0) ==
              StringView("Packages depend on each other in a cycle: A -> B -> A."));
    }
    {
        Fixture fixture(allocator);
        fixture.Package("C",
                        "{ name: \"C\", sparks: 1, profile: \"core\", dependencies: [\"A\"] }");
        fixture.Package("B",
                        "{ name: \"B\", sparks: 1, profile: \"core\", dependencies: [\"C\"] }");
        fixture.Package("A",
                        "{ name: \"A\", sparks: 1, profile: \"core\", dependencies: [\"B\"] }");
        // 순환에 들지 않고 순환을 가리키기만 하는 패키지는 알리지 않는다.
        fixture.Package("E",
                        "{ name: \"E\", sparks: 1, profile: \"core\", dependencies: [\"A\"] }");
        // 따로 떨어진 두 번째 순환.
        fixture.Package("Q",
                        "{ name: \"Q\", sparks: 1, profile: \"core\", dependencies: [\"P\"] }");
        fixture.Package("P",
                        "{ name: \"P\", sparks: 1, profile: \"core\", dependencies: [\"Q\"] }");
        REQUIRE(fixture.Check());
        const Expected expected[] = {{C::DependencyCycle, "A/SparksPackage.json5", 1, 57},
                                     {C::DependencyCycle, "P/SparksPackage.json5", 1, 57}};
        CHECK(fixture.Reports(expected));
        CHECK(fixture.Message(0) ==
              StringView("Packages depend on each other in a cycle: A -> B -> C -> A."));
        CHECK(fixture.Message(1) ==
              StringView("Packages depend on each other in a cycle: P -> Q -> P."));
    }
    {
        // manifest가 틀린 패키지는 순환에 들지 않는다.
        Fixture fixture(allocator);
        fixture.Package("A",
                        "{ name: \"A\", sparks: 1, profile: \"core\", dependencies: [\"B\"] }");
        fixture.Package("B",
                        "{ name: \"B\", sparks: 9, profile: \"core\", dependencies: [\"A\"] }");
        REQUIRE(fixture.Check());
        CHECK(fixture.Reports(C::UnsupportedLanguageVersion, "B/SparksPackage.json5", 1, 22));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 순환 둘이 패키지를 나눠 가지면 강하게 연결된 묶음 하나다(진단 하나, 가장 짧은 길). 긴 사슬도
// 재귀 없이 찾는다.
TEST(SparksCompilation_DependencyCycleComponents) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator);
        fixture.Package(
            "B", "{ name: \"B\", sparks: 1, profile: \"core\", dependencies: [\"C\", \"A\"] }");
        fixture.Package("C",
                        "{ name: \"C\", sparks: 1, profile: \"core\", dependencies: [\"B\"] }");
        fixture.Package("A",
                        "{ name: \"A\", sparks: 1, profile: \"core\", dependencies: [\"B\"] }");
        REQUIRE(fixture.Check());
        CHECK(fixture.Reports(C::DependencyCycle, "A/SparksPackage.json5", 1, 57));
        CHECK(fixture.Message(0) ==
              StringView("Packages depend on each other in a cycle: A -> B -> A."));
    }
    {
        // 묶음 {A, B, C, D}와 밖의 X. 길 찾기는 묶음 밖(X)과 이미 지난 패키지(B)를 건너뛰고,
        // Tarjan은 이미 낮아진 low보다 큰 번호의 back edge(D -> B)를 만난다.
        Fixture fixture(allocator);
        fixture.Package("A",
                        "{ name: \"A\", sparks: 1, profile: \"core\", dependencies: [\"B\"] }");
        fixture.Package(
            "B", "{ name: \"B\", sparks: 1, profile: \"core\", dependencies: [\"C\", \"D\"] }");
        fixture.Package("C", "{ name: \"C\", sparks: 1, profile: \"core\", "
                             "dependencies: [\"X\", \"B\", \"A\"] }");
        fixture.Package(
            "D", "{ name: \"D\", sparks: 1, profile: \"core\", dependencies: [\"A\", \"B\"] }");
        fixture.Package("X", "{ name: \"X\", sparks: 1, profile: \"core\" }");
        REQUIRE(fixture.Check());
        CHECK(fixture.Reports(C::DependencyCycle, "A/SparksPackage.json5", 1, 57));
        CHECK(fixture.Message(0) ==
              StringView("Packages depend on each other in a cycle: A -> B -> C -> A."));
    }
    {
        constexpr uint32 ChainLength = 300;
        Fixture fixture(allocator);
        for (uint32 index = 0; index < ChainLength; ++index) {
            StringBuilder<32> name(allocator);
            StringBuilder<32> next(allocator);
            StringBuilder<128> manifest(allocator);
            name.AppendFormat("P{}", 1000 + index);
            next.AppendFormat("P{}", 1000 + (index + 1) % ChainLength);
            manifest.Append("{ name: \"")
                .Append(name.View())
                .Append("\", sparks: 1, profile: \"core\", dependencies: [\"")
                .Append(next.View())
                .Append("\"] }");
            REQUIRE(fixture.Package(name.View(), manifest.View()) == index);
        }

        REQUIRE(fixture.Check());
        CHECK(fixture.Reports(C::DependencyCycle, "P1000/SparksPackage.json5", 1, 61));
        CHECK(fixture.Message(0).StartsWith(
            "Packages depend on each other in a cycle: P1000 -> P1001 -> P1002",
            StringComparison::CaseSensitive));
        CHECK(fixture.Message(0).EndsWith("P1298 -> P1299 -> P1000.",
                                          StringComparison::CaseSensitive));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SparksCompilation_DuplicatePackageS1112) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator);
        const uint32 first = fixture.SimplePackage("Game");
        auto id = fixture.sources.TryAdd("Copy/SparksPackage.json5",
                                         "{ name: \"Game\", sparks: 1, profile: \"core\" }",
                                         fixture.diagnostics);
        REQUIRE(id);
        auto second = fixture.compilation.AddPackage(id.Value());
        REQUIRE(second);
        auto copySource = fixture.sources.TryAdd("Copy/Main.spark", "x", fixture.diagnostics);
        REQUIRE(copySource);
        auto copyModule =
            fixture.compilation.AddModule(second.Value(), copySource.Value(), "Main.spark");
        REQUIRE(copyModule);
        REQUIRE(fixture.Check());
        CHECK(fixture.Reports(C::DuplicatePackage, "Copy/SparksPackage.json5", 1, 9));
        CHECK(fixture.compilation.PackageIsValid(first));
        CHECK(!fixture.compilation.PackageIsValid(second.Value()));
        // 무효 패키지의 모듈은 분석하지 않는다(구문 오류 'x'를 알리지 않는다).
        CHECK(fixture.compilation.ModuleTree(copyModule.Value()) == nullptr);
        CHECK(fixture.compilation.ModuleImports(copyModule.Value()).IsEmpty());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 이름 비교는 다른 manifest 오류와 무관하다: 먼저 더한 패키지가 무효여도 뒤의 같은 이름은 S1112다.
// 그 이름을 가리키는 의존·import는 무효 패키지를 가리키므로 판정하지 않는다.
TEST(SparksCompilation_DuplicatePackageWithInvalidFirst) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator);
        const uint32 first =
            fixture.Package("Game", "{ name: \"Game\", sparks: 2, profile: \"core\" }");
        const uint32 copy =
            fixture.Package("Copy", "{ name: \"Game\", sparks: 1, profile: \"core\" }");
        const uint32 user = fixture.SimplePackage("User", "\"Game\"");
        const uint32 main = fixture.Module(user, "User", "Main.spark", "import Game.X;\n");
        REQUIRE(fixture.Check());
        const Expected expected[] = {
            {C::DuplicatePackage, "Copy/SparksPackage.json5", 1, 9},
            {C::UnsupportedLanguageVersion, "Game/SparksPackage.json5", 1, 25},
        };
        CHECK(fixture.Reports(expected));
        CHECK(!fixture.compilation.PackageIsValid(first));
        CHECK(!fixture.compilation.PackageIsValid(copy));
        CHECK(fixture.compilation.PackageIsValid(user));
        CHECK(fixture.compilation.FindPackage("Game") == NoPackage);
        // 무효인 패키지도 `name` 값이 맞으면 이름은 남는다.
        CHECK(fixture.compilation.Manifest(first).Name() == StringView("Game"));
        REQUIRE(fixture.compilation.ModuleImports(main).Size() == 1);
        CHECK(fixture.compilation.ModuleImports(main)[0].module == NoModule);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 대소문자만 다른 이름도 S1112이고, 세 번 나온 이름은 뒤의 것마다 하나다.
TEST(SparksCompilation_DuplicatePackageLetterCase) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator);
        const uint32 a = fixture.Package("A", "{ name: \"Game\", sparks: 1, profile: \"core\" }");
        const uint32 b = fixture.Package("B", "{ name: \"GAME\", sparks: 1, profile: \"core\" }");
        fixture.Package("C", "{ name: \"Game\", sparks: 1, profile: \"core\" }");
        fixture.Package("D", "{ name: \"GaMe\", sparks: 1, profile: \"core\" }");
        // 접두만 같은 이름은 다른 이름이다.
        const uint32 kit =
            fixture.Package("E", "{ name: \"GameKit\", sparks: 1, profile: \"core\" }");
        const uint32 short_ =
            fixture.Package("F", "{ name: \"Gam\", sparks: 1, profile: \"core\" }");
        REQUIRE(fixture.Check());
        const Expected expected[] = {{C::DuplicatePackage, "B/SparksPackage.json5", 1, 9},
                                     {C::DuplicatePackage, "C/SparksPackage.json5", 1, 9},
                                     {C::DuplicatePackage, "D/SparksPackage.json5", 1, 9}};
        CHECK(fixture.Reports(expected));
        CHECK(fixture.Message(0) ==
              StringView("Package 'GAME' differs from package 'Game' only in letter case."));
        CHECK(fixture.Message(1) ==
              StringView("Package 'Game' is defined twice in this compilation."));
        CHECK(fixture.Message(2) ==
              StringView("Package 'GaMe' differs from package 'Game' only in letter case."));
        CHECK(fixture.compilation.FindPackage("Game") == a);
        CHECK(fixture.compilation.FindPackage("GAME") == NoPackage);
        CHECK(!fixture.compilation.PackageIsValid(b));
        CHECK(fixture.compilation.FindPackage("GameKit") == kit);
        CHECK(fixture.compilation.FindPackage("Gam") == short_);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 이름을 읽지 못한 manifest가 있으면 그것이 그 의존일 수 있으므로 S1106을 내지 않는다.
TEST(SparksCompilation_UnknownNameSuppressesMissingDependency) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator);
        fixture.Package("Lost", "{ name: \"Lost\", sparks: 1, profile: \"core\"");
        fixture.SimplePackage("Game", "\"Lost\", \"Nope\"");
        REQUIRE(fixture.Check());
        CHECK(fixture.Reports(C::ManifestSyntax, "Lost/SparksPackage.json5", 1, 43));
    }
    {
        Fixture fixture(allocator);
        fixture.Package("Bad", "{ name: \"bad\", sparks: 1, profile: \"core\" }");
        fixture.SimplePackage("Game", "\"Nope\"");
        REQUIRE(fixture.Check());
        CHECK(fixture.Reports(C::InvalidManifestValue, "Bad/SparksPackage.json5", 1, 9));
    }
    {
        // 이름을 모두 읽었으면 낸다.
        Fixture fixture(allocator);
        fixture.Package("Bad", "{ name: \"Bad\", sparks: 3, profile: \"core\" }");
        fixture.SimplePackage("Game", "\"Nope\"");
        REQUIRE(fixture.Check());
        const Expected expected[] = {
            {C::UnsupportedLanguageVersion, "Bad/SparksPackage.json5", 1, 24},
            {C::MissingDependency, "Game/SparksPackage.json5", 1, 60},
        };
        CHECK(fixture.Reports(expected));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 없는 의존이 최상위 폴더 이름과도 같으면 둘 다 그 항목에 낸다.
TEST(SparksCompilation_MissingDependencyThatIsAlsoAFolder) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator);
        const uint32 game = fixture.SimplePackage("Game", "\"Missing\"");
        fixture.Module(game, "Game", "Missing/A.spark");
        REQUIRE(fixture.Check());
        const Expected expected[] = {
            {C::MissingDependency, "Game/SparksPackage.json5", 1, 60},
            {C::PackageFolderConflict, "Game/SparksPackage.json5", 1, 60},
        };
        CHECK(fixture.Reports(expected));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SparksCompilation_ManifestErrorsStopModuleAnalysis) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator);
        const uint32 game = fixture.Package("Game", "{ name: \"Game\", sparks: 1 }");
        const uint32 module = fixture.Module(game, "Game", "Main.spark", "garbage import X;");
        const uint32 bad = fixture.Module(game, "Game", "bad.spark");
        REQUIRE(fixture.Check());
        CHECK(fixture.Reports(C::MissingManifestKey, "Game/SparksPackage.json5", 1, 1));
        CHECK(fixture.compilation.ModuleTree(module) == nullptr);
        // 모듈 경로도 검사하지 않는다(그 패키지의 manifest가 틀렸으므로).
        CHECK(fixture.compilation.ModuleImportPath(bad).IsEmpty());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SparksCompilation_ImportResolutionS0301ToS0305) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator);
        const uint32 game = fixture.SimplePackage("Game", "\"Weapons\"");
        const uint32 weapons = fixture.SimplePackage("Weapons");
        fixture.SimplePackage("Ui");
        fixture.Module(weapons, "Weapons", "Sword.spark");
        fixture.Module(game, "Game", "Combat/Score.spark");
        const uint32 main = fixture.Module(game, "Game", "Main.spark",
                                           "import Combat.Scor;\n"       // 1 S0301 제안
                                           "import Nothing.Here;\n"      // 2 S0301
                                           "import Weapons;\n"           // 3 S0301 패키지
                                           "import Weapons.Swrd;\n"      // 4 S0301 제안
                                           "import Ui.Hud;\n"            // 5 S0302
                                           "import Combat.Score;\n"      // 6 해석
                                           "import Combat.Score as S;\n" // 7 S0303
                                           "import Main;\n"              // 8 S0304
                                           "import Rocks.Engine;\n"      // 9 S0305
                                           "import Rocks;\n"             // 10 S0305
                                           "import Weapons.Sword;\n");   // 11 해석
        // import 단계만 본다(쓰지 않는 import S0714는 Flow 단계 — Conformance/Flow).
        REQUIRE(fixture.compilation.SetLastStage(CompilationStage::Imports));
        REQUIRE(fixture.Check());
        const Expected expected[] = {
            {C::UnknownModule, "Game/Main.spark", 1, 8},
            {C::UnknownModule, "Game/Main.spark", 2, 8},
            {C::UnknownModule, "Game/Main.spark", 3, 8},
            {C::UnknownModule, "Game/Main.spark", 4, 8},
            {C::UndeclaredDependency, "Game/Main.spark", 5, 8},
            {C::DuplicateImport, "Game/Main.spark", 7, 8},
            {C::SelfImport, "Game/Main.spark", 8, 8},
            {C::RocksImport, "Game/Main.spark", 9, 8},
            {C::RocksImport, "Game/Main.spark", 10, 8},
        };
        CHECK(fixture.Reports(expected));
        CHECK(fixture.Message(0) == StringView("Unknown module 'Combat.Scor'."));
        CHECK(fixture.Fix(0) == StringView("Write 'import Combat.Score;'."));
        CHECK(fixture.Fix(1) == StringView("Check the file path of the module."));
        CHECK(fixture.Message(2) == StringView("'Weapons' is a package, not a module."));
        CHECK(fixture.Message(3) == StringView("Package 'Weapons' has no module 'Swrd'."));
        CHECK(fixture.Fix(3) == StringView("Write 'import Weapons.Sword;'."));
        CHECK(fixture.Message(4) ==
              StringView("Package 'Ui' is not a dependency of this package."));
        CHECK(fixture.Fix(4) == StringView("Add \"Ui\" to 'dependencies' in SparksPackage.json5."));
        CHECK(fixture.Message(5) == StringView("Module 'Combat.Score' is already imported."));
        CHECK(fixture.Message(6) == StringView("A module cannot import itself."));
        CHECK(fixture.Message(7) == StringView("The Rocks package is not imported."));
        const Span<const ImportResolution> imports = fixture.compilation.ModuleImports(main);
        REQUIRE(imports.Size() == 11);
        CHECK(imports[0].module == NoModule);
        CHECK(imports[5].module == fixture.compilation.FindModule(game, "Combat.Score"));
        CHECK(imports[6].module == NoModule);
        CHECK(imports[7].module == NoModule);
        CHECK(imports[10].module == fixture.compilation.FindModule(weapons, "Sword"));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 판정하지 않는 import: 없거나 manifest가 틀린 의존 패키지(이미 알렸다), 구문 오류가 있는 경로.
// 현재 패키지의 모듈이 다른 패키지 이름과 첫 구간이 같으면 현재 패키지 쪽이다.
TEST(SparksCompilation_ImportsThatAreNotJudged) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator);
        const uint32 game = fixture.SimplePackage("Game", "\"Broken\", \"Missing\"");
        fixture.Package("Broken", "{ name: \"Broken\", sparks: 2, profile: \"core\" }");
        fixture.SimplePackage("Other");
        const uint32 local = fixture.Module(game, "Game", "Other/Thing.spark");
        const uint32 main = fixture.Module(game, "Game", "Main.spark",
                                           "import Broken.X;\nimport Missing.Y;\nimport A.;\n"
                                           "import Other.Thing;\n");
        // import 단계만 본다(쓰지 않는 import S0714는 Flow 단계 — Conformance/Flow).
        REQUIRE(fixture.compilation.SetLastStage(CompilationStage::Imports));
        REQUIRE(fixture.Check());
        const Expected expected[] = {
            {C::UnsupportedLanguageVersion, "Broken/SparksPackage.json5", 1, 27},
            {C::ExpectedName, "Game/Main.spark", 3, 10},
            {C::MissingDependency, "Game/SparksPackage.json5", 1, 70},
        };
        CHECK(fixture.Reports(expected));
        const Span<const ImportResolution> imports = fixture.compilation.ModuleImports(main);
        REQUIRE(imports.Size() == 4);
        CHECK(imports[0].module == NoModule);
        CHECK(imports[1].module == NoModule);
        CHECK(imports[2].module == NoModule);
        CHECK(imports[3].module == local);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 현재 패키지 이름으로 시작하는 import는 패키지 이름 없이 쓴다고 설명한다. 경로에 어휘 오류가 있는
// import와 선언 뒤의 import(S0210)는 판정하지 않는다.
TEST(SparksCompilation_ImportOwnPackageNameAndLexicalErrors) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator);
        const uint32 game = fixture.SimplePackage("Game");
        fixture.Module(game, "Game", "X.spark");
        const uint32 main = fixture.Module(game, "Game", "Main.spark",
                                           "import Game.X;\n"     // 1 S0301, 'import X;' 제안
                                           "import Game.Nope;\n"  // 2 S0301
                                           "import Game;\n"       // 3 S0301(한 구간)
                                           "import Foo_Bar;\n"    // 4 S0105만
                                           "import Bar.Ba_z;\n"   // 5 S0105만
                                           "enum E { A }\n"       // 6
                                           "import Late.One;\n"); // 7 S0210만
        REQUIRE(fixture.Check());
        const Expected expected[] = {
            {C::UnknownModule, "Game/Main.spark", 1, 8},
            {C::UnknownModule, "Game/Main.spark", 2, 8},
            {C::UnknownModule, "Game/Main.spark", 3, 8},
            {C::UnderscoreInIdentifier, "Game/Main.spark", 4, 8},
            {C::UnderscoreInIdentifier, "Game/Main.spark", 5, 12},
            {C::ImportAfterDeclaration, "Game/Main.spark", 7, 1},
        };
        CHECK(fixture.Reports(expected));
        const StringView own = "Modules of the same package are imported without the package name.";
        CHECK(fixture.Message(0) == StringView("Unknown module 'Game.X'."));
        CHECK(fixture.diagnostics[0].Explanation() == own);
        CHECK(fixture.Fix(0) == StringView("Write 'import X;'."));
        CHECK(fixture.Message(1) == StringView("Unknown module 'Game.Nope'."));
        CHECK(fixture.diagnostics[1].Explanation() == own);
        CHECK(fixture.Fix(1) == StringView("Check the file path of the module."));
        CHECK(fixture.Message(2) == StringView("Unknown module 'Game'."));
        CHECK(fixture.diagnostics[2].Explanation() != own);
        const Span<const ImportResolution> imports = fixture.compilation.ModuleImports(main);
        REQUIRE(imports.Size() == 5);
        for (const ImportResolution& import : imports) {
            CHECK(import.module == NoModule);
        }
    }
    {
        // 자기 모듈 import는 줄마다, 같은 모듈의 거듭된 import는 두 번째 줄부터 줄마다다.
        Fixture fixture(allocator);
        const uint32 game = fixture.SimplePackage("Game");
        fixture.Module(game, "Game", "X.spark");
        fixture.Module(game, "Game", "Main.spark",
                       "import Main;\nimport Main;\nimport X;\nimport X as Y;\nimport X;\n");
        // import 단계만 본다(쓰지 않는 import S0714는 Flow 단계 — Conformance/Flow).
        REQUIRE(fixture.compilation.SetLastStage(CompilationStage::Imports));
        REQUIRE(fixture.Check());
        const Expected expected[] = {
            {C::SelfImport, "Game/Main.spark", 1, 8},
            {C::SelfImport, "Game/Main.spark", 2, 8},
            {C::DuplicateImport, "Game/Main.spark", 4, 8},
            {C::DuplicateImport, "Game/Main.spark", 5, 8},
        };
        CHECK(fixture.Reports(expected));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// schema 파일은 더한 순서로 앞서 받아들인 파일하고만 비교한다. 모듈 이름도 파일 사이에서 하나다.
TEST(SparksCompilation_SchemaFilesComparedWithAcceptedOnly) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator);
        REQUIRE(fixture.Schema("Schema/A.sparkbind.json", EngineSchema));
        // Actor가 겹쳐 거부한다(Pawn은 받아들이지 않는다).
        REQUIRE(fixture.Schema("Schema/B.sparkbind.json",
                               R"({ "format": 1, "package": "Rocks", "module": "Physics",
  "types": [ { "name": "Actor", "id": "5a7e0000000000000000000000000009", "kind": "object" },
             { "name": "Pawn", "id": "5a7e000000000000000000000000000a", "kind": "object" } ] })"));
        // 거부한 B와만 겹치므로 받아들인다.
        REQUIRE(fixture.Schema("Schema/C.sparkbind.json",
                               R"({ "format": 1, "package": "Rocks", "module": "Physics",
  "types": [ { "name": "Pawn", "id": "5a7e000000000000000000000000000a", "kind": "object" } ] })"));
        // 모듈 이름 Engine이 A와 겹친다.
        REQUIRE(fixture.Schema("Schema/D.sparkbind.json",
                               R"({ "format": 1, "package": "Rocks", "module": "Engine",
  "types": [ { "name": "Widget", "id": "5a7e000000000000000000000000000b", "kind": "object" } ] })"));
        // 겹치는 것마다 하나(이름 둘, ID 하나).
        REQUIRE(fixture.Schema("Schema/E.sparkbind.json",
                               R"({ "format": 1, "package": "Rocks", "module": "Audio",
  "types": [ { "name": "Actor", "id": "5a7e000000000000000000000000000c", "kind": "object" },
             { "name": "Pawn", "id": "5a7e000000000000000000000000000d", "kind": "object" },
             { "name": "Sound", "id": "5a7e0000000000000000000000000001", "kind": "asset" } ] })"));
        REQUIRE(fixture.Check());
        const Expected expected[] = {
            {C::InvalidBindingSchema, "Schema/B.sparkbind.json", 2, 24},
            {C::InvalidBindingSchema, "Schema/D.sparkbind.json", 1, 46},
            {C::InvalidBindingSchema, "Schema/E.sparkbind.json", 2, 24},
            {C::InvalidBindingSchema, "Schema/E.sparkbind.json", 3, 24},
            {C::InvalidBindingSchema, "Schema/E.sparkbind.json", 4, 39},
        };
        CHECK(fixture.Reports(expected));
        CHECK(fixture.Message(1) ==
              StringView("Module 'Engine' is also described by another binding schema file."));
        CHECK(fixture.compilation.SchemaIsValid(0));
        CHECK(!fixture.compilation.SchemaIsValid(1));
        CHECK(fixture.compilation.SchemaIsValid(2));
        CHECK(!fixture.compilation.SchemaIsValid(3));
        CHECK(!fixture.compilation.SchemaIsValid(4));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SparksCompilation_SchemaFilesShareNamesAndIds) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator);
        REQUIRE(fixture.Schema("Schema/Engine.sparkbind.json", EngineSchema));
        REQUIRE(fixture.Schema("Schema/Physics.sparkbind.json",
                               R"({ "format": 1, "package": "Rocks", "module": "Physics",
  "types": [ { "name": "Actor", "id": "5a7e0000000000000000000000000009", "kind": "object" } ] })"));
        REQUIRE(fixture.Schema("Schema/Audio.sparkbind.json",
                               R"({ "format": 1, "package": "Rocks", "module": "Audio",
  "types": [ { "name": "Sound", "id": "5a7e0000000000000000000000000001", "kind": "asset" } ] })"));
        REQUIRE(fixture.Schema("Schema/Ui.sparkbind.json",
                               R"({ "format": 1, "package": "Rocks", "module": "Ui",
  "types": [ { "name": "Widget", "id": "5a7e0000000000000000000000000002", "kind": "object" } ] })"));
        REQUIRE(fixture.Check());
        const Expected expected[] = {
            {C::InvalidBindingSchema, "Schema/Audio.sparkbind.json", 2, 39},
            {C::InvalidBindingSchema, "Schema/Physics.sparkbind.json", 2, 24},
        };
        CHECK(fixture.Reports(expected));
        CHECK(fixture.Message(0) ==
              StringView("This ID is also used in another binding schema file."));
        CHECK(fixture.Message(1) ==
              StringView("This type is also declared in another binding schema file."));
        CHECK(fixture.compilation.SchemaIsValid(0));
        CHECK(!fixture.compilation.SchemaIsValid(1));
        CHECK(!fixture.compilation.SchemaIsValid(2));
        CHECK(fixture.compilation.SchemaIsValid(3));
        CHECK(fixture.compilation.Schema(3).Module() == StringView("Ui"));
    }
    {
        // 이벤트 ID도 파일 사이에서 하나다.
        Fixture fixture(allocator);
        REQUIRE(fixture.Schema("Schema/A.sparkbind.json",
                               R"({ "format": 1, "package": "Rocks", "module": "A",
  "types": [ { "name": "A", "id": "5a7e0000000000000000000000000001", "kind": "object",
               "events": [ { "name": "E", "id": "5a7e0000000000000000000000000002" } ] } ] })"));
        REQUIRE(fixture.Schema("Schema/B.sparkbind.json",
                               R"({ "format": 1, "package": "Rocks", "module": "B",
  "types": [ { "name": "B", "id": "5a7e0000000000000000000000000003", "kind": "object",
               "events": [ { "name": "E", "id": "5a7e0000000000000000000000000002" } ] } ] })"));
        REQUIRE(fixture.Check());
        CHECK(fixture.Reports(C::InvalidBindingSchema, "Schema/B.sparkbind.json", 3, 49));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 원문 검사가 막은(S0101) schema·manifest 파일은 읽지 않는다. 그 패키지는 무효이고 모듈을 분석하지
// 않는다(진단은 원문 검사의 것 하나씩).
TEST(SparksCompilation_UnusableSchemaAndManifestFiles) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator);
        REQUIRE(fixture.Schema("Schema/Bad.sparkbind.json", "\xFF"));
        const uint32 game = fixture.Package("Game", "\xFF");
        const uint32 main = fixture.Module(game, "Game", "Main.spark", "x");
        REQUIRE(fixture.Check());
        const Expected expected[] = {{C::InvalidUtf8, "Game/SparksPackage.json5", 1, 1},
                                     {C::InvalidUtf8, "Schema/Bad.sparkbind.json", 1, 1}};
        CHECK(fixture.Reports(expected));
        CHECK(!fixture.compilation.SchemaIsValid(0));
        CHECK(!fixture.compilation.PackageIsValid(game));
        CHECK(fixture.compilation.ModuleTree(main) == nullptr);
        CHECK(fixture.compilation.ModuleSource(main).index == 2);
        CHECK(fixture.compilation.Manifest(game).Name().IsEmpty());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SparksCompilation_ApiMisuseIsInvalidArgument) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator);
        const uint32 game = fixture.SimplePackage("Game");
        auto source = fixture.sources.TryAdd("Game/Main.spark", "", fixture.diagnostics);
        REQUIRE(source);
        REQUIRE(fixture.compilation.AddModule(game, source.Value(), "Main.spark"));
        auto again = fixture.compilation.AddModule(game, source.Value(), "Main.spark");
        CHECK(!again && again.Error() == ErrorCode::InvalidArgument);
        auto noPackage = fixture.compilation.AddModule(7, source.Value(), "Other.spark");
        CHECK(!noPackage && noPackage.Error() == ErrorCode::InvalidArgument);
        auto noSource = fixture.compilation.AddModule(game, SourceId{99}, "Other.spark");
        CHECK(!noSource && noSource.Error() == ErrorCode::InvalidArgument);
        REQUIRE(fixture.Check());
        auto twice = fixture.compilation.Check(fixture.diagnostics);
        CHECK(!twice && twice.Error() == ErrorCode::InvalidArgument);
        auto late = fixture.compilation.AddPackage(source.Value());
        CHECK(!late && late.Error() == ErrorCode::InvalidArgument);
        auto lateSchema = fixture.compilation.AddBindingSchema(source.Value());
        CHECK(!lateSchema && lateSchema.Error() == ErrorCode::InvalidArgument);
        auto lateModule = fixture.compilation.AddModule(game, source.Value(), "Late.spark");
        CHECK(!lateModule && lateModule.Error() == ErrorCode::InvalidArgument);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SparksCompilation_AllocationFailureEverywhere) {
    SystemAllocator backing;
    bool succeeded = false;
    uint64 failures = 0;
    for (uint64 failAt = 1; !succeeded && failAt < 100000; ++failAt) {
        test::NFailAllocator allocator(backing);
        {
            allocator.DisableFailure();
            Fixture fixture(allocator);
            REQUIRE(fixture.Schema("Schema/Engine.sparkbind.json", EngineSchema));
            const uint32 game = fixture.SimplePackage("Game", "\"Weapons\", \"Missing\"");
            const uint32 weapons = fixture.SimplePackage("Weapons", "\"Game\"");
            fixture.Module(game, "Game", "Main.spark", "import Weapons.Sword;\nimport Nope;\n");
            fixture.Module(game, "Game", "Combat.spark");
            fixture.Module(game, "Game", "Combat/A.spark");
            fixture.Module(game, "Game", "combat2.spark");
            fixture.Module(weapons, "Weapons", "Sword.spark");
            allocator.FailOnAttempt(failAt);
            auto checked = fixture.compilation.Check(fixture.diagnostics);
            if (checked) {
                succeeded = true;
                CHECK(fixture.diagnostics.Count() == 6); // S0714(Weapons.Sword)까지
            } else {
                ++failures;
                CHECK(checked.Error() == ErrorCode::OutOfMemory);
            }
        }
        CHECK(backing.Stats().liveAllocations == 0);
    }

    CHECK(succeeded);
    CHECK(failures >= 10);
}

} // namespace sparks_compilation_tests_file
