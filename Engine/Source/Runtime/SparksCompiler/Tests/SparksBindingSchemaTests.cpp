#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/String.h>
#include <Core/Text/StringBuilder.h>
#include <Platform/Fatal.h>
#include <SparksCompiler/BindingSchema.h>
#include <SparksCompiler/Diagnostic.h>
#include <SparksCompiler/Source.h>
#include <SparksCompiler/SyntaxTree.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

// binding schema reader 시험이다(Docs/Design/SparksLanguage 10.2절 "binding schema 파일
// 형식", S1120). 시험 schema는 key 하나를 한 줄에 쓰므로 값의 열은 key 길이 + 5다("key": 값).
using namespace rocks;
using namespace rocks::sparks;
namespace sparks_binding_schema_tests_file {

namespace {

constexpr StringView TypeId = "5a7e0000000000000000000000000001";

// 줄 1–5는 최상위, 타입 객체 '{'는 6줄, 타입 key는 7줄부터다.
constexpr StringView RootHead =
    "{\n\"format\": 1,\n\"package\": \"Rocks\",\n\"module\": \"Engine\",\n"
    "\"types\": [\n";

String Join(Allocator& allocator, StringView a, StringView b = StringView(),
            StringView c = StringView(), StringView d = StringView(),
            StringView e = StringView()) noexcept {
    StringBuilder<1024> out(allocator);
    out.Append(a).Append(b).Append(c).Append(d).Append(e);
    String result(allocator);
    static_cast<void>(result.TryAssign(out.View()));
    return result;
}

// 타입 하나의 schema다. lines는 타입 객체 안 key 줄들(7줄부터).
String TypeSchema(Allocator& allocator, StringView lines) noexcept {
    return Join(allocator, RootHead, "{\n", lines, "\n}\n]\n}\n");
}

// Actor 타입(7–9줄) 아래 arrayKey 배열(10줄)의 원소 하나다. 원소 '{'는 11줄, lines는 12줄부터.
String MemberSchema(Allocator& allocator, StringView arrayKey, StringView lines) noexcept {
    StringBuilder<256> head(allocator);
    head.Append("\"name\": \"Actor\",\n\"id\": \"")
        .Append(TypeId)
        .Append("\",\n\"kind\": \"object\",\n\"");
    head.Append(arrayKey).Append("\": [\n{\n");
    const String body = Join(allocator, head.View(), lines, "\n}\n]");
    return TypeSchema(allocator, body.View());
}

// Actor의 함수 하나(11줄 '{', 12 name, 13 id) 아래 arrayKey 배열(14줄) 원소 하나. 원소 '{'는 15줄,
// lines는 16줄부터.
String FunctionPartSchema(Allocator& allocator, StringView arrayKey, StringView lines) noexcept {
    StringBuilder<256> head(allocator);
    head.Append("\"name\": \"F\",\n\"id\": \"5a7e0000000000000000000000000003\",\n\"")
        .Append(arrayKey)
        .Append("\": [\n{\n");
    const String body = Join(allocator, head.View(), lines, "\n}\n]");
    return MemberSchema(allocator, "functions", body.View());
}

struct Read {
    explicit Read(Allocator& allocator) noexcept
        : sources(allocator), diagnostics(allocator), schema(allocator) {
    }

    SourceSet sources;
    DiagnosticList diagnostics;
    BindingSchema schema;
    bool valid = false;
};

bool ReadText(Read& read, StringView text) noexcept {
    auto id = read.sources.TryAdd("Schema/Engine.sparkbind.json", text, read.diagnostics);
    if (!id) {
        return false;
    }

    auto result = ReadBindingSchema(read.sources, id.Value(), read.diagnostics, read.schema);
    if (!result) {
        return false;
    }

    read.valid = result.Value();
    read.diagnostics.Sort(read.sources);
    return true;
}

void Print(Allocator& allocator, StringView text, const Read& read) noexcept {
    platform::WriteDiagnostic(text.Data(), text.ByteCount());
    platform::WriteDiagnostic("\n", 1);
    StringBuilder<512> out(allocator);
    static_cast<void>(AppendDiagnostics(out, read.sources, read.diagnostics));
    platform::WriteDiagnostic(out.View().Data(), out.ByteCount());
}

struct At {
    uint32 line;
    uint32 column;
};

// S1120이 정확히 이 위치들에만 난다.
bool Rejects(StringView text, Span<const At> expected) noexcept {
    SystemAllocator allocator;
    bool same = false;
    {
        Read read(allocator);
        if (ReadText(read, text)) {
            same = !read.valid && read.diagnostics.Count() == expected.Size();
            const SourceFile& file = read.sources.Get(SourceId{0});
            for (usize index = 0; same && index < expected.Size(); ++index) {
                const Diagnostic& diagnostic = read.diagnostics[index];
                const sparks::SourceLocation location = file.Locate(diagnostic.Span().begin);
                same = diagnostic.Code() == DiagnosticCode::InvalidBindingSchema &&
                       location.line == expected[index].line &&
                       location.column == expected[index].column &&
                       diagnostic.Fix() ==
                           StringView("Regenerate the binding schema; do not edit it by hand.");
            }

            if (!same) {
                Print(allocator, text, read);
            }
        }
    }

    return same && allocator.Stats().liveAllocations == 0;
}

bool Rejects(StringView text, uint32 line, uint32 column) noexcept {
    const At expected[] = {{line, column}};
    return Rejects(text, expected);
}

bool Accepts(StringView text) noexcept {
    SystemAllocator allocator;
    bool valid = false;
    {
        Read read(allocator);
        if (ReadText(read, text)) {
            valid = read.valid && read.diagnostics.IsEmpty();
            if (!valid) {
                Print(allocator, text, read);
            }
        }
    }

    return valid && allocator.Stats().liveAllocations == 0;
}

bool FirstSays(StringView text, StringView message) noexcept {
    SystemAllocator allocator;
    bool same = false;
    {
        Read read(allocator);
        if (ReadText(read, text) && !read.diagnostics.IsEmpty()) {
            same = read.diagnostics[0].Message() == message;
            if (!same) {
                Print(allocator, text, read);
            }
        }
    }

    return same && allocator.Stats().liveAllocations == 0;
}

// 한 타입 줄들 앞에 공통 name·id·kind를 붙인다.
String ObjectType(Allocator& allocator, StringView kind, StringView extra) noexcept {
    StringBuilder<256> lines(allocator);
    lines.Append("\"name\": \"T\",\n\"id\": \"").Append(TypeId).Append("\",\n\"kind\": \"");
    lines.Append(kind).Append("\"");
    if (!extra.IsEmpty()) {
        lines.Append(",\n").Append(extra);
    }

    return TypeSchema(allocator, lines.View());
}

} // namespace

TEST(SparksBindingSchema_ReadsEveryKind) {
    SystemAllocator allocator;
    {
        Read read(allocator);
        REQUIRE(ReadText(read, R"({
  "format": 1,
  "package": "Rocks",
  "module": "Engine",
  "types": [
    { "name": "Actor", "id": "5a7e0000000000000000000000000001", "kind": "object",
      "scriptExtensible": true,
      "fields": [
        { "name": "Location", "type": "vec3", "access": "readWrite", "computed": true },
        { "name": "Tags", "id": "5a7e0000000000000000000000000002", "type": "TagSet",
          "changed": true },
        { "name": "Health", "id": "5a7e0000000000000000000000000003", "type": "int32",
          "access": "read" }
      ],
      "functions": [
        { "name": "Find", "id": "5a7e0000000000000000000000000004",
          "typeParameters": [ { "name": "T", "constraint": "Component" }, { "name": "U" } ],
          "returns": "T?" },
        { "name": "Move", "id": "5a7e0000000000000000000000000005", "static": true,
          "async": true,
          "parameters": [ { "name": "velocity", "type": "vec3", "perSecond": true },
                          { "name": "targets", "type": "Map<string, Array<Actor>>" } ] }
      ],
      "events": [
        { "name": "Died", "id": "5a7e0000000000000000000000000006", "publisher": "server",
          "parameters": [ { "name": "killer", "type": "Actor?" } ] },
        { "name": "Hit", "id": "5a7e0000000000000000000000000007", "publisher": "owner" },
        { "name": "Seen", "id": "5a7e0000000000000000000000000008", "publisher": "all" }
      ] },
    { "name": "Pawn", "id": "5a7e0000000000000000000000000009", "kind": "object",
      "base": "Actor" },
    { "name": "Mesh", "id": "5a7e000000000000000000000000000a", "kind": "asset",
      "base": "Actor" },
    { "name": "Hit", "id": "5a7e000000000000000000000000000b", "kind": "struct",
      "constructible": false,
      "fields": [ { "name": "Normal", "id": "5a7e000000000000000000000000000c", "type": "vec3" } ],
      "functions": [ { "name": "Length", "id": "5a7e000000000000000000000000000d",
                       "returns": "float" } ] },
    { "name": "State", "id": "5a7e000000000000000000000000000e", "kind": "enum",
      "cases": [ "Closed", "Open" ] },
    { "name": "Usable", "id": "5a7e000000000000000000000000000f", "kind": "interface",
      "base": "Thing",
      "functions": [ { "name": "Use", "id": "5a7e0000000000000000000000000010" } ] }
  ]
}
)"));
        CHECK(read.valid);
        CHECK(read.diagnostics.IsEmpty());
        const BindingSchema& schema = read.schema;
        CHECK(schema.Package() == StringView("Rocks"));
        CHECK(schema.Module() == StringView("Engine"));
        REQUIRE(schema.Types().Size() == 6);
        const SchemaType& actor = schema.Types()[0];
        CHECK(schema.Text(actor.name) == StringView("Actor"));
        CHECK(schema.Text(actor.id) == StringView("5a7e0000000000000000000000000001"));
        CHECK(actor.kind == SchemaTypeKind::Object);
        CHECK(actor.scriptExtensible);
        CHECK(actor.base.length == 0);
        REQUIRE(actor.fieldCount == 3);
        const SchemaField& location = schema.Fields()[actor.firstField];
        CHECK(schema.Text(location.name) == StringView("Location"));
        CHECK(location.computed && location.readWrite && !location.changed);
        CHECK(location.id.length == 0);
        CHECK(schema.Text(location.type) == StringView("vec3"));
        const SchemaField& tags = schema.Fields()[actor.firstField + 1];
        CHECK(tags.changed && !tags.readWrite && !tags.computed);
        CHECK(!schema.Fields()[actor.firstField + 2].readWrite);
        REQUIRE(actor.functionCount == 2);
        const SchemaFunction& find = schema.Functions()[actor.firstFunction];
        CHECK(schema.Text(find.returns) == StringView("T?"));
        CHECK(!find.isStatic && !find.isAsync);
        REQUIRE(find.typeParameterCount == 2);
        CHECK(schema.Text(schema.TypeParameters()[find.firstTypeParameter].constraint) ==
              StringView("Component"));
        CHECK(schema.TypeParameters()[find.firstTypeParameter + 1].constraint.length == 0);
        const SchemaFunction& move = schema.Functions()[actor.firstFunction + 1];
        CHECK(move.isStatic && move.isAsync);
        CHECK(move.returns.length == 0);
        REQUIRE(move.parameterCount == 2);
        CHECK(schema.Parameters()[move.firstParameter].perSecond);
        CHECK(schema.Text(schema.Parameters()[move.firstParameter + 1].type) ==
              StringView("Map<string, Array<Actor>>"));
        REQUIRE(actor.eventCount == 3);
        CHECK(schema.Events()[actor.firstEvent].publisher == SchemaPublisher::Server);
        CHECK(schema.Events()[actor.firstEvent + 1].publisher == SchemaPublisher::Owner);
        CHECK(schema.Events()[actor.firstEvent + 2].publisher == SchemaPublisher::All);
        REQUIRE(schema.Events()[actor.firstEvent].parameterCount == 1);
        CHECK(schema.Text(schema.Types()[1].base) == StringView("Actor"));
        CHECK(schema.Types()[2].kind == SchemaTypeKind::Asset);
        CHECK(schema.Types()[3].kind == SchemaTypeKind::Struct);
        CHECK(!schema.Types()[3].constructible);
        CHECK(schema.Types()[1].constructible); // struct가 아니면 기본값 그대로
        const SchemaType& state = schema.Types()[4];
        CHECK(state.kind == SchemaTypeKind::Enum);
        REQUIRE(state.caseCount == 2);
        CHECK(schema.Text(schema.Cases()[state.firstCase + 1].name) == StringView("Open"));
        CHECK(schema.Types()[5].kind == SchemaTypeKind::Interface);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SparksBindingSchema_StableIdText) {
    CHECK(IsStableIdText("0123456789abcdef0123456789abcdef"));
    CHECK(!IsStableIdText("0123456789ABCDEF0123456789abcdef"));
    CHECK(!IsStableIdText("0123456789abcdef0123456789abcde"));
    CHECK(!IsStableIdText("0123456789abcdef0123456789abcdef0"));
    CHECK(!IsStableIdText("0123456789abcdef0123456789abcdeg"));
    CHECK(!IsStableIdText(""));
    // 모두 0은 안 된다(`.rocksids.json`과 같다).
    CHECK(!IsStableIdText("00000000000000000000000000000000"));
    CHECK(IsStableIdText("00000000000000000000000000000001"));
    CHECK(IsStableIdText("10000000000000000000000000000000"));
    CHECK(!IsStableIdText("0123456789abcdef-123456789abcdef"));
    CHECK(!IsStableIdText("0123456789abcdef 123456789abcdef"));
    SystemAllocator allocator;
    {
        auto type = [&](StringView id) noexcept {
            return TypeSchema(allocator, Join(allocator, "\"name\": \"T\",\n\"id\": \"", id,
                                              "\",\n\"kind\": \"object\"")
                                             .View());
        };
        CHECK(Rejects(type("00000000000000000000000000000000").View(), 8, 7));
        CHECK(Rejects(type("5A7E0000000000000000000000000001").View(), 8, 7));
        CHECK(Accepts(type("00000000000000000000000000000001").View()));
        CHECK(FirstSays(type("00000000000000000000000000000000").View(),
                        "'id' must be 32 lowercase hexadecimal digits, not all zero."));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SparksBindingSchema_RootS1120) {
    CHECK(Rejects("{", 1, 2));
    CHECK(Rejects("// generated\n{}", 1, 1)); // 엄격한 JSON: 주석 없음
    CHECK(Rejects("[]", 1, 1));
    CHECK(Rejects("{\n\"format\": 1,\n\"package\": \"Rocks\",\n\"module\": \"Engine\",\n"
                  "\"types\": [],\n\"extra\": 1\n}",
                  6, 10));
    // 빠진 key마다 하나(객체의 시작).
    const At missing[] = {{1, 1}, {1, 1}, {1, 1}, {1, 1}};
    CHECK(Rejects("{}", missing));
    CHECK(Rejects("{\n\"format\": 2,\n\"package\": \"Rocks\",\n\"module\": \"Engine\",\n"
                  "\"types\": []\n}",
                  2, 11));
    CHECK(Rejects("{\n\"format\": \"1\",\n\"package\": \"Rocks\",\n\"module\": \"Engine\",\n"
                  "\"types\": []\n}",
                  2, 11));
    CHECK(Rejects("{\n\"format\": 1,\n\"package\": \"Game\",\n\"module\": \"Engine\",\n"
                  "\"types\": []\n}",
                  3, 12));
    CHECK(Rejects("{\n\"format\": 1,\n\"package\": \"Rocks\",\n\"module\": \"engine\",\n"
                  "\"types\": []\n}",
                  4, 11));
    CHECK(Rejects("{\n\"format\": 1,\n\"package\": \"Rocks\",\n\"module\": \"Engine\",\n"
                  "\"types\": {}\n}",
                  5, 10));
    CHECK(Accepts("{\n\"format\": 1,\n\"package\": \"Rocks\",\n\"module\": \"Engine\",\n"
                  "\"types\": []\n}"));
    CHECK(FirstSays("{\n\"format\": 1,\n\"package\": \"Game\",\n\"module\": \"Engine\",\n"
                    "\"types\": []\n}",
                    "The binding schema package must be 'Rocks'."));
    // 인쇄할 수 없는 key는 문장에 넣지 않는다.
    CHECK(FirstSays("{\n\"format\": 1,\n\"package\": \"Rocks\",\n\"module\": \"Engine\",\n"
                    "\"types\": [],\n\"a\\u0001b\": 1\n}",
                    "Key is not a key of a binding schema."));
}

TEST(SparksBindingSchema_TypeS1120) {
    SystemAllocator allocator;
    {
        CHECK(Accepts(ObjectType(allocator, "object", "").View()));
        CHECK(Rejects(Join(allocator, RootHead, "1\n]\n}").View(), 6, 1));
        // 알 수 없는 key·빠진 key.
        CHECK(Rejects(ObjectType(allocator, "object", "\"bad\": 1").View(), 10, 8));
        CHECK(Rejects(TypeSchema(allocator, "\"id\": \"5a7e0000000000000000000000000001\",\n"
                                            "\"kind\": \"object\"")
                          .View(),
                      6, 1));
        CHECK(
            Rejects(TypeSchema(allocator, "\"name\": \"T\",\n\"kind\": \"object\"").View(), 6, 1));
        CHECK(Rejects(TypeSchema(allocator, "\"name\": \"T\",\n"
                                            "\"id\": \"5a7e0000000000000000000000000001\"")
                          .View(),
                      6, 1));
        // 값.
        CHECK(Rejects(TypeSchema(allocator, "\"name\": \"t\",\n"
                                            "\"id\": \"5a7e0000000000000000000000000001\",\n"
                                            "\"kind\": \"object\"")
                          .View(),
                      7, 9));
        CHECK(Rejects(TypeSchema(allocator, "\"name\": \"T\",\n\"id\": \"5A7E\",\n"
                                            "\"kind\": \"object\"")
                          .View(),
                      8, 7));
        CHECK(Rejects(ObjectType(allocator, "class", "").View(), 9, 9));
        CHECK(Rejects(ObjectType(allocator, "object", "\"base\": \"actor\"").View(), 10, 9));
        CHECK(Rejects(ObjectType(allocator, "object", "\"scriptExtensible\": 1").View(), 10, 21));
        // kind마다 허용하지 않는 key.
        // struct는 기반 struct를 가질 수 있다(EI1a — 기반 종류는 이름 해석이 본다).
        CHECK(Accepts(ObjectType(allocator, "struct", "\"base\": \"Actor\"").View()));
        CHECK(
            Rejects(ObjectType(allocator, "struct", "\"scriptExtensible\": true").View(), 10, 21));
        CHECK(Rejects(ObjectType(allocator, "object", "\"constructible\": false").View(), 10, 18));
        CHECK(Rejects(ObjectType(allocator, "interface", "\"fields\": []").View(), 10, 11));
        CHECK(
            Rejects(ObjectType(allocator, "enum", "\"cases\": [\"A\"],\n\"functions\": []").View(),
                    11, 14));
        CHECK(Rejects(ObjectType(allocator, "struct", "\"events\": []").View(), 10, 11));
        CHECK(Rejects(ObjectType(allocator, "object", "\"cases\": [\"A\"]").View(), 10, 10));
        CHECK(Accepts(
            ObjectType(allocator, "interface", "\"base\": \"Usable\",\n\"functions\": []").View()));
        CHECK(Accepts(
            ObjectType(allocator, "asset", "\"scriptExtensible\": true,\n\"fields\": []").View()));
        // enum case.
        CHECK(Rejects(ObjectType(allocator, "enum", "").View(), 6, 1));
        CHECK(Rejects(ObjectType(allocator, "enum", "\"cases\": []").View(), 10, 10));
        CHECK(Rejects(ObjectType(allocator, "enum", "\"cases\": [\n\"open\"\n]").View(), 11, 1));
        CHECK(
            Rejects(ObjectType(allocator, "enum", "\"cases\": [\n\"A\",\n\"A\"\n]").View(), 12, 1));
        CHECK(Rejects(ObjectType(allocator, "enum", "\"cases\": \"A\"").View(), 10, 10));
        CHECK(FirstSays(ObjectType(allocator, "enum", "\"cases\": [\n\"A\",\n\"A\"\n]").View(),
                        "Enum case 'A' is listed twice."));
        CHECK(FirstSays(ObjectType(allocator, "object", "\"bad\": 1").View(),
                        "Key 'bad' is not a key of a type."));
        CHECK(FirstSays(ObjectType(allocator, "struct", "\"events\": []").View(),
                        "'events' is only for object types."));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 같은 파일 안 타입 이름·ID 중복(뒤에 나온 쪽).
TEST(SparksBindingSchema_DuplicatesS1120) {
    CHECK(Rejects(R"({
"format": 1,
"package": "Rocks",
"module": "Engine",
"types": [
{ "name": "A", "id": "5a7e0000000000000000000000000001", "kind": "object" },
{ "name": "A", "id": "5a7e0000000000000000000000000002", "kind": "object" }
]
})",
                  7, 11));
    CHECK(Rejects(R"({
"format": 1,
"package": "Rocks",
"module": "Engine",
"types": [
{ "name": "A", "id": "5a7e0000000000000000000000000001", "kind": "object",
  "fields": [ { "name": "X", "id": "5a7e0000000000000000000000000001", "type": "int" } ] }
]
})",
                  7, 36));
    // 필드·함수·이벤트는 한 이름 공간이다.
    const At namespaceClash[] = {{8, 28}, {9, 25}};
    CHECK(Rejects(R"({
"format": 1,
"package": "Rocks",
"module": "Engine",
"types": [
{ "name": "A", "id": "5a7e0000000000000000000000000001", "kind": "object",
  "fields": [ { "name": "X", "id": "5a7e0000000000000000000000000002", "type": "int" } ],
  "functions": [ { "name": "X", "id": "5a7e0000000000000000000000000003" } ],
  "events": [ { "name": "X", "id": "5a7e0000000000000000000000000004" } ] }
]
})",
                  namespaceClash));
    CHECK(FirstSays(R"({
"format": 1,
"package": "Rocks",
"module": "Engine",
"types": [
{ "name": "A", "id": "5a7e0000000000000000000000000001", "kind": "object" },
{ "name": "B", "id": "5a7e0000000000000000000000000001", "kind": "object" }
]
})",
                    "This ID is used twice in the binding schema."));
}

TEST(SparksBindingSchema_FieldS1120) {
    SystemAllocator allocator;
    {
        constexpr StringView Id = "\"id\": \"5a7e0000000000000000000000000002\"";
        CHECK(Accepts(
            MemberSchema(allocator, "fields",
                         Join(allocator, "\"name\": \"X\",\n\"type\": \"int\",\n", Id).View())
                .View()));
        CHECK(Rejects(ObjectType(allocator, "object", "\"fields\": [1]").View(), 10, 12));
        CHECK(Rejects(MemberSchema(allocator, "fields",
                                   Join(allocator, "\"name\": \"X\",\n\"type\": \"int\",\n", Id,
                                        ",\n\"bad\": 1")
                                       .View())
                          .View(),
                      15, 8));
        CHECK(Rejects(
            MemberSchema(allocator, "fields", Join(allocator, "\"type\": \"int\",\n", Id).View())
                .View(),
            11, 1));
        CHECK(Rejects(
            MemberSchema(allocator, "fields", Join(allocator, "\"name\": \"X\",\n", Id).View())
                .View(),
            11, 1));
        // id: computed가 아니면 필수, computed면 없음.
        CHECK(
            Rejects(MemberSchema(allocator, "fields", "\"name\": \"X\",\n\"type\": \"int\"").View(),
                    11, 1));
        CHECK(Rejects(MemberSchema(allocator, "fields",
                                   Join(allocator, "\"name\": \"X\",\n\"type\": \"int\",\n", Id,
                                        ",\n\"computed\": true")
                                       .View())
                          .View(),
                      14, 7));
        CHECK(Accepts(MemberSchema(allocator, "fields",
                                   "\"name\": \"X\",\n\"type\": \"int\",\n\"computed\": true")
                          .View()));
        CHECK(Rejects(
            MemberSchema(allocator, "fields",
                         Join(allocator, "\"name\": \"x\",\n\"type\": \"int\",\n", Id).View())
                .View(),
            12, 9));
        CHECK(Rejects(
            MemberSchema(allocator, "fields",
                         Join(allocator, "\"name\": \"X\",\n\"type\": \"Array<\",\n", Id).View())
                .View(),
            13, 9));
        CHECK(Rejects(MemberSchema(allocator, "fields",
                                   Join(allocator, "\"name\": \"X\",\n\"type\": \"int\",\n", Id,
                                        ",\n\"access\": \"write\"")
                                       .View())
                          .View(),
                      15, 11));
        CHECK(Rejects(MemberSchema(allocator, "fields",
                                   Join(allocator, "\"name\": \"X\",\n\"type\": \"int\",\n", Id,
                                        ",\n\"changed\": \"yes\"")
                                       .View())
                          .View(),
                      15, 12));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 타입 표기 문법(10.2절): 이름 {'.' 이름} ['<' 표기 {", " 표기} '>'] ['?'].
TEST(SparksBindingSchema_TypeNotation) {
    SystemAllocator allocator;
    {
        auto field = [&](StringView type) noexcept {
            const String lines = Join(allocator, "\"name\": \"X\",\n\"type\": \"", type,
                                      "\",\n\"id\": \"5a7e0000000000000000000000000002\"");
            return MemberSchema(allocator, "fields", lines.View());
        };
        constexpr StringView Valid[] = {
            "int32", "Actor?", "Array<Actor>", "Map<string, Array<int>>", "Result<void>",
            "Weapons.Sword", "T?", "Array<Array<Array<int>>>?", "A1", "Map<string, int32>",
            // 키워드는 대소문자를 구분한다.
            "In", "Classy"};
        for (const StringView type : Valid) {
            CHECK(Accepts(field(type).View()));
        }

        constexpr StringView Invalid[] = {
            "", "1int", "int ?", "Array<int", "Array<>", "Map<int,int>", "Map<int,  int>", "A..B",
            "A.", ".A", "A??", "Array<int>>", "a_b", "Array <int>", "int?x", " int",
            // 이름 구간은 키워드일 수 없다(3.1절).
            "null", "Array<class>", "Weapons.in", "Map<string, fn>", "this?",
            // 타입 인자에는 `?`를 쓸 수 없다(SL 4.2절 — EI1a).
            "Array<Actor?>", "Map<string, Actor?>", "Map<Actor?, int>", "Array<Array<int?>>?",
            "Soft<T?>"};
        for (const StringView type : Invalid) {
            CHECK(Rejects(field(type).View(), 13, 9));
        }

        // 중첩 상한(MaxSyntaxNesting).
        StringBuilder<1024> deep(allocator);
        StringBuilder<1024> tooDeep(allocator);
        for (uint32 index = 0; index < MaxSyntaxNesting; ++index) {
            deep.Append("Array<");
        }

        tooDeep.Append(deep.View()).Append("Array<");
        deep.Append("int");
        tooDeep.Append("int");
        for (uint32 index = 0; index < MaxSyntaxNesting; ++index) {
            deep.Append(">");
            tooDeep.Append(">");
        }

        tooDeep.Append(">");
        CHECK(Accepts(field(deep.View()).View()));
        CHECK(Rejects(field(tooDeep.View()).View(), 13, 9));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 효과·유지 자원 key(LC3b·LC3c — SL 3.13·3.14절): `effects`·`pure`·`oneShot`(함수), `held`(object
// 타입).
TEST(SparksBindingSchema_EffectKeysS1120) {
    SystemAllocator allocator;
    {
        constexpr StringView Id = "\"id\": \"5a7e0000000000000000000000000003\"";
        auto function = [&](StringView extra) noexcept {
            return MemberSchema(allocator, "functions",
                                Join(allocator, "\"name\": \"F\",\n", Id, extra).View());
        };
        CHECK(Accepts(function(",\n\"effects\": [\"spawn\", \"io\", \"wallClock\"]").View()));
        CHECK(Accepts(function(",\n\"pure\": true").View()));
        CHECK(Accepts(function(",\n\"oneShot\": true").View()));
        CHECK(Rejects(function(",\n\"effects\": [\"fly\"]").View(), 14, 13));
        CHECK(Rejects(function(",\n\"effects\": [\"io\", \"io\"]").View(), 14, 19));
        CHECK(Rejects(function(",\n\"effects\": \"io\"").View(), 14, 12));
        CHECK(Rejects(function(",\n\"pure\": true,\n\"oneShot\": true").View(), 15, 12));
        CHECK(Rejects(function(",\n\"pure\": true,\n\"effects\": [\"spawn\"]").View(), 15, 13));
        constexpr StringView Head =
            "\"name\": \"Loop\",\n\"id\": \"5a7e0000000000000000000000000002\",\n\"kind\": ";
        CHECK(Accepts(
            TypeSchema(allocator, Join(allocator, Head, "\"object\",\n\"held\": true").View())
                .View()));
        CHECK(Rejects(
            TypeSchema(allocator, Join(allocator, Head, "\"struct\",\n\"held\": true").View())
                .View(),
            10, 9));
        CHECK(Rejects(
            TypeSchema(allocator, Join(allocator, Head,
                                       "\"object\",\n\"scriptExtensible\": true,\n\"held\": true")
                                      .View())
                .View(),
            11, 9));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SparksBindingSchema_FunctionS1120) {
    SystemAllocator allocator;
    {
        constexpr StringView Id = "\"id\": \"5a7e0000000000000000000000000003\"";
        CHECK(Accepts(
            MemberSchema(allocator, "functions", Join(allocator, "\"name\": \"F\",\n", Id).View())
                .View()));
        CHECK(Rejects(MemberSchema(allocator, "functions", "\"name\": \"F\"").View(), 11, 1));
        CHECK(Rejects(
            MemberSchema(allocator, "functions",
                         Join(allocator, "\"name\": \"F\",\n", Id, ",\n\"static\": 0").View())
                .View(),
            14, 11));
        CHECK(Rejects(
            MemberSchema(allocator, "functions",
                         Join(allocator, "\"name\": \"F\",\n", Id, ",\n\"async\": null").View())
                .View(),
            14, 10));
        CHECK(Rejects(
            MemberSchema(
                allocator, "functions",
                Join(allocator, "\"name\": \"F\",\n", Id, ",\n\"returns\": \"Array<\"").View())
                .View(),
            14, 12));
        CHECK(Rejects(
            MemberSchema(allocator, "functions",
                         Join(allocator, "\"name\": \"F\",\n", Id, ",\n\"parameters\": {}").View())
                .View(),
            14, 15));
        CHECK(Rejects(
            MemberSchema(
                allocator, "functions",
                Join(allocator, "\"name\": \"F\",\n", Id, ",\n\"typeParameters\": [1]").View())
                .View(),
            14, 20));
        // 매개변수(16줄부터).
        CHECK(Accepts(FunctionPartSchema(allocator, "parameters",
                                         "\"name\": \"amount\",\n\"type\": \"int\",\n"
                                         "\"perSecond\": true")
                          .View()));
        CHECK(Rejects(
            FunctionPartSchema(allocator, "parameters", "\"name\": \"Amount\",\n\"type\": \"int\"")
                .View(),
            16, 9));
        CHECK(Rejects(FunctionPartSchema(allocator, "parameters",
                                         "\"name\": \"amount\",\n\"type\": \"int?x\"")
                          .View(),
                      17, 9));
        CHECK(Rejects(FunctionPartSchema(allocator, "parameters",
                                         "\"name\": \"amount\",\n\"type\": \"int\",\n"
                                         "\"perSecond\": 1")
                          .View(),
                      18, 14));
        CHECK(Rejects(FunctionPartSchema(allocator, "parameters",
                                         "\"name\": \"amount\",\n\"type\": \"int\",\n\"bad\": 1")
                          .View(),
                      18, 8));
        CHECK(Rejects(FunctionPartSchema(allocator, "parameters", "\"name\": \"amount\"").View(),
                      15, 1));
        CHECK(Rejects(
            MemberSchema(allocator, "functions",
                         Join(allocator, "\"name\": \"F\",\n", Id, ",\n\"parameters\": [2]").View())
                .View(),
            14, 16));
        CHECK(Rejects(FunctionPartSchema(allocator, "parameters",
                                         "\"name\": \"a\",\n\"type\": \"int\"\n},\n{\n"
                                         "\"name\": \"a\",\n\"type\": \"int\"")
                          .View(),
                      20, 9));
        // 타입 매개변수.
        CHECK(Accepts(FunctionPartSchema(allocator, "typeParameters",
                                         "\"name\": \"T\",\n\"constraint\": \"Component\"")
                          .View()));
        CHECK(Rejects(FunctionPartSchema(allocator, "typeParameters", "\"name\": \"t\"").View(), 16,
                      9));
        CHECK(Rejects(FunctionPartSchema(allocator, "typeParameters",
                                         "\"name\": \"T\",\n\"constraint\": \"component\"")
                          .View(),
                      17, 15));
        CHECK(
            Rejects(FunctionPartSchema(allocator, "typeParameters", "\"constraint\": \"A\"").View(),
                    15, 1));
        CHECK(Rejects(FunctionPartSchema(allocator, "typeParameters",
                                         "\"name\": \"T\"\n},\n{\n\"name\": \"T\"")
                          .View(),
                      19, 9));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SparksBindingSchema_EventS1120) {
    SystemAllocator allocator;
    {
        constexpr StringView Id = "\"id\": \"5a7e0000000000000000000000000004\"";
        CHECK(Accepts(MemberSchema(allocator, "events",
                                   Join(allocator, "\"name\": \"Died\",\n", Id,
                                        ",\n\"publisher\": \"server\",\n\"parameters\": [\n"
                                        "{ \"name\": \"killer\", \"type\": \"Actor?\" }\n]")
                                       .View())
                          .View()));
        CHECK(Rejects(MemberSchema(allocator, "events",
                                   Join(allocator, "\"name\": \"Died\",\n", Id,
                                        ",\n\"publisher\": \"clients\"")
                                       .View())
                          .View(),
                      14, 14));
        // 이벤트 매개변수에는 perSecond가 없다.
        CHECK(Rejects(
            MemberSchema(allocator, "events",
                         Join(allocator, "\"name\": \"Died\",\n", Id,
                              ",\n\"parameters\": [\n"
                              "{ \"name\": \"a\", \"type\": \"int\", \"perSecond\": true }\n]")
                             .View())
                .View(),
            15, 44));
        CHECK(Rejects(MemberSchema(allocator, "events", "\"name\": \"Died\"").View(), 11, 1));
        CHECK(Rejects(ObjectType(allocator, "object", "\"events\": [true]").View(), 10, 12));
        CHECK(Rejects(ObjectType(allocator, "object", "\"functions\": [\"F\"]").View(), 10, 15));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 오류가 여럿이면 모두 알린다.
// kind × kind가 제한하는 key 전체다. 허용하지 않으면 그 key의 진단 하나로 끝나고 값(틀린 값 5)은
// 검사하지 않는다. 허용하면 틀린 값의 진단 하나다.
TEST(SparksBindingSchema_KindRestrictedKeys) {
    struct Restriction {
        StringView key;
        StringView only;       // "is only for <only> types."
        StringView valueError; // 허용할 때 틀린 값 5의 문장
        StringView allowed[4]; // 허용하는 kind(빈 칸은 없음)
    };

    constexpr Restriction Restrictions[] = {
        {"base",
         "object, asset, interface and struct",
         "'base' must be a PascalCase name.",
         {"object", "asset", "interface", "struct"}},
        {"scriptExtensible",
         "object and asset",
         "'scriptExtensible' must be true or false.",
         {"object", "asset", "", ""}},
        {"constructible",
         "struct",
         "'constructible' must be true or false.",
         {"struct", "", "", ""}},
        {"fields",
         "object, asset and struct",
         "'fields' must be an array.",
         {"object", "asset", "struct", ""}},
        {"functions",
         "non-enum",
         "'functions' must be an array.",
         {"object", "asset", "struct", "interface"}},
        {"events", "object", "'events' must be an array.", {"object", "", "", ""}},
        {"cases", "enum", "'cases' must be an array.", {"enum", "", "", ""}},
    };
    constexpr StringView Kinds[] = {"object", "asset", "struct", "enum", "interface"};
    SystemAllocator allocator;
    {
        for (const StringView kind : Kinds) {
            for (const Restriction& restriction : Restrictions) {
                bool allowed = false;
                for (const StringView candidate : restriction.allowed) {
                    allowed = allowed || candidate == kind;
                }

                // enum은 cases가 있어야 하므로 앞 줄(10줄)에 두고 시험 key를 11줄에 둔다.
                const bool enumCases = kind == "enum" && restriction.key != "cases";
                const String extra = Join(allocator, enumCases ? "\"cases\": [\"A\"],\n\"" : "\"",
                                          restriction.key, "\": 5");
                const String text = ObjectType(allocator, kind, extra.View());
                const uint32 line = enumCases ? 11 : 10;
                const uint32 column = static_cast<uint32>(restriction.key.ByteCount()) + 5;
                CHECK(Rejects(text.View(), line, column));
                if (allowed) {
                    CHECK(FirstSays(text.View(), restriction.valueError));
                } else {
                    const String only = Join(allocator, "'", restriction.key, "' is only for ",
                                             restriction.only, " types.");
                    CHECK(FirstSays(text.View(), only.View()));
                }
            }
        }

        // kind를 모르면 어느 key도 거부하지 않고 값을 검사한다(case 개수 규칙은 enum만).
        CHECK(Rejects(ObjectType(allocator, "thing", "\"cases\": []").View(), 9, 9));
        const At unknownKind[] = {{9, 9}, {10, 9}};
        CHECK(Rejects(ObjectType(allocator, "thing", "\"base\": \"x\"").View(), unknownKind));
        // 빠진 kind(객체 시작)와 틀린 events 값.
        const At missingKind[] = {{6, 1}, {9, 11}};
        CHECK(Rejects(TypeSchema(allocator, "\"name\": \"T\",\n"
                                            "\"id\": \"5a7e0000000000000000000000000001\",\n"
                                            "\"events\": 5")
                          .View(),
                      missingKind));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 세 번 나온 중복은 진단 둘이다(원문에서 뒤에 나온 것마다 하나).
TEST(SparksBindingSchema_RepeatedDuplicates) {
    const At types[] = {{7, 11}, {8, 11}};
    CHECK(Rejects(R"({
"format": 1,
"package": "Rocks",
"module": "Engine",
"types": [
{ "name": "A", "id": "5a7e0000000000000000000000000001", "kind": "object" },
{ "name": "A", "id": "5a7e0000000000000000000000000002", "kind": "object" },
{ "name": "A", "id": "5a7e0000000000000000000000000003", "kind": "object" }
]
})",
                  types));
    const At ids[] = {{7, 22}, {8, 22}};
    CHECK(Rejects(R"({
"format": 1,
"package": "Rocks",
"module": "Engine",
"types": [
{ "name": "A", "id": "5a7e0000000000000000000000000001", "kind": "object" },
{ "name": "B", "id": "5a7e0000000000000000000000000001", "kind": "object" },
{ "name": "C", "id": "5a7e0000000000000000000000000001", "kind": "object" }
]
})",
                  ids));
    const At members[] = {{9, 15}, {10, 15}};
    CHECK(Rejects(R"({
"format": 1,
"package": "Rocks",
"module": "Engine",
"types": [
{ "name": "A", "id": "5a7e0000000000000000000000000001", "kind": "object", "fields": [
    { "name": "X", "id": "5a7e0000000000000000000000000002", "type": "int" } ],
  "functions": [
    { "name": "X", "id": "5a7e0000000000000000000000000003" },
    { "name": "X", "id": "5a7e0000000000000000000000000004" } ] }
]
})",
                  members));
    const At parameters[] = {{9, 15}, {10, 15}};
    CHECK(Rejects(R"({
"format": 1,
"package": "Rocks",
"module": "Engine",
"types": [
{ "name": "A", "id": "5a7e0000000000000000000000000001", "kind": "object", "functions": [
  { "name": "F", "id": "5a7e0000000000000000000000000002", "parameters": [
    { "name": "a", "type": "int" },
    { "name": "a", "type": "int" },
    { "name": "a", "type": "int" } ] } ] }
]
})",
                  parameters));
    const At typeParameters[] = {{9, 15}, {10, 15}};
    CHECK(Rejects(R"({
"format": 1,
"package": "Rocks",
"module": "Engine",
"types": [
{ "name": "A", "id": "5a7e0000000000000000000000000001", "kind": "object", "functions": [
  { "name": "F", "id": "5a7e0000000000000000000000000002", "typeParameters": [
    { "name": "T" },
    { "name": "T" },
    { "name": "T" } ] } ] }
]
})",
                  typeParameters));
    const At cases[] = {{8, 1}, {9, 1}};
    CHECK(Rejects(R"({
"format": 1,
"package": "Rocks",
"module": "Engine",
"types": [
{ "name": "E", "id": "5a7e0000000000000000000000000001", "kind": "enum", "cases": [
"A",
"A",
"A" ] }
]
})",
                  cases));
}

// 매개변수 이름은 키워드일 수 없다(lowerCamel이므로 겹칠 수 있다).
TEST(SparksBindingSchema_KeywordNames) {
    SystemAllocator allocator;
    {
        constexpr StringView Keywords[] = {"in", "on", "default"};
        for (const StringView name : Keywords) {
            const String lines = Join(allocator, "\"name\": \"", name, "\",\n\"type\": \"int\"");
            CHECK(Rejects(FunctionPartSchema(allocator, "parameters", lines.View()).View(), 16, 9));
            CHECK(FirstSays(FunctionPartSchema(allocator, "parameters", lines.View()).View(),
                            "'name' must not be a keyword."));
            CHECK(Rejects(MemberSchema(allocator, "events",
                                       Join(allocator,
                                            "\"name\": \"E\",\n\"id\": "
                                            "\"5a7e0000000000000000000000000003\",\n"
                                            "\"parameters\": [\n{\n",
                                            lines.View(), "\n}\n]")
                                           .View())
                              .View(),
                          16, 9));
        }

        CHECK(Accepts(
            FunctionPartSchema(allocator, "parameters", "\"name\": \"inside\",\n\"type\": \"int\"")
                .View()));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 문자열이어야 하는 값에 다른 종류가 오거나 이름이 빠진 요소다(진단 하나씩).
TEST(SparksBindingSchema_ValueKinds) {
    SystemAllocator allocator;
    {
        CHECK(Rejects("{\n\"format\": 1,\n\"package\": 1,\n\"module\": \"Engine\",\n"
                      "\"types\": []\n}",
                      3, 12));
        CHECK(Rejects("{\n\"format\": 1,\n\"package\": \"Rocks\",\n\"module\": 1,\n"
                      "\"types\": []\n}",
                      4, 11));
        CHECK(Rejects(TypeSchema(allocator, "\"name\": \"T\",\n"
                                            "\"id\": \"5a7e0000000000000000000000000001\",\n"
                                            "\"kind\": 1")
                          .View(),
                      9, 9));
        CHECK(
            Rejects(MemberSchema(allocator, "fields",
                                 "\"name\": \"X\",\n\"id\": \"5a7e0000000000000000000000000002\",\n"
                                 "\"type\": \"int\",\n\"access\": 1")
                        .View(),
                    15, 11));
        CHECK(
            Rejects(MemberSchema(allocator, "events",
                                 "\"name\": \"E\",\n\"id\": \"5a7e0000000000000000000000000003\",\n"
                                 "\"publisher\": 1")
                        .View(),
                    14, 14));
        CHECK(Rejects(ObjectType(allocator, "enum", "\"cases\": [\n1\n]").View(), 11, 1));
        CHECK(Rejects(FunctionPartSchema(allocator, "parameters", "\"type\": \"int\"").View(), 15,
                      1));
        CHECK(Rejects(
            MemberSchema(allocator, "functions", "\"id\": \"5a7e0000000000000000000000000003\"")
                .View(),
            11, 1));
        CHECK(Rejects(
            MemberSchema(allocator, "events", "\"id\": \"5a7e0000000000000000000000000003\"")
                .View(),
            11, 1));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// `overridable`(LC2b, SD65): object·asset 타입의 static이 아닌 함수만. 타입 이름은 언어 소유 이름일
// 수 없다(3.9절 "이름의 출처").
TEST(SparksBindingSchema_OverridableAndLanguageNames) {
    SystemAllocator allocator;
    {
        constexpr StringView Id = "\"id\": \"5a7e0000000000000000000000000003\"";
        auto function = [&](StringView kind, StringView extra) noexcept {
            const String lines = Join(allocator, "\"name\": \"F\",\n", Id, ",\n", extra);
            StringBuilder<512> type(allocator);
            type.Append("\"name\": \"T\",\n\"id\": \"").Append(TypeId).Append("\",\n\"kind\": \"");
            type.Append(kind)
                .Append("\",\n\"functions\": [\n{\n")
                .Append(lines.View())
                .Append("\n}\n]");
            return TypeSchema(allocator, type.View());
        };
        {
            Read read(allocator);
            REQUIRE(ReadText(read, function("object", "\"overridable\": true").View()));
            CHECK(read.valid);
            REQUIRE(read.schema.Functions().Size() == 1);
            CHECK(read.schema.Functions()[0].overridable);
        }
        CHECK(Accepts(function("asset", "\"overridable\": false").View()));
        // 줄: 7 name, 8 id, 9 kind, 10 functions, 11 '{', 12 name, 13 id, 14 key.
        CHECK(Rejects(function("struct", "\"overridable\": true").View(), 14, 16));
        CHECK(FirstSays(function("struct", "\"overridable\": true").View(),
                        "'overridable' is only for functions of object and asset types."));
        CHECK(Rejects(function("interface", "\"overridable\": true").View(), 14, 16));
        CHECK(
            Rejects(function("object", "\"static\": true,\n\"overridable\": true").View(), 15, 16));
        CHECK(FirstSays(function("object", "\"static\": true,\n\"overridable\": true").View(),
                        "A static function cannot be overridable."));
        CHECK(Rejects(function("object", "\"overridable\": 1").View(), 14, 16));
        // kind를 모르면 overridable을 거부하지 않는다(kind 진단 하나).
        CHECK(Rejects(function("thing", "\"overridable\": true").View(), 9, 9));

        constexpr StringView LanguageOwned[] = {"Array",  "Text",          "Log",
                                                "Action", "WorldPosition", "Clock"};
        for (const StringView name : LanguageOwned) {
            const String lines = Join(allocator, "\"name\": \"", name, "\",\n\"id\": \"", TypeId);
            const String text = TypeSchema(
                allocator, Join(allocator, lines.View(), "\",\n\"kind\": \"object\"").View());
            CHECK(Rejects(text.View(), 7, 9));
            CHECK(FirstSays(
                text.View(),
                Join(allocator, "Type '", name, "' uses a name that the language owns.").View()));
        }

        // 엔진이 제공하는 예약 이름은 schema가 정의한다.
        CHECK(Accepts(ObjectType(allocator, "object", "").View()));
        const String world =
            TypeSchema(allocator, Join(allocator, "\"name\": \"World\",\n\"id\": \"", TypeId,
                                       "\",\n\"kind\": \"object\"")
                                      .View());
        CHECK(Accepts(world.View()));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SparksBindingSchema_ReportsEveryProblem) {
    SystemAllocator allocator;
    {
        const At all[] = {{2, 11}, {3, 12}, {4, 11}};
        CHECK(Rejects(
            "{\n\"format\": 3,\n\"package\": \"X\",\n\"module\": \"y\",\n\"types\": []\n}", all));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SparksBindingSchema_RejectsUnusableSources) {
    SystemAllocator allocator;
    {
        SourceSet sources(allocator);
        DiagnosticList diagnostics(allocator);
        auto id = sources.TryAdd("Schema/X.sparkbind.json", "\xFF", diagnostics);
        REQUIRE(id);
        BindingSchema schema(allocator);
        auto read = ReadBindingSchema(sources, id.Value(), diagnostics, schema);
        CHECK(!read && read.Error() == ErrorCode::InvalidArgument);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SparksBindingSchema_AllocationFailureEverywhere) {
    SystemAllocator backing;
    constexpr StringView Text = R"({ "format": 1, "package": "Rocks", "module": "Engine", "types": [
  { "name": "A", "id": "5a7e0000000000000000000000000001", "kind": "object",
    "fields": [ { "name": "X", "id": "5a7e0000000000000000000000000002", "type": "int" } ],
    "functions": [ { "name": "F", "id": "5a7e0000000000000000000000000003",
      "typeParameters": [ { "name": "T" } ],
      "parameters": [ { "name": "a", "type": "T" } ], "returns": "T?" } ],
    "events": [ { "name": "E", "id": "5a7e0000000000000000000000000004" } ] },
  { "name": "B", "id": "5a7e0000000000000000000000000001", "kind": "enum", "cases": ["A"] } ] })";
    bool succeeded = false;
    uint64 failures = 0;
    for (uint64 failAt = 1; !succeeded && failAt < 10000; ++failAt) {
        test::NFailAllocator allocator(backing);
        {
            SourceSet sources(allocator);
            DiagnosticList diagnostics(allocator);
            allocator.DisableFailure();
            auto id = sources.TryAdd("Schema/A.sparkbind.json", Text, diagnostics);
            REQUIRE(id);
            BindingSchema schema(allocator);
            allocator.FailOnAttempt(failAt);
            auto read = ReadBindingSchema(sources, id.Value(), diagnostics, schema);
            if (read) {
                succeeded = true;
                CHECK(!read.Value());
                CHECK(diagnostics.Count() == 1); // ID 중복 하나
            } else {
                ++failures;
                CHECK(read.Error() == ErrorCode::OutOfMemory);
            }
        }
        CHECK(backing.Stats().liveAllocations == 0);
    }

    CHECK(succeeded);
    CHECK(failures >= 5);
}

} // namespace sparks_binding_schema_tests_file
