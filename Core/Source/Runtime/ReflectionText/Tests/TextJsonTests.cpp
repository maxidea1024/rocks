#include "TextAssignTestSupport.h"

#include <ReflectionTestFixtures/ReflectionTestFixtures.module.generated.h>
#include <ReflectionTestFixtures/TextFixture.generated.h>
#include <Core/Text/NamePool.h>
#include <ReflectionText/TextJson.h>

// 리플렉션 B9i4 JSON export·import 시험이다(Docs/Design/ReflectionExtensions.ko.md 10.4·10.5절,
// fixture ReflectionTestFixtures/TextFixture.h·AssignFixture.h): 모든 종류의 왕복(Map·Set 순서,
// float 단정도 표기, staged 대상), import 의미(struct 병합·컨테이너 교체·mapMerge), 엄격성과 JSON
// 위치, 용도별 제외, 한도, struct 값 대상, PathReadKeys.
using namespace rocks;
namespace text_json_tests_file {

using namespace rocks::reflection_text;
using namespace rocks::test::assign;
using reflection_fixture::AssignHolder;
using reflection_fixture::AssignPoint;
using reflection_fixture::TextHolder;

namespace {

// TextHolder 객체 Registry(Name pool 포함)다(소멸: Registry → 등록 → pool → 타입).
struct TextObjects {
    TextObjects() noexcept
        : types(system), registration(types, system), names(system),
          objects(system, types, {0x4a53, 1}, MakeOptions(names)), access(objects.OwnerAccess()),
          diagnostics(system) {
        REQUIRE(generated::ReflectionTestFixtures::RegisterModuleTypes(registration));
        REQUIRE(names.Initialize());
    }

    ~TextObjects() noexcept {
        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
        CHECK(registration.Unregister());
        CHECK(names.Shutdown());
        CHECK(types.Shutdown());
    }

    static RegistryOptions MakeOptions(NamePool& pool) noexcept {
        RegistryOptions options;
        options.names = &pool;
        return options;
    }

    RootRef<TextHolder> Make() noexcept {
        auto created =
            objects.NewObject<TextHolder>(access, GeneratedType<TextHolder>::Schema().id);
        CHECK(created);
        return created ? Move(created.Value()) : RootRef<TextHolder>{};
    }

    SystemAllocator system;
    TypeRegistry types;
    ModuleRegistration registration;
    NamePool names;
    ObjectRegistry objects;
    ObjectAccessContext access;
    TextDiagnostics diagnostics;
    TextCodecOptions options;
};

// target을 JSON 텍스트로 out에 쓴다.
Result<void> Export(const AssignmentTarget& target, const TextCodecOptions& options,
                    StringBuilderBase& out) noexcept {
    out.Clear();
    JsonWriter writer(out);
    if (auto written = ExportJson(target, writer, options); !written) {
        return written;
    }

    return writer.Finish();
}

// text를 읽어 target에 import한다(문서는 호출 동안만 산다).
Result<PreparedAssignments> Import(Allocator& allocator, StringView text,
                                   const AssignmentTarget& target, const TextCodecOptions& options,
                                   TextDiagnostics& diagnostics) noexcept {
    JsonDocument document(allocator);
    const bool parsed = document.Parse(text).Succeeded();
    CHECK(parsed);
    if (!parsed) {
        return Result<PreparedAssignments>::Failure(ErrorCode::InvalidArgument);
    }

    diagnostics.Clear();
    return ImportJson(document.Root(), target, options, diagnostics);
}

bool Publishes(Result<PreparedAssignments> prepared) noexcept {
    return prepared && prepared.Value().Publish();
}

} // namespace

// TextHolder의 모든 종류를 쓰고(float 단정도·Map 삽입 순서·Flags 배열·null 참조) 그 JSON을 staged
// 객체에 읽어 다시 쓰면 같은 텍스트다. 같은 객체에 다시 읽으면 ReadOnly 필드도 값이 같아
// 무동작이다(asset-info --json → asset-set 왕복).
TEST(TextJson_RoundTripsEveryKindInOrder) {
    TextObjects fixture;
    auto root = fixture.Make();
    const ObjectHandle object = root.Handle();
    const TextAssignment inputs[] = {
        Set("enabled", "true"),
        Set("small", "200", 1),
        Set("medium", "60000", 2),
        Set("count", "4000000000", 3),
        Set("big", "18446744073709551615", 4),
        Set("tiny", "-128", 5),
        Set("narrow", "-3", 6),
        Set("wide", "-9223372036854775808", 7),
        Set("ratio", "0.1", 8),
        Set("precise", "nan", 9),
        Set("id", "00112233-4455-6677-8899-aabbccddeeff", 10),
        Set("asset", "10112233-4455-6677-8899-aabbccddeeff", 11),
        Set("digest", "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff", 12),
        Set("label", "a,b", 13),
        Set("tag", "Hero", 14),
        Set("quality", "High", 15),
        Set("channels", "Red+Blue", 16),
        Set("source", "20112233-4455-6677-8899-aabbccddeeff", 17),
        Set("kind", "TextKindDerived", 18),
        Set("numbers", "3,1,2", 19),
        Set("words", "x,y", 20),
        Set("points[0]", "{}", 21),
        Set("points[0].x", "4", 22),
        Set("triple", "1,2,3", 23),
        Set("tags", "b,a", 24),
        Set("scores", "z:1,a:2", 25),
        Set("byQuality", "High:h,Low:l", 26),
        Set("byName[k]", "{}", 27),
        Set("byName[k].y", "6", 28),
        Set("byClass", "TextKindDerived:1", 29),
        Set("bySoft", "30112233-4455-6677-8899-aabbccddeeff:5", 30),
        Set("limit", "9", 31),
        Set("anchor", "{}", 32),
        Set("anchor.x", "7", 33),
        Set("origin.y", "-1", 34)};
    fixture.diagnostics.Clear();
    REQUIRE(Publishes(
        PrepareAssignments(AssignmentTarget::Live(fixture.objects, fixture.access, object), inputs,
                           fixture.options, fixture.diagnostics)));
    {
        auto borrowed = fixture.objects.Resolve<TextHolder>(fixture.access, object);
        REQUIRE(borrowed);
        borrowed.Value().Get().Value()->locked = 3;
        borrowed.Value().Get().Value()->cache = 5;
    }

    StringBuilder<> first(fixture.system);
    const AssignmentTarget live = AssignmentTarget::Live(fixture.objects, fixture.access, object);
    REQUIRE(Export(live, fixture.options, first));
    JsonDocument document(fixture.system);
    REQUIRE(document.Parse(first.View()).Succeeded());
    const JsonValue top = document.Root();
    // 키 순서는 등록 스키마의 필드 순서다(MetaCompiler가 낸 순서 — 선언 순서가 아닐 수 있다).
    CHECK(top.Find("ratio").RawNumber() == "0.1" && top.Find("enabled").GetBool());
    CHECK(top.Find("precise").GetString() == "nan");
    CHECK(top.Find("big").RawNumber() == "18446744073709551615");
    CHECK(top.Find("wide").RawNumber() == "-9223372036854775808");
    CHECK(top.Find("channels").Size() == 2 && top.Find("channels").At(1).GetString() == "Blue");
    CHECK(top.Find("next").IsNull() && top.Find("peer").IsNull());
    CHECK(top.Find("kind").GetString() == "TextKindDerived");
    CHECK(top.Find("scores").KeyAt(0) == "z" && top.Find("scores").KeyAt(1) == "a");
    CHECK(top.Find("tags").At(0).GetString() == "b" && top.Find("byName").Find("k").IsObject());
    CHECK(top.Find("triple").Size() == 3 && top.Find("limit").RawNumber() == "9");
    // Edit 출력은 Transient를 빼고 ReadOnly는 쓴다.
    CHECK(!top.Find("cache").IsValid() && top.Find("locked").RawNumber() == "3");

    // 같은 객체에 다시 읽으면 ReadOnly 필드도 값이 같아 받는다.
    REQUIRE(Publishes(
        Import(fixture.system, first.View(), live, fixture.options, fixture.diagnostics)));

    // staged 객체에 읽어 다시 쓰면 같은 텍스트다.
    ConstructionContext transaction(fixture.objects, fixture.access);
    auto created = transaction.Create<TextHolder>(GeneratedType<TextHolder>::Schema().id);
    REQUIRE(created);
    const AssignmentTarget staged = AssignmentTarget::Staged(transaction, created.Value());
    REQUIRE(Publishes(
        Import(fixture.system, first.View(), staged, fixture.options, fixture.diagnostics)));
    StringBuilder<> second(fixture.system);
    REQUIRE(Export(staged, fixture.options, second));
    CHECK(second.View() == first.View());
    transaction.Rollback();
}

// struct는 멤버 병합(없는 키는 지금 값), Array·StaticArray·Set·Optional·Map은 통째 교체(Optional
// null은 unset, struct 원소는 기본값 뒤 병합)다. MergeByKey는 JSON에 있는 키만 넣거나 바꾼다.
TEST(TextJson_ImportMergesStructsAndReplacesContainers) {
    SystemAllocator system;
    Fixture fixture(system);
    auto root = fixture.Make();
    const ObjectHandle object = root.Handle();
    const TextAssignment inputs[] = {
        Set("inner.value", "1"),   Set("inner.point.x", "2", 1), Set("numbers", "1,2,3", 2),
        Set("points[0]", "{}", 3), Set("points[1]", "{}", 4),    Set("byKey[a]", "{}", 5),
        Set("byKey[a].x", "1", 6), Set("byKey[b]", "{}", 7),     Set("scores", "a:1,b:2", 8),
        Set("tags", "x,y", 9),     Set("limit", "4", 10),        Set("anchor", "{}", 11),
        Set("anchor.x", "5", 12),  Set("pair[0].x", "1", 13)};
    REQUIRE(Publishes(fixture.Prepare(object, inputs)));
    const AssignmentTarget live = AssignmentTarget::Live(fixture.objects, fixture.Access(), object);
    const char* replace = R"({
  "inner": {"value": 9},
  "numbers": [7],
  "points": [{"x": 4}],
  "byKey": {"c": {"y": 3}},
  "scores": {"c": 3},
  "tags": ["z"],
  "limit": null,
  "anchor": {"y": 6},
  "pair": [{"x": 8}, {}]
})";
    auto prepared = Import(system, replace, live, fixture.options, fixture.diagnostics);
    REQUIRE(prepared);
    CHECK(HasOverride(fixture, prepared.Value().OverridePaths(), "inner.value"));
    CHECK(!Touches(fixture, prepared.Value(), "inner.point"));
    REQUIRE(prepared.Value().Publish());
    {
        Peek holder(fixture, object);
        CHECK(holder->inner.value == 9 && holder->inner.point.x == 2);
        CHECK(holder->numbers.Size() == 1 && holder->numbers[0] == 7);
        CHECK(holder->points.Size() == 1 && holder->points[0].x == 4 && holder->points[0].y == 0);
        CHECK(holder->byKey.Count() == 1 && holder->byKey.Find(StringView("c"))->y == 3);
        CHECK(holder->scores.Count() == 1 && *holder->scores.Find(StringView("c")) == 3);
        CHECK(holder->tags.Count() == 1 && holder->tags.Contains(StringView("z")));
        CHECK(!holder->limit.HasValue());
        CHECK(holder->anchor.HasValue() && holder->anchor.Value().x == 0 &&
              holder->anchor.Value().y == 6);
        CHECK(holder->pair[0].x == 8 && holder->pair[1].x == 0);
    }

    // MergeByKey: 있던 struct 항목은 멤버 병합, 없던 항목은 기본값 뒤 병합, 단일 값은 넣거나
    // 바꾼다.
    const TextAssignment more[] = {Set("byKey[a]", "{}"), Set("byKey[a].x", "1", 1),
                                   Set("scores[a]", "1", 2)};
    REQUIRE(Publishes(fixture.Prepare(object, more)));
    fixture.options.mapMerge = MapMergeMode::MergeByKey;
    const char* merge = R"({"byKey": {"a": {"y": 7}, "d": {"x": 1}}, "scores": {"a": 5, "d": 6}})";
    REQUIRE(Publishes(Import(system, merge, live, fixture.options, fixture.diagnostics)));
    Peek holder(fixture, object);
    CHECK(holder->byKey.Count() == 3 && holder->byKey.Find(StringView("a"))->x == 1);
    CHECK(holder->byKey.Find(StringView("a"))->y == 7 &&
          holder->byKey.Find(StringView("c"))->y == 3);
    CHECK(holder->byKey.Find(StringView("d"))->x == 1);
    CHECK(holder->scores.Count() == 3 && *holder->scores.Find(StringView("a")) == 5);
    CHECK(*holder->scores.Find(StringView("d")) == 6);
}

// 필드 플래그 TextMapMergeByKey(AssetSystem 10.3.1절): 옵션이 Replace여도 플래그 단 Map은 키별
// 병합(있던 항목은 멤버 병합 — 그 안의 플래그 없는 Map은 통째 교체, 없던 항목은 기본값 뒤 병합,
// 없는 키는 유지), 플래그 없는 Map은 통째 교체다. 옵션 MergeByKey면 둘 다 키별이다.
TEST(TextJson_FieldMapMergeByKey) {
    SystemAllocator system;
    Fixture fixture(system);
    auto shelfType = fixture.types.AcquireValueType(GeneratedType<AssignKeyedShelf>::Schema().id,
                                                    TypeKind::Struct);
    REQUIRE(shelfType);
    AssignKeyedShelf shelf(system);
    auto fill = [&](StringView key, StringView inner, int32 x) noexcept {
        AssignShelf value(system);
        String innerKey(system);
        String outerKey(system);
        CHECK(innerKey.TryAssign(inner) && outerKey.TryAssign(key));
        AssignPoint point;
        point.x = x;
        CHECK(value.byKey.InsertOrAssign(Move(innerKey), point));
        CHECK(shelf.keyed.InsertOrAssign(Move(outerKey), Move(value)));
    };
    fill("a", "p", 1);
    fill("b", "p", 2);
    String one(system);
    String two(system);
    REQUIRE(one.TryAssign("a") && two.TryAssign("b"));
    REQUIRE(shelf.plain.InsertOrAssign(Move(one), 1) && shelf.plain.InsertOrAssign(Move(two), 2));
    const AssignmentTarget target = AssignmentTarget::Value(fixture.objects, fixture.Access(),
                                                            shelfType.Value(), &shelf, system);
    fixture.options.mapMerge = MapMergeMode::Replace;
    const char* text =
        R"({"keyed": {"a": {"byKey": {"q": {"y": 5}}}, "c": {}}, "plain": {"c": 3}})";
    REQUIRE(Publishes(Import(system, text, target, fixture.options, fixture.diagnostics)));
    CHECK(shelf.keyed.Count() == 3 && shelf.keyed.Find(StringView("b")) != nullptr &&
          shelf.keyed.Find(StringView("c")) != nullptr);
    const AssignShelf* a = shelf.keyed.Find(StringView("a"));
    REQUIRE(a != nullptr);
    CHECK(a->byKey.Count() == 1 && a->byKey.Find(StringView("q")) != nullptr &&
          a->byKey.Find(StringView("q"))->y == 5);
    CHECK(shelf.plain.Count() == 1 && *shelf.plain.Find(StringView("c")) == 3);

    // 옵션 MergeByKey는 플래그 없는 Map도 키별이다.
    fixture.options.mapMerge = MapMergeMode::MergeByKey;
    REQUIRE(Publishes(
        Import(system, R"({"plain": {"d": 4}})", target, fixture.options, fixture.diagnostics)));
    CHECK(shelf.plain.Count() == 2 && *shelf.plain.Find(StringView("d")) == 4);
    fixture.options.mapMerge = MapMergeMode::Replace;
}

// 엄격성: 모르는 키·대소문자만 다른 키·JSON 종류 불일치·ReadOnly·Deprecated 클래스·배열 원소 형식을
// 모두 모아 JSON 위치(line·column)와 경로로 보고하고 아무것도 게시하지 않는다. Deprecated 클래스의
// 같은 값은 무동작이다.
TEST(TextJson_StrictDiagnosticsCarryJsonPositions) {
    SystemAllocator system;
    Fixture fixture(system);
    auto root = fixture.Make();
    const ObjectHandle object = root.Handle();
    const AssignmentTarget live = AssignmentTarget::Live(fixture.objects, fixture.Access(), object);
    const char* text = R"({
  "Count": 1,
  "COUNT": 2,
  "nope": 3,
  "label": 5,
  "locked": 7,
  "kind": "AssignKindOld",
  "numbers": [1, "x"]
})";
    auto prepared = Import(system, text, live, fixture.options, fixture.diagnostics);
    REQUIRE(!prepared && prepared.Error() == ErrorCode::InvalidArgument);
    const TextDiagnostics& diagnostics = fixture.diagnostics;
    REQUIRE(diagnostics.Count() == 6);
    // ReadOnly 자리는 import 끝에 대조하므로 그 진단은 맨 뒤다(리플렉션 B9i 리뷰 반영).
    const TextDiagnosticCode expected[] = {
        TextDiagnosticCode::DuplicateKey, TextDiagnosticCode::UnknownProperty,
        TextDiagnosticCode::InvalidValue, TextDiagnosticCode::InvalidValue,
        TextDiagnosticCode::InvalidValue, TextDiagnosticCode::ReadOnly};
    for (usize index = 0; index < diagnostics.Count(); ++index) {
        CHECK(diagnostics.Get(index).code == expected[index]);
        CHECK(diagnostics.Get(index).source.kind == TextSourceKind::Json);
    }

    CHECK(diagnostics.Get(1).path == "nope");
    CHECK(diagnostics.Get(1).source.position.line == 4 &&
          diagnostics.Get(1).source.position.column == 11);
    CHECK(diagnostics.Get(2).path == "label" && diagnostics.Get(2).source.position.line == 5);
    CHECK(diagnostics.Get(4).path == "numbers" && diagnostics.Get(4).source.position.column == 18);
    CHECK(diagnostics.Get(5).path == "locked" && diagnostics.Get(5).source.position.line == 6);
    CHECK(Peek(fixture, object)->count == 0);

    // Deprecated 클래스 값을 담은 객체: 같은 값 재대입은 거부하지 않고 무동작이다.
    {
        Peek holder(fixture, object);
        holder->kind = ClassRef<reflection_fixture::AssignKindBase>(
            GeneratedType<reflection_fixture::AssignKindOld>::Schema().id);
    }

    StringBuilder<> exported(system);
    REQUIRE(Export(live, fixture.options, exported));
    REQUIRE(Publishes(Import(system, exported.View(), live, fixture.options, fixture.diagnostics)));
}

// 용도별 제외: 출력은 용도 표대로 필드를 빼고, 입력 제외 필드는 ExcludedProperty, Report 용도의
// 입력은 InvalidArgument다. 숨김 열거자 입력은 HiddenEnumerator다.
TEST(TextJson_UseExclusionAndHiddenEnumerators) {
    TextObjects fixture;
    auto root = fixture.Make();
    const AssignmentTarget live =
        AssignmentTarget::Live(fixture.objects, fixture.access, root.Handle());
    struct Case {
        TextUse use;
        bool cache;
        bool instance;
        bool exportOnly;
        bool copied;
    };

    const Case cases[] = {{TextUse::Edit, false, false, true, true},
                          {TextUse::Copy, false, false, false, false},
                          {TextUse::TryoutCopy, false, true, false, false},
                          {TextUse::Report, false, true, true, true}};
    StringBuilder<> text(fixture.system);
    for (const Case& item : cases) {
        fixture.options.use = item.use;
        REQUIRE(Export(live, fixture.options, text));
        JsonDocument document(fixture.system);
        REQUIRE(document.Parse(text.View()).Succeeded());
        const JsonValue top = document.Root();
        CHECK(top.Find("cache").IsValid() == item.cache);
        CHECK(top.Find("instance").IsValid() == item.instance);
        CHECK(top.Find("exportOnly").IsValid() == item.exportOnly);
        CHECK(top.Find("copied").IsValid() == item.copied);
    }

    fixture.options.use = TextUse::Report;
    auto report = Import(fixture.system, "{}", live, fixture.options, fixture.diagnostics);
    CHECK(!report && report.Error() == ErrorCode::InvalidArgument &&
          fixture.diagnostics.Count() == 0);

    fixture.options.use = TextUse::Copy;
    auto copy = Import(fixture.system, R"({"copied": {}, "cache": 1, "exportOnly": 2})", live,
                       fixture.options, fixture.diagnostics);
    REQUIRE(!copy && fixture.diagnostics.Count() == 3);
    CHECK(fixture.diagnostics.Get(0).code == TextDiagnosticCode::ExcludedProperty);
    CHECK(fixture.diagnostics.Get(2).path == "exportOnly");

    fixture.options.use = TextUse::Edit;
    auto edit = Import(fixture.system,
                       R"({"exportOnly": 2, "quality": "Secret", "channels": ["Red", "Alpha"]})",
                       live, fixture.options, fixture.diagnostics);
    REQUIRE(!edit && fixture.diagnostics.Count() == 2);
    CHECK(fixture.diagnostics.Get(0).code == TextDiagnosticCode::HiddenEnumerator);
    CHECK(fixture.diagnostics.Get(1).code == TextDiagnosticCode::HiddenEnumerator);
    CHECK(Publishes(Import(fixture.system, R"({"exportOnly": 2, "channels": []})", live,
                           fixture.options, fixture.diagnostics)));
}

// 한도(JSON 깊이·입력 byte는 호출 오류, 원소 수는 진단), struct 값 대상의 export·import, staged
// 대상의 ReadOnly 허용, 참조(resolver)와 원소를 담은 StructRefArray, PathReadKeys의 순서·오류.
TEST(TextJson_LimitsTargetsAndReadKeys) {
    SystemAllocator system;
    Fixture fixture(system);
    auto root = fixture.Make();
    const ObjectHandle object = root.Handle();
    const AssignmentTarget live = AssignmentTarget::Live(fixture.objects, fixture.Access(), object);
    TextCodecOptions limited;
    limited.maxJsonDepth = 1;
    CHECK(ErrorOf(Import(system, R"({"inner": {"value": 1}})", live, limited,
                         fixture.diagnostics)) == ErrorCode::CapacityExceeded);
    limited = TextCodecOptions{};
    limited.maxInputBytes = 8;
    CHECK(ErrorOf(Import(system, R"({"label": "hello"})", live, limited, fixture.diagnostics)) ==
          ErrorCode::CapacityExceeded);
    limited = TextCodecOptions{};
    limited.maxElements = 2;
    auto many = Import(system, R"({"numbers": [1, 2, 3]})", live, limited, fixture.diagnostics);
    REQUIRE(!many && fixture.diagnostics.Count() == 1);
    CHECK(fixture.diagnostics.Get(0).code == TextDiagnosticCode::OutOfRange);

    // struct 값 대상.
    auto pointType =
        fixture.types.AcquireValueType(GeneratedType<AssignPoint>::Schema().id, TypeKind::Struct);
    REQUIRE(pointType);
    AssignPoint point{1, 2};
    const AssignmentTarget value = AssignmentTarget::Value(fixture.objects, fixture.Access(),
                                                           pointType.Value(), &point, system);
    StringBuilder<> text(system);
    REQUIRE(Export(value, fixture.options, text));
    CHECK(text.View() == "{\n  \"x\": 1,\n  \"y\": 2\n}\n");
    REQUIRE(Publishes(Import(system, R"({"y": 5})", value, fixture.options, fixture.diagnostics)));
    CHECK(point.x == 1 && point.y == 5);

    // staged 대상은 ReadOnly도 쓴다.
    {
        ConstructionContext transaction(fixture.objects, fixture.Access());
        auto created = transaction.Create<AssignHolder>(GeneratedType<AssignHolder>::Schema().id);
        REQUIRE(created);
        REQUIRE(Publishes(Import(system, R"({"locked": 4, "sealed": {"x": 2}})",
                                 AssignmentTarget::Staged(transaction, created.Value()),
                                 fixture.options, fixture.diagnostics)));
        auto staged = transaction.Get<AssignHolder>(created.Value());
        REQUIRE(staged);
        CHECK(staged.Value()->locked == 4 && staged.Value()->sealed.x == 2);
        transaction.Rollback();
    }

    // 참조는 resolver로 읽는다. 원소를 담은 StructRefArray는 비운 뒤 원소마다 더하고 멤버를
    // 병합한다(리플렉션 B9i5 — 빈 배열은 비우기).
    auto target = fixture.MakeTarget();
    TargetResolver resolver;
    resolver.target = target.Handle();
    fixture.options.resolver = &resolver;
    REQUIRE(Publishes(Import(system, R"({"next": "/t", "peer": "/t", "items": []})", live,
                             fixture.options, fixture.diagnostics)));
    CHECK(Peek(fixture, object)->next.Handle() == target.Handle());
    REQUIRE(Publishes(Import(system, R"({"items": [{}, {"weight": 3, "link": "/t"}]})", live,
                             fixture.options, fixture.diagnostics)));
    {
        Peek holder(fixture, object);
        REQUIRE(holder->items.Size() == 2);
        CHECK(holder->items[0].weight == 0 && holder->items[0].link.Handle().IsNull());
        CHECK(holder->items[1].weight == 3 && holder->items[1].link.Handle() == target.Handle());
    }

    REQUIRE(Publishes(Import(system, R"({"items": [{"weight": 4}]})", live, fixture.options,
                             fixture.diagnostics)));
    {
        Peek holder(fixture, object);
        REQUIRE(holder->items.Size() == 1);
        CHECK(holder->items[0].weight == 4 && holder->items[0].link.Handle().IsNull());
    }

    // resolver가 쓰지 못하는 non-null 참조는 export 오류다.
    CHECK(ErrorOf(Export(live, fixture.options, text)) == ErrorCode::Unsupported);
    fixture.options.resolver = nullptr;

    // PathReadKeys: 삽입 순서, key 단계·Map이 아닌 필드는 오류(out 그대로).
    const TextAssignment scores[] = {Set("scores", "z:1,a:2")};
    REQUIRE(Publishes(fixture.Prepare(object, scores)));
    PropertyPathBuffer path;
    REQUIRE(ParsePropertyPath(fixture.holder, "scores", path));
    Array<FieldValue> keys(system);
    REQUIRE(PathReadKeys(fixture.objects, fixture.Access(), object, path.View(), keys));
    REQUIRE(keys.Size() == 2);
    CHECK(keys[0].string.View() == "z" && keys[1].string.View() == "a");
    REQUIRE(ParsePropertyPath(fixture.holder, "scores[z]", path));
    PropertyPathFailure failure;
    CHECK(ErrorOf(PathReadKeys(fixture.objects, fixture.Access(), object, path.View(), keys,
                               &failure)) == ErrorCode::InvalidArgument);
    CHECK(failure.reason == PropertyPathError::IndexNotAllowed);
    REQUIRE(ParsePropertyPath(fixture.holder, "count", path));
    CHECK(ErrorOf(PathReadKeys(fixture.objects, fixture.Access(), object, path.View(), keys,
                               &failure)) == ErrorCode::TypeMismatch);
    CHECK(failure.reason == PropertyPathError::NotKeyed && keys.Size() == 2);
}
} // namespace text_json_tests_file
