#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/StringBuilder.h>
#include <Platform/Fatal.h>
#include <SparksCompiler/Compilation.h>
#include <SparksCompiler/Diagnostic.h>
#include <SparksCompiler/Source.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

// typed IR 내림 시험이다(Docs/Design/SparksBytecode.ko.md 3절, LC4a). 명령 모양은 corpus
// `Conformance/Codegen`의 `.ir` golden이 맡고, 여기서는 단계(Codegen 전·진단 있는 컴파일은 덤프가
// 없다), 결정성, 깊은 식 사슬의 stack, 할당 실패를 본다.
using namespace rocks;
using namespace rocks::sparks;
namespace sparks_codegen_tests_file {

namespace {

class Fixture final {
public:
    explicit Fixture(Allocator& allocator) noexcept
        : allocator_(allocator), sources(allocator), diagnostics(allocator),
          compilation(allocator, sources) {
    }

    uint32 Package(StringView name) noexcept {
        StringBuilder<128> path(allocator_);
        path.Append(name).Append("/SparksPackage.json5");
        StringBuilder<256> manifest(allocator_);
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
        if (diagnostics.Count() != 0) {
            Print();
        }

        return true;
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

// 문장·식 종류를 고루 쓰는 진단 없는 프로그램이다.
constexpr StringView CodegenProgram = R"(enum Side { Left, Right }

struct Pair {
    var First: int = 0;
    var Second: Side;
}

class Door : ScriptableComponent {
    var Open = false;
    var Info = Pair();
    var Names: Array<string> = [];

    event Opened(by: Actor);

    async fn Fetch(): Result<int> {
        await NextTick();
        return 1;
    }

    @skipWhileBusy
    on Owner.Destroyed() {
        let n = try await Fetch();
        var total = 0;
        for (i in 0..<n) {
            if (i == 2 || Open) {
                continue;
            }

            total += i;
        }

        for (index, name in Names) {
            if (name == "stop") {
                break;
            }

            total += index;
        }

        Info.First = total;
        switch (Info.Second) {
            case Side.Left: {
                emit Opened(Owner);
            }
            case Side.Right: {
                Open = !Open;
            }
        }

        let where = Owner.Location * 2.0;
        Owner.Location = where + vec3(0.0, 0.0, 1.0);
    }
}
)";

bool LowerProgram(Allocator& allocator, StringView program, StringBuilderBase& dump) noexcept {
    Fixture fixture(allocator);
    if (!fixture.Schema(EngineSchema)) {
        return false;
    }

    const uint32 game = fixture.Package("Game");
    const uint32 module = fixture.Module(game, "Game", "Main.spark", program);
    if (!fixture.Check() || fixture.diagnostics.Count() != 0) {
        return false;
    }

    return fixture.compilation.AppendIrDump(dump, module) &&
           fixture.compilation.AppendBytecodeDump(dump, module);
}

} // namespace

TEST(SparksCodegen_DumpNeedsCodegenWithoutDiagnostics) {
    SystemAllocator allocator;
    {
        // Rules까지만 검사하면 IR을 내리지 않는다.
        Fixture fixture(allocator);
        REQUIRE(fixture.Schema(EngineSchema));
        const uint32 game = fixture.Package("Game");
        const uint32 module = fixture.Module(game, "Game", "Main.spark", CodegenProgram);
        REQUIRE(fixture.compilation.SetLastStage(CompilationStage::Rules));
        REQUIRE(fixture.Check());
        CHECK(fixture.diagnostics.Count() == 0);
        StringBuilder<256> dump(allocator);
        CHECK(fixture.compilation.AppendIrDump(dump, module).Error() == ErrorCode::InvalidArgument);
        CHECK(dump.View().IsEmpty());
    }
    {
        // 진단이 하나라도 있으면(다른 모듈이어도) 어느 모듈도 내리지 않는다.
        Fixture fixture(allocator);
        REQUIRE(fixture.Schema(EngineSchema));
        const uint32 game = fixture.Package("Game");
        const uint32 good = fixture.Module(game, "Game", "Good.spark", "fn F(): int { return 1; }");
        fixture.Module(game, "Game", "Bad.spark", "fn G(): int { return true; }");
        REQUIRE(fixture.compilation.Check(fixture.diagnostics));
        CHECK(fixture.diagnostics.Count() == 1);
        StringBuilder<256> dump(allocator);
        CHECK(fixture.compilation.AppendIrDump(dump, good).Error() == ErrorCode::InvalidArgument);
    }
    {
        // 진단이 없으면 모든 모듈을 내린다(함수가 없는 모듈은 빈 덤프).
        Fixture fixture(allocator);
        REQUIRE(fixture.Schema(EngineSchema));
        const uint32 game = fixture.Package("Game");
        const uint32 main = fixture.Module(game, "Game", "Main.spark", CodegenProgram);
        const uint32 empty = fixture.Module(game, "Game", "Empty.spark", "enum E { A }\n");
        REQUIRE(fixture.Check());
        REQUIRE(fixture.diagnostics.Count() == 0);
        StringBuilder<16384> dump(allocator);
        REQUIRE(fixture.compilation.AppendIrDump(dump, main));
        CHECK(dump.View().StartsWith("function Door.Fetch()\n"));
        CHECK(dump.View().Contains("handler Door.on Owner.Destroyed()\n"));
        StringBuilder<64> none(allocator);
        REQUIRE(fixture.compilation.AppendIrDump(none, empty));
        CHECK(none.View().IsEmpty());
        CHECK(fixture.compilation.AppendIrDump(none, 99).Error() == ErrorCode::InvalidArgument);
    }
}

TEST(SparksCodegen_Deterministic) {
    SystemAllocator allocator;
    StringBuilder<16384> first(allocator);
    StringBuilder<16384> second(allocator);
    REQUIRE(LowerProgram(allocator, CodegenProgram, first));
    REQUIRE(LowerProgram(allocator, CodegenProgram, second));
    CHECK(!first.View().IsEmpty());
    CHECK(first.View() == second.View());
}

TEST(SparksCodegen_DeepChainsStayWithinTheStack) {
    // 트리 깊이 상한 가까이의 왼쪽 사슬(산술·비교, 값 `&&`, 조건 `||`, 메서드 호출)을 stack 넘침
    // 없이 내린다(LowerBinary·LowerLogical·LowerCondition은 척추를 반복으로 돈다).
    SystemAllocator allocator;
    constexpr uint32 Terms = 1000;
    constexpr uint32 Steps = 480;
    StringBuilder<65536> source(allocator);
    source.Append("fn F(x: int, flag: bool, v: vec3): int {\n    let sum = x");
    for (uint32 index = 1; index < Terms; ++index) {
        source.Append(" + x");
    }

    source.Append(";\n    let all = flag");
    for (uint32 index = 1; index < Terms; ++index) {
        source.Append(" && flag");
    }

    source.Append(";\n    if (flag");
    for (uint32 index = 1; index < Terms; ++index) {
        source.Append(" || flag");
    }

    source.Append(") {\n        _ = all;\n    }\n    _ = v");
    for (uint32 index = 0; index < Steps; ++index) {
        source.Append(".Normalize()");
    }

    source.Append(";\n    return sum;\n}\n");
    REQUIRE(!source.HasFailed());
    Fixture fixture(allocator);
    REQUIRE(fixture.Schema(EngineSchema));
    const uint32 game = fixture.Package("Game");
    const uint32 module = fixture.Module(game, "Game", "Main.spark", source.View());
    REQUIRE(fixture.Check());
    CHECK(fixture.diagnostics.Count() == 0);
    StringBuilder<1024> dump(allocator);
    REQUIRE(fixture.compilation.AppendIrDump(dump, module));
    CHECK(dump.View().StartsWith("function F("));
}

TEST(SparksCodegen_FunctionTooLargeS1150) {
    // 동시에 살아 있는 Scalar 값이 256 slot을 넘는 함수는 S1150이다(위치는 함수 이름). 같은 수의
    // 값이 차례로 죽으면(앞 값을 다 쓴 뒤 다음 값) 레지스터를 다시 써 통과한다.
    SystemAllocator allocator;
    constexpr uint32 Values = 300;
    StringBuilder<32768> wide(allocator);
    wide.Append("fn Wide(x: int): int {\n");
    for (uint32 index = 0; index < Values; ++index) {
        wide.AppendFormat("    let a{} = x + {};\n", index, index);
    }

    wide.Append("    return a0");
    for (uint32 index = 1; index < Values; ++index) {
        wide.AppendFormat(" + a{}", index);
    }

    wide.Append(";\n}\n");
    REQUIRE(!wide.HasFailed());
    Fixture fixture(allocator);
    const uint32 game = fixture.Package("Game");
    fixture.Module(game, "Game", "Main.spark", wide.View());
    REQUIRE(fixture.compilation.Check(fixture.diagnostics));
    fixture.diagnostics.Sort(fixture.sources);
    REQUIRE(fixture.diagnostics.Count() == 1);
    const Diagnostic& diagnostic = fixture.diagnostics[0];
    CHECK(diagnostic.Code() == DiagnosticCode::FunctionTooLarge);
    const SourceLocation at =
        fixture.sources.Get(diagnostic.Source()).Locate(diagnostic.Span().begin);
    CHECK(at.line == 1);
    CHECK(at.column == 4);
    CHECK(diagnostic.Message() == StringView("The function is too large to compile."));
    CHECK(fixture.compilation.ModuleBytecode(0) == nullptr ||
          fixture.compilation.ModuleBytecode(0)->functions.Size() <= 1);

    // 같은 수의 값이 차례로 죽으면 통과한다(값 300개가 몇 개의 레지스터를 돌려 쓴다).
    StringBuilder<32768> narrow(allocator);
    narrow.Append("fn Narrow(x: int): int {\n    var total = 0;\n");
    for (uint32 index = 0; index < Values; ++index) {
        narrow.AppendFormat("    let b{} = x + {};\n    total += b{};\n", index, index, index);
    }

    narrow.Append("    return total;\n}\n");
    REQUIRE(!narrow.HasFailed());
    Fixture second(allocator);
    const uint32 package = second.Package("Game");
    const uint32 module = second.Module(package, "Game", "Main.spark", narrow.View());
    REQUIRE(second.Check());
    CHECK(second.diagnostics.Count() == 0);
    const BytecodeModule* bytecode = second.compilation.ModuleBytecode(module);
    REQUIRE(bytecode != nullptr);
    REQUIRE(bytecode->functions.Size() == 1);
    CHECK(bytecode->functions[0].registers[0] < 8);
}

namespace {

// 두 모듈(Util과 그것을 import하는 Main) 패키지를 컴파일한다.
bool CompilePair(Fixture& fixture, StringView util, StringView main) noexcept {
    const uint32 game = fixture.Package("Game");
    fixture.Module(game, "Game", "Util.spark", util);
    fixture.Module(game, "Game", "Main.spark", main);
    return fixture.Check() && fixture.diagnostics.Count() == 0;
}

constexpr StringView Util = "fn Helper(): int {\n    return 1;\n}\n";
constexpr StringView Main = "import Util;\n\nfn Use(): int {\n    return Helper() + 1;\n}\n";

} // namespace

TEST(SparksCodegen_RecompileScope) {
    SystemAllocator allocator;
    Fixture before(allocator);
    REQUIRE(CompilePair(before, Util, Main));
    auto scope = [&](StringView util, StringView main, Array<uint32>& modules,
                     Array<uint64>& functions) noexcept {
        Fixture after(allocator);
        return CompilePair(after, util, main) &&
               static_cast<bool>(
                   FindRecompileScope(before.compilation, after.compilation, modules, functions));
    };
    {
        // 같은 입력: 아무것도 다시 하지 않는다(hash가 결정적이다).
        Array<uint32> modules(allocator);
        Array<uint64> functions(allocator);
        REQUIRE(scope(Util, Main, modules, functions));
        CHECK(modules.IsEmpty());
        CHECK(functions.IsEmpty());
    }
    {
        // 줄만 옮긴 본문: body hash는 위치를 넣지 않으므로 같다.
        Array<uint32> modules(allocator);
        Array<uint64> functions(allocator);
        REQUIRE(scope("\n\nfn Helper(): int {\n    return 1;\n}\n", Main, modules, functions));
        CHECK(modules.IsEmpty());
        CHECK(functions.IsEmpty());
    }
    {
        // 본문만 바뀜: interface는 같고 Util.Helper만 다시 내린다(모듈 0 — 더한 순서).
        Array<uint32> modules(allocator);
        Array<uint64> functions(allocator);
        REQUIRE(scope("fn Helper(): int {\n    return 2;\n}\n", Main, modules, functions));
        CHECK(modules.IsEmpty());
        REQUIRE(functions.Size() == 1);
        CHECK(functions[0] == ((uint64(0) << 32) | 0));
    }
    {
        // interface가 바뀜(공개 함수 추가): Util과 그것을 import하는 Main을 다시 검사한다.
        Array<uint32> modules(allocator);
        Array<uint64> functions(allocator);
        REQUIRE(
            scope("fn Helper(): int {\n    return 1;\n}\n\nfn Extra(): int {\n    return 3;\n}\n",
                  Main, modules, functions));
        REQUIRE(modules.Size() == 2);
        CHECK(modules[0] == 0);
        CHECK(modules[1] == 1);
        CHECK(functions.IsEmpty());
    }
    {
        // 진단이 있는 컴파일은 범위를 정할 수 없다.
        Fixture after(allocator);
        const uint32 game = after.Package("Game");
        after.Module(game, "Game", "Util.spark", "fn Helper(): int {\n    return true;\n}\n");
        after.Module(game, "Game", "Main.spark", Main);
        REQUIRE(after.compilation.Check(after.diagnostics));
        Array<uint32> modules(allocator);
        Array<uint64> functions(allocator);
        CHECK(
            FindRecompileScope(before.compilation, after.compilation, modules, functions).Error() ==
            ErrorCode::InvalidArgument);
    }
}

TEST(SparksCodegen_AllocationFailureEverywhere) {
    SystemAllocator backing;
    uint64 checkAttempts = 0;
    StringBuilder<16384> clean(backing);
    uint64 failures = 0;
    for (uint64 failAt = 0; failAt == 0 || failAt <= checkAttempts; ++failAt) {
        test::NFailAllocator allocator(backing);
        {
            allocator.DisableFailure();
            Fixture fixture(allocator);
            REQUIRE(fixture.Schema(EngineSchema));
            const uint32 game = fixture.Package("Game");
            const uint32 module = fixture.Module(game, "Game", "Main.spark", CodegenProgram);
            if (failAt != 0) {
                allocator.FailOnAttempt(failAt);
            }

            auto checked = fixture.compilation.Check(fixture.diagnostics);
            const uint64 attempts = allocator.Attempts();
            allocator.DisableFailure(); // 덤프 글자 만들기는 실패시키지 않는다
            if (failAt == 0) {
                REQUIRE(checked);
                checkAttempts = attempts;
                REQUIRE(fixture.diagnostics.Count() == 0);
                REQUIRE(fixture.compilation.AppendIrDump(clean, module));
                REQUIRE(fixture.compilation.AppendBytecodeDump(clean, module));
            } else if (checked) {
                StringBuilder<16384> text(backing);
                REQUIRE(fixture.compilation.AppendIrDump(text, module));
                REQUIRE(fixture.compilation.AppendBytecodeDump(text, module));
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

} // namespace sparks_codegen_tests_file
