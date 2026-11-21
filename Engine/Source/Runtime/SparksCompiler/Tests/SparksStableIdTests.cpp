#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/StringBuilder.h>
#include <Platform/Fatal.h>
#include <SparksCompiler/Compilation.h>
#include <SparksCompiler/Diagnostic.h>
#include <SparksCompiler/Source.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

// 안정 ID 시험이다(Docs/Design/SparksBytecode.ko.md 6.1절, LC4d): 선언의 이름 키와 발급 대기 목록,
// manifest와 맞춘 S1005(사라진 데이터 연결 ID)·`@formerly`·은퇴 항목, manifest 형식 오류 S1006,
// 진단이 난 모듈의 제외, 할당 실패.
using namespace rocks;
using namespace rocks::sparks;
namespace sparks_stable_id_tests_file {

namespace {

constexpr StringView EngineSchema =
    R"({ "format": 1, "package": "Rocks", "module": "Engine", "types": [
{ "name": "Actor", "id": "5a7e0000000000000000000000000001", "kind": "object", "scriptExtensible": true, "events": [ { "name": "Destroyed", "id": "5a7e0000000000000000000000000104" } ] },
{ "name": "Component", "id": "5a7e0000000000000000000000000002", "kind": "object", "fields": [ { "name": "Owner", "id": "5a7e0000000000000000000000000201", "type": "Actor" } ] },
{ "name": "ScriptableComponent", "id": "5a7e0000000000000000000000000003", "kind": "object", "base": "Component", "scriptExtensible": true },
{ "name": "World", "id": "5a7e0000000000000000000000000006", "kind": "object" }
] })";

// 이름 키를 받는 선언(type·field·enumCase·handler·RPC)과 받지 않는 선언(최상위 fn·일반 메서드).
constexpr StringView Program = R"(enum Side { Left, Right }

struct Stats {
    var Level: int = 1;
}

class Door : ScriptableComponent {
    @save var Open = false;
    var Info = Stats();

    on Owner.Destroyed() {
    }

    @rpc(Server) fn Request() {
        Open = true;
    }

    fn Plain() {
    }
}

fn Helper(): int {
    return 1;
}
)";

class Fixture final {
public:
    explicit Fixture(Allocator& allocator) noexcept
        : sources(allocator), diagnostics(allocator), compilation(allocator, sources) {
    }

    bool Build(StringView program, StringView manifest = StringView(),
               StringView second = StringView()) noexcept {
        auto schema = sources.TryAdd("Schema/Engine.sparkbind.json", EngineSchema, diagnostics);
        auto package =
            sources.TryAdd("Game/SparksPackage.json5",
                           R"({ name: "Game", sparks: 1, profile: "full" })", diagnostics);
        auto main = sources.TryAdd("Game/Doors/Door.spark", program, diagnostics);
        if (!schema || !package || !main || !compilation.AddBindingSchema(schema.Value())) {
            return false;
        }

        auto added = compilation.AddPackage(package.Value());
        if (!added || !compilation.AddModule(added.Value(), main.Value(), "Doors/Door.spark")) {
            return false;
        }

        if (!second.IsEmpty()) {
            auto other = sources.TryAdd("Game/Broken.spark", second, diagnostics);
            if (!other || !compilation.AddModule(added.Value(), other.Value(), "Broken.spark")) {
                return false;
            }
        }

        if (!manifest.IsEmpty()) {
            auto ids = sources.TryAdd("Game/Game.rocksids.json", manifest, diagnostics);
            if (!ids || !compilation.AddIdManifest(added.Value(), ids.Value())) {
                return false;
            }

            if (compilation.AddIdManifest(added.Value(), ids.Value()).Error() !=
                ErrorCode::InvalidArgument) {
                return false; // 패키지 하나에 manifest 하나
            }
        }

        if (!compilation.Check(diagnostics)) {
            return false;
        }

        diagnostics.Sort(sources);
        return true;
    }

    // 발급 대기를 `kind name[<-former]` 줄로 잇는다.
    void Pending(StringBuilderBase& out) const noexcept {
        for (uint32 index = 0; index < compilation.PendingIdCount(); ++index) {
            const PendingId entry = compilation.PendingIdAt(index);
            out.Append(entry.kind).Append(" ").Append(entry.name);
            if (!entry.formerName.IsEmpty()) {
                out.Append(" <- ").Append(entry.formerName);
            }

            out.Append("\n");
        }
    }

    void Print() const noexcept {
        SystemAllocator allocator;
        StringBuilder<4096> out(allocator);
        static_cast<void>(AppendDiagnostics(out, sources, diagnostics));
        platform::WriteDiagnostic(out.View().Data(), out.ByteCount());
    }

    SourceSet sources;
    DiagnosticList diagnostics;
    Compilation compilation;
};

// 모든 이름 키를 담은 manifest다(필요하면 항목을 바꿔 쓴다).
constexpr StringView FullManifest = R"json({ "schema": 1, "entries": [
{ "kind": "type", "name": "Game/Doors/Door.Side", "id": "00000000000000000000000000000001", "aliases": [], "retired": false },
{ "kind": "enumCase", "name": "Game/Doors/Door.Side.Left", "id": "00000000000000000000000000000002", "aliases": [], "retired": false },
{ "kind": "enumCase", "name": "Game/Doors/Door.Side.Right", "id": "00000000000000000000000000000003", "aliases": [], "retired": false },
{ "kind": "type", "name": "Game/Doors/Door.Stats", "id": "00000000000000000000000000000004", "aliases": [], "retired": false },
{ "kind": "field", "name": "Game/Doors/Door.Stats.Level", "id": "00000000000000000000000000000005", "aliases": [], "retired": false },
{ "kind": "type", "name": "Game/Doors/Door.Door", "id": "00000000000000000000000000000006", "aliases": [], "retired": false },
{ "kind": "field", "name": "Game/Doors/Door.Door.Open", "id": "00000000000000000000000000000007", "aliases": [], "retired": false },
{ "kind": "field", "name": "Game/Doors/Door.Door.Info", "id": "00000000000000000000000000000008", "aliases": [], "retired": false },
{ "kind": "function", "name": "Game/Doors/Door.Door.on(Owner.Destroyed)", "id": "00000000000000000000000000000009", "aliases": [], "retired": false },
{ "kind": "function", "name": "Game/Doors/Door.Door.Request", "id": "0000000000000000000000000000000a", "aliases": [], "retired": false },
{ "kind": "rpc", "name": "Game/Doors/Door.Door.Request", "id": "0000000000000000000000000000000b", "aliases": [], "retired": false }
] })json";

} // namespace

TEST(SparksStableIds_PendingWithoutManifest) {
    SystemAllocator allocator;
    Fixture fixture(allocator);
    REQUIRE(fixture.Build(Program));
    CHECK(fixture.diagnostics.Count() == 0);
    StringBuilder<2048> pending(allocator);
    fixture.Pending(pending);
    CHECK(pending.View() == StringView("type Game/Doors/Door.Side\n"
                                       "enumCase Game/Doors/Door.Side.Left\n"
                                       "enumCase Game/Doors/Door.Side.Right\n"
                                       "type Game/Doors/Door.Stats\n"
                                       "field Game/Doors/Door.Stats.Level\n"
                                       "type Game/Doors/Door.Door\n"
                                       "field Game/Doors/Door.Door.Open\n"
                                       "field Game/Doors/Door.Door.Info\n"
                                       "function Game/Doors/Door.Door.on(Owner.Destroyed)\n"
                                       "function Game/Doors/Door.Door.Request\n"
                                       "rpc Game/Doors/Door.Door.Request\n"));
}

TEST(SparksStableIds_FullManifestResolvesEverything) {
    SystemAllocator allocator;
    Fixture fixture(allocator);
    REQUIRE(fixture.Build(Program, FullManifest));
    if (fixture.diagnostics.Count() != 0) {
        fixture.Print();
    }

    CHECK(fixture.diagnostics.Count() == 0);
    CHECK(fixture.compilation.PendingIdCount() == 0);
}

TEST(SparksStableIds_LostDataIdsS1005) {
    SystemAllocator allocator;
    {
        // `Open`을 `IsOpen`으로 바꾸고 RPC를 지우면 manifest의 Open이 사라진다(S1005, 위치는 그
        // name 값). 함수·RPC ID가 사라지는 것은 오류가 아니다.
        constexpr StringView Renamed = R"(enum Side { Left, Right }

struct Stats {
    var Level: int = 1;
}

class Door : ScriptableComponent {
    @save var IsOpen = false;
    var Info = Stats();

    on Owner.Destroyed() {
    }
}
)";
        Fixture fixture(allocator);
        REQUIRE(fixture.Build(Renamed, FullManifest));
        REQUIRE(fixture.diagnostics.Count() == 1);
        const Diagnostic& diagnostic = fixture.diagnostics[0];
        CHECK(diagnostic.Code() == DiagnosticCode::DataIdLost);
        CHECK(diagnostic.Message() ==
              StringView("field 'Game/Doors/Door.Door.Open' is in the ID manifest but is no longer "
                         "declared."));
        const SourceLocation at =
            fixture.sources.Get(diagnostic.Source()).Locate(diagnostic.Span().begin);
        CHECK(at.line == 8);
        CHECK(at.column == 28);
        StringBuilder<512> pending(allocator);
        fixture.Pending(pending);
        CHECK(pending.View() == StringView("field Game/Doors/Door.Door.IsOpen\n"));
    }
    {
        // `@formerly("Open")`이면 옛 ID를 잇는다(S1005 없음, 발급 대기는 rename).
        constexpr StringView Formerly = R"(enum Side { Left, Right }

struct Stats {
    var Level: int = 1;
}

class Door : ScriptableComponent {
    @formerly("Open") @save var IsOpen = false;
    var Info = Stats();

    on Owner.Destroyed() {
    }

    @rpc(Server) fn Request() {
        IsOpen = true;
    }
}
)";
        Fixture fixture(allocator);
        REQUIRE(fixture.Build(Formerly, FullManifest));
        if (fixture.diagnostics.Count() != 0) {
            fixture.Print();
        }

        CHECK(fixture.diagnostics.Count() == 0);
        StringBuilder<512> pending(allocator);
        fixture.Pending(pending);
        CHECK(pending.View() ==
              StringView("field Game/Doors/Door.Door.IsOpen <- Game/Doors/Door.Door.Open\n"));
    }
    {
        // 은퇴한 항목과 사라진 function 항목은 오류가 아니다.
        constexpr StringView Retired = R"({ "schema": 1, "entries": [
{ "kind": "field", "name": "Game/Doors/Door.Door.Gone", "id": "0000000000000000000000000000000c", "aliases": [], "retired": true },
{ "kind": "type", "name": "Game/Doors/Door.Door", "id": "00000000000000000000000000000006", "aliases": [], "retired": false },
{ "kind": "function", "name": "Game/Doors/Door.Door.Removed", "id": "0000000000000000000000000000000d", "aliases": [], "retired": false }
] })";
        Fixture fixture(allocator);
        REQUIRE(fixture.Build(Program, Retired));
        CHECK(fixture.diagnostics.Count() == 0);
    }
}

TEST(SparksStableIds_UnstableModulesAreNotJudged) {
    // 구문 오류가 난 모듈의 항목은 S1005로 보지 않는다(입력 중 미완성).
    SystemAllocator allocator;
    constexpr StringView Manifest = R"({ "schema": 1, "entries": [
{ "kind": "type", "name": "Game/Broken.Thing", "id": "00000000000000000000000000000010", "aliases": [], "retired": false },
{ "kind": "type", "name": "Game/Gone.Thing", "id": "00000000000000000000000000000011", "aliases": [], "retired": false }
] })";
    Fixture fixture(allocator);
    REQUIRE(
        fixture.Build(Program, Manifest, "class Thing : ScriptableComponent {\n    var = ;\n}\n"));
    usize lost = 0;
    usize syntax = 0;
    for (usize index = 0; index < fixture.diagnostics.Count(); ++index) {
        const DiagnosticCode code = fixture.diagnostics[index].Code();
        lost += code == DiagnosticCode::DataIdLost ? 1 : 0;
        syntax += code != DiagnosticCode::DataIdLost ? 1 : 0;
    }

    CHECK(syntax > 0);
    CHECK(lost == 1); // 지운 모듈(Gone)의 항목만
}

TEST(SparksStableIds_InvalidManifestsS1006) {
    SystemAllocator allocator;
    const StringView bad[] = {
        "{ not json",
        R"({ "schema": 2, "entries": [] })",
        R"({ "schema": 1 })",
        R"({ "schema": 1, "entries": [ { "kind": "type" } ] })",
        R"({ "schema": 1, "entries": [ { "kind": "class", "name": "Game/A.B", "id": "00000000000000000000000000000001", "aliases": [], "retired": false } ] })",
        R"({ "schema": 1, "entries": [ { "kind": "type", "name": "Game/A.B", "id": "0000000000000000000000000000000G", "aliases": [], "retired": false } ] })",
        R"({ "schema": 1, "entries": [ { "kind": "type", "name": "Game/A.B", "id": "00000000000000000000000000000000", "aliases": [], "retired": false } ] })",
        R"({ "schema": 1, "entries": [ { "kind": "type", "name": "Other/A.B", "id": "00000000000000000000000000000001", "aliases": [], "retired": false } ] })",
        R"({ "schema": 1, "entries": [ { "kind": "type", "name": "Game/A.B", "id": "00000000000000000000000000000001", "aliases": [1], "retired": false } ] })",
    };
    for (const StringView manifest : bad) {
        Fixture fixture(allocator);
        REQUIRE(fixture.Build(Program, manifest));
        REQUIRE(fixture.diagnostics.Count() == 1);
        CHECK(fixture.diagnostics[0].Code() == DiagnosticCode::InvalidIdManifest);
        CHECK(fixture.compilation.PendingIdCount() == 11); // 형식 오류면 모두 발급 대기로 남긴다
    }
}

TEST(SparksStableIds_AllocationFailureEverywhere) {
    SystemAllocator backing;
    uint64 attempts = 0;
    uint64 failures = 0;
    for (uint64 failAt = 0; failAt == 0 || failAt <= attempts; ++failAt) {
        test::NFailAllocator allocator(backing);
        {
            allocator.DisableFailure();
            Fixture fixture(allocator);
            if (failAt != 0) {
                allocator.FailOnAttempt(failAt);
            }

            const bool built = fixture.Build(Program, FullManifest);
            const uint64 used = allocator.Attempts();
            allocator.DisableFailure();
            if (failAt == 0) {
                REQUIRE(built);
                attempts = used;
                CHECK(fixture.compilation.PendingIdCount() == 0);
            } else if (!built) {
                ++failures;
            } else {
                CHECK(fixture.compilation.PendingIdCount() == 0);
            }
        }
        CHECK(backing.Stats().liveAllocations == 0);
    }

    CHECK(failures > 0);
}

} // namespace sparks_stable_id_tests_file
