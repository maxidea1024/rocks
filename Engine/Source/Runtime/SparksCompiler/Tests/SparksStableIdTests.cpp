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
// 진단이 난 모듈의 제외, 맞춤 순서(alias·은퇴 이름 S1007, 이중 사용 S1008), type rename의 멤버
// 승계, 전체 키 `@formerly`(모듈 이동), interface 구현 함수, subobject 판정, 할당 실패.
using namespace rocks;
using namespace rocks::sparks;
namespace sparks_stable_id_tests_file {

namespace {

constexpr StringView EngineSchema =
    R"({ "format": 1, "package": "Rocks", "module": "Engine", "types": [
{ "name": "Actor", "id": "5a7e0000000000000000000000000001", "kind": "object", "scriptExtensible": true, "events": [ { "name": "Destroyed", "id": "5a7e0000000000000000000000000104" } ] },
{ "name": "Component", "id": "5a7e0000000000000000000000000002", "kind": "object", "fields": [ { "name": "Owner", "id": "5a7e0000000000000000000000000201", "type": "Actor" } ] },
{ "name": "ScriptableComponent", "id": "5a7e0000000000000000000000000003", "kind": "object", "base": "Component", "scriptExtensible": true },
{ "name": "World", "id": "5a7e0000000000000000000000000006", "kind": "object" },
{ "name": "Usable", "id": "5a7e0000000000000000000000000007", "kind": "interface", "functions": [ { "name": "Use", "id": "5a7e0000000000000000000000000701" } ] }
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

namespace {

// 발급 대기 줄 하나가 있는지다.
bool HasPending(const Fixture& fixture, StringView line) noexcept {
    SystemAllocator allocator;
    StringBuilder<2048> pending(allocator);
    fixture.Pending(pending);
    const StringView text = pending.View();
    for (usize at = 0; at + line.ByteCount() <= text.ByteCount(); ++at) {
        if (StringView(text.Data() + at, line.ByteCount()) == line &&
            (at == 0 || text.Data()[at - 1] == '\n') &&
            (at + line.ByteCount() == text.ByteCount() ||
             text.Data()[at + line.ByteCount()] == '\n')) {
            return true;
        }
    }

    return false;
}

// 진단 index의 원문 위치(줄·열)다.
SourceLocation LocationOf(const Fixture& fixture, usize index) noexcept {
    const Diagnostic& diagnostic = fixture.diagnostics[index];
    return fixture.sources.Get(diagnostic.Source()).Locate(diagnostic.Span().begin);
}

usize CountCode(const Fixture& fixture, DiagnosticCode code) noexcept {
    usize count = 0;
    for (usize index = 0; index < fixture.diagnostics.Count(); ++index) {
        count += fixture.diagnostics[index].Code() == code ? 1 : 0;
    }

    return count;
}

// Program의 Door를 Gate로 바꾸고(`@formerly("Door")`) Open을 IsOpen으로 바꾼 원문이다.
constexpr StringView RenamedType = R"(enum Side { Left, Right }

struct Stats {
    var Level: int = 1;
}

@formerly("Door") class Gate : ScriptableComponent {
    @save @formerly("Open") var IsOpen = false;
    var Info = Stats();

    on Owner.Destroyed() {
    }

    @rpc(Server) fn Request() {
        IsOpen = true;
    }
}
)";

} // namespace

TEST(SparksStableIds_TypeRenameCarriesMembers) {
    // type이 `@formerly`로 이어지면 멤버는 옛 주인 키 아래 같은 이름을 잇고(발급 대기 없음 — 도구의
    // type rename이 옮긴다), 이름도 바꾼 멤버는 type rename 뒤의 키에서 rename한다.
    SystemAllocator allocator;
    Fixture fixture(allocator);
    REQUIRE(fixture.Build(RenamedType, FullManifest));
    if (fixture.diagnostics.Count() != 0) {
        fixture.Print();
    }

    CHECK(fixture.diagnostics.Count() == 0);
    StringBuilder<2048> pending(allocator);
    fixture.Pending(pending);
    CHECK(pending.View() ==
          StringView("type Game/Doors/Door.Gate <- Game/Doors/Door.Door\n"
                     "field Game/Doors/Door.Gate.IsOpen <- Game/Doors/Door.Gate.Open\n"));
}

TEST(SparksStableIds_FullKeyFormerlyAfterModuleMove) {
    // 모듈을 옮긴 type은 전체 옛 키로 잇는다. 옛 모듈이 컴파일에 없어도(지운 모듈) 이은 항목은
    // S1005가 아니다.
    SystemAllocator allocator;
    constexpr StringView Manifest = R"json({ "schema": 1, "entries": [
{ "kind": "type", "name": "Game/Old/Place.Door", "id": "00000000000000000000000000000006", "aliases": [], "retired": false },
{ "kind": "field", "name": "Game/Old/Place.Door.Open", "id": "00000000000000000000000000000007", "aliases": [], "retired": false },
{ "kind": "field", "name": "Game/Old/Place.Door.Gone", "id": "00000000000000000000000000000008", "aliases": [], "retired": false }
] })json";
    constexpr StringView Moved =
        R"(@formerly("Game/Old/Place.Door") class Door : ScriptableComponent {
    @save var Open = false;
}
)";
    Fixture fixture(allocator);
    REQUIRE(fixture.Build(Moved, Manifest));
    REQUIRE(fixture.diagnostics.Count() == 1); // Gone만
    CHECK(fixture.diagnostics[0].Code() == DiagnosticCode::DataIdLost);
    StringBuilder<1024> pending(allocator);
    fixture.Pending(pending);
    CHECK(pending.View() == StringView("type Game/Doors/Door.Door <- Game/Old/Place.Door\n"));
}

TEST(SparksStableIds_ReservedNamesS1007) {
    SystemAllocator allocator;
    {
        // 현재 이름 키가 다른 ID의 alias(옛 이름)다. 직접 맞추지 않는다(S1007, 위치는 선언 이름).
        // 그 ID의 현재 이름(Gate)은 선언이 없으므로 S1005다.
        constexpr StringView Manifest = R"json({ "schema": 1, "entries": [
{ "kind": "type", "name": "Game/Doors/Door.Gate", "id": "00000000000000000000000000000006", "aliases": ["Game/Doors/Door.Door"], "retired": false }
] })json";
        Fixture fixture(allocator);
        REQUIRE(fixture.Build(Program, Manifest));
        CHECK(CountCode(fixture, DiagnosticCode::IdNameReserved) == 1);
        CHECK(CountCode(fixture, DiagnosticCode::DataIdLost) == 1);
        CHECK(fixture.diagnostics.Count() == 2);
        for (usize index = 0; index < fixture.diagnostics.Count(); ++index) {
            if (fixture.diagnostics[index].Code() == DiagnosticCode::IdNameReserved) {
                CHECK(LocationOf(fixture, index).line == 7);
                CHECK(LocationOf(fixture, index).column == 7);
            }
        }

        CHECK(!HasPending(fixture, "type Game/Doors/Door.Door"));
        CHECK(HasPending(fixture, "field Game/Doors/Door.Door.Open"));
    }
    {
        // 은퇴한 이름(이름·alias 모두)을 새 선언에 쓰면 S1007이다.
        constexpr StringView Manifest = R"json({ "schema": 1, "entries": [
{ "kind": "type", "name": "Game/Doors/Door.Door", "id": "00000000000000000000000000000006", "aliases": ["Game/Doors/Door.Stats"], "retired": true }
] })json";
        Fixture fixture(allocator);
        REQUIRE(fixture.Build(Program, Manifest));
        CHECK(CountCode(fixture, DiagnosticCode::IdNameReserved) == 2);
        CHECK(fixture.diagnostics.Count() == 2);
    }
}

TEST(SparksStableIds_ClaimedTwiceS1008) {
    SystemAllocator allocator;
    {
        // 옛 이름이 아직 선언돼 있다(Door와 `@formerly("Door") class Portal`).
        StringBuilder<2048> program(allocator);
        program.Append(Program).Append(
            "\n@formerly(\"Door\") class Portal : ScriptableComponent {\n}\n");
        Fixture fixture(allocator);
        REQUIRE(fixture.Build(program.View(), FullManifest));
        REQUIRE(fixture.diagnostics.Count() == 1);
        CHECK(fixture.diagnostics[0].Code() == DiagnosticCode::IdClaimedTwice);
        CHECK(LocationOf(fixture, 0).line == 26);
        // 진단을 낸 선언은 발급 대기에 넣지 않는다(오류가 있는 컴파일은 도구가 발급하지 않는다).
        CHECK(!HasPending(fixture, "type Game/Doors/Door.Portal"));
    }
    {
        // 두 `@formerly`가 같은 옛 이름이다(둘째가 S1008 — 선언 순).
        constexpr StringView Twice = R"(@formerly("Door") class A : ScriptableComponent {
}

@formerly("Door") class B : ScriptableComponent {
}
)";
        constexpr StringView Manifest = R"json({ "schema": 1, "entries": [
{ "kind": "type", "name": "Game/Doors/Door.Door", "id": "00000000000000000000000000000006", "aliases": [], "retired": false }
] })json";
        Fixture fixture(allocator);
        REQUIRE(fixture.Build(Twice, Manifest));
        REQUIRE(fixture.diagnostics.Count() == 1);
        CHECK(fixture.diagnostics[0].Code() == DiagnosticCode::IdClaimedTwice);
        CHECK(LocationOf(fixture, 0).line == 4);
        CHECK(HasPending(fixture, "type Game/Doors/Door.A <- Game/Doors/Door.Door"));
        CHECK(!HasPending(fixture, "type Game/Doors/Door.B"));
    }
}

TEST(SparksStableIds_CarriedMemberRetiredOrClaimed) {
    // type rename이 옮기는 같은 멤버 이름(승계)이 은퇴 항목이면 S1007, `@formerly`가 이미 가져간
    // 항목이면 `@formerly` 쪽에 S1008이다 — 선언 순서와 무관하다(승계가 `@formerly`보다 먼저
    // 맞춘다). 진단 없이 새 발급으로 넘기면 도구의 type rename이 옮긴 항목과 이름이 겹친다.
    SystemAllocator allocator;
    {
        constexpr StringView Manifest = R"json({ "schema": 1, "entries": [
{ "kind": "type", "name": "Game/Doors/Door.Door", "id": "00000000000000000000000000000006", "aliases": [], "retired": false },
{ "kind": "field", "name": "Game/Doors/Door.Door.Old", "id": "00000000000000000000000000000007", "aliases": [], "retired": true }
] })json";
        constexpr StringView Source = R"(@formerly("Door") class Gate : ScriptableComponent {
    var Old = 1;
}
)";
        Fixture fixture(allocator);
        REQUIRE(fixture.Build(Source, Manifest));
        REQUIRE(fixture.diagnostics.Count() == 1);
        CHECK(fixture.diagnostics[0].Code() == DiagnosticCode::IdNameReserved);
        CHECK(LocationOf(fixture, 0).line == 2);
        StringBuilder<1024> pending(allocator);
        fixture.Pending(pending);
        CHECK(pending.View() == StringView("type Game/Doors/Door.Gate <- Game/Doors/Door.Door\n"));
    }

    constexpr StringView Manifest = R"json({ "schema": 1, "entries": [
{ "kind": "type", "name": "Game/Doors/Door.Door", "id": "00000000000000000000000000000006", "aliases": [], "retired": false },
{ "kind": "field", "name": "Game/Doors/Door.Door.Old", "id": "00000000000000000000000000000007", "aliases": [], "retired": false }
] })json";
    constexpr StringView FormerFirst = R"(@formerly("Door") class Gate : ScriptableComponent {
    @formerly("Old") var Fresh = 1;
    var Old = 2;
}
)";
    constexpr StringView CarriedFirst = R"(@formerly("Door") class Gate : ScriptableComponent {
    var Old = 2;
    @formerly("Old") var Fresh = 1;
}
)";
    const StringView sources[] = {FormerFirst, CarriedFirst};
    const uint32 lines[] = {2, 3};
    for (usize at = 0; at < 2; ++at) {
        Fixture fixture(allocator);
        REQUIRE(fixture.Build(sources[at], Manifest));
        REQUIRE(fixture.diagnostics.Count() == 1);
        CHECK(fixture.diagnostics[0].Code() == DiagnosticCode::IdClaimedTwice);
        CHECK(LocationOf(fixture, 0).line == lines[at]);
        StringBuilder<1024> pending(allocator);
        fixture.Pending(pending);
        CHECK(pending.View() == StringView("type Game/Doors/Door.Gate <- Game/Doors/Door.Door\n"));
    }
}

TEST(SparksStableIds_FormerlyThroughAliasUsesCurrentName) {
    // `@formerly`가 alias에 맞아도 발급 대기의 옛 이름은 항목의 현재 이름이다(도구의 rename은 현재
    // 이름으로 찾는다).
    SystemAllocator allocator;
    constexpr StringView Manifest = R"json({ "schema": 1, "entries": [
{ "kind": "type", "name": "Game/Doors/Door.Door", "id": "00000000000000000000000000000006", "aliases": [], "retired": false },
{ "kind": "field", "name": "Game/Doors/Door.Door.Open2", "id": "00000000000000000000000000000007", "aliases": ["Game/Doors/Door.Door.Open"], "retired": false }
] })json";
    constexpr StringView Source = R"(class Door : ScriptableComponent {
    @save @formerly("Open") var IsOpen = false;
}
)";
    Fixture fixture(allocator);
    REQUIRE(fixture.Build(Source, Manifest));
    CHECK(fixture.diagnostics.Count() == 0);
    StringBuilder<1024> pending(allocator);
    fixture.Pending(pending);
    CHECK(pending.View() ==
          StringView("field Game/Doors/Door.Door.IsOpen <- Game/Doors/Door.Door.Open2\n"));
}

TEST(SparksStableIds_InterfaceImplementationsAndSubobjects) {
    // Sparks·엔진 interface를 구현하는 함수(상속한 구현은 그 기반 class 함수)는 FunctionId를
    // 받는다. class의 `let` 필드가 component 생성식이면 subobject, 객체 참조 필드는 field다.
    SystemAllocator allocator;
    constexpr StringView Source = R"(interface Openable {
    fn Toggle();
}

open class Base : ScriptableComponent {
    fn Use() {
    }
}

class Lever : Base, Usable, Openable {
    let Hinge = Part();
    @edit let Target: Lever;
    let Cached: Lever? = null;

    fn Toggle() {
    }

    fn Plain() {
    }
}

class Part : ScriptableComponent {
}
)";
    Fixture fixture(allocator);
    REQUIRE(fixture.Build(Source));
    if (fixture.diagnostics.Count() != 0) {
        fixture.Print();
    }

    CHECK(fixture.diagnostics.Count() == 0);
    StringBuilder<2048> pending(allocator);
    fixture.Pending(pending);
    CHECK(pending.View() == StringView("type Game/Doors/Door.Base\n"
                                       "function Game/Doors/Door.Base.Use\n"
                                       "type Game/Doors/Door.Lever\n"
                                       "subobject Game/Doors/Door.Lever.Hinge\n"
                                       "field Game/Doors/Door.Lever.Target\n"
                                       "field Game/Doors/Door.Lever.Cached\n"
                                       "function Game/Doors/Door.Lever.Toggle\n"
                                       "type Game/Doors/Door.Part\n"));
}

TEST(SparksStableIds_FieldSubobjectReshapeS1005) {
    // field가 subobject로 형태만 바뀌면 S1005이며 메시지가 형태 변경을 알린다.
    SystemAllocator allocator;
    constexpr StringView Manifest = R"json({ "schema": 1, "entries": [
{ "kind": "type", "name": "Game/Doors/Door.Door", "id": "00000000000000000000000000000006", "aliases": [], "retired": false },
{ "kind": "field", "name": "Game/Doors/Door.Door.Hinge", "id": "00000000000000000000000000000007", "aliases": [], "retired": false },
{ "kind": "type", "name": "Game/Doors/Door.Part", "id": "00000000000000000000000000000008", "aliases": [], "retired": false }
] })json";
    constexpr StringView Source = R"(class Door : ScriptableComponent {
    let Hinge = Part();
}

class Part : ScriptableComponent {
}
)";
    Fixture fixture(allocator);
    REQUIRE(fixture.Build(Source, Manifest));
    REQUIRE(fixture.diagnostics.Count() == 1);
    CHECK(fixture.diagnostics[0].Code() == DiagnosticCode::DataIdLost);
    CHECK(fixture.diagnostics[0].Message() ==
          StringView("field 'Game/Doors/Door.Door.Hinge' in the ID manifest is now declared as a "
                     "subobject."));
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
        // 다른 패키지의 alias, 같은 이름 둘(이름·alias·은퇴 이름 사이), 같은 ID 둘, 주인 없는 멤버.
        R"({ "schema": 1, "entries": [ { "kind": "type", "name": "Game/A.B", "id": "00000000000000000000000000000001", "aliases": ["Other/A.B"], "retired": false } ] })",
        R"({ "schema": 1, "entries": [ { "kind": "type", "name": "Game/A.B", "id": "00000000000000000000000000000001", "aliases": [], "retired": false }, { "kind": "type", "name": "Game/A.C", "id": "00000000000000000000000000000002", "aliases": ["Game/A.B"], "retired": false } ] })",
        R"({ "schema": 1, "entries": [ { "kind": "type", "name": "Game/A.B", "id": "00000000000000000000000000000001", "aliases": [], "retired": true }, { "kind": "type", "name": "Game/A.B", "id": "00000000000000000000000000000002", "aliases": [], "retired": false } ] })",
        R"({ "schema": 1, "entries": [ { "kind": "type", "name": "Game/A.B", "id": "00000000000000000000000000000001", "aliases": [], "retired": false }, { "kind": "type", "name": "Game/A.C", "id": "00000000000000000000000000000001", "aliases": [], "retired": false } ] })",
        R"({ "schema": 1, "entries": [ { "kind": "field", "name": "Game/A.B.X", "id": "00000000000000000000000000000001", "aliases": [], "retired": false } ] })",
        R"({ "schema": 1, "entries": [ { "kind": "type", "name": "Game/A.B", "id": "00000000000000000000000000000001", "aliases": [], "retired": true }, { "kind": "field", "name": "Game/A.B.X", "id": "00000000000000000000000000000002", "aliases": [], "retired": false } ] })",
        // 모르는 키(최상위·항목 — C# Load와 같이 거부), 같은 키 둘.
        R"({ "schema": 1, "entries": [], "note": "x" })",
        R"({ "schema": 1, "entries": [ { "kind": "type", "name": "Game/A.B", "id": "00000000000000000000000000000001", "aliases": [], "retired": false, "note": 1 } ] })",
        R"({ "schema": 1, "entries": [ { "kind": "type", "kind": "type", "name": "Game/A.B", "id": "00000000000000000000000000000001", "aliases": [], "retired": false } ] })",
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
