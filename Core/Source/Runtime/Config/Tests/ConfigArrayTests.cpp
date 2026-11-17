#include "ConfigTestSupport.h"

// 설정 CF1 배열 연산·경로 이름 시험이다(Docs/Design/ConfigSystem.ko.md 4.2·4.3절): 연산마다의 뜻과
// 적용 순서($clear → $remove → $add·$append, 키 순서와 무관), 이전 층까지의 값 위 적용과 Unset 뒤
// 재적용, Set 연산, 원소 비교가 리플렉션 값 동등성(enum 대소문자·실수 표기·struct 멤버
// 순서·기본값)인지, 잘못된 연산·`$` 위치·컨테이너 안 연산의 줄·열 진단, 경로 이름 펼치기와 오류.
using namespace rocks;
using namespace rocks::config;
using namespace rocks::test::config;

// 연산 객체는 이전 층까지의 값에 $remove → $add → $append 순서로 적용된다(객체 안 키 순서와 무관).
// $clear는 그보다 먼저다. 연산 층을 빼면 나머지 층으로 다시 합친다.
TEST(Config_ArrayOperationsApplyInOrder) {
    ConfigFixture fixture;
    ConfigContext& context = fixture.Context();
    REQUIRE(context.AddLayerText(ConfigLayer::Engine, "Render.json", "Render", "engine",
                                 R"({"RenderGraph": {"passes": ["a", "b", "c"]}})"));
    REQUIRE(context.AddLayerText(
        ConfigLayer::Project, "Render.json", "Render", "project",
        R"({"RenderGraph": {"passes": {"$append": ["a"], "$add": ["c", "d"], "$remove": ["b"]}}})"));
    REQUIRE(context.Sync());
    CHECK(JoinedEquals(fixture.Graph().passes, "a,c,d,a"));

    // $append는 중복을 허용하고 $remove는 같은 값의 원소를 모두 뺀다.
    REQUIRE(context.AddLayerText(
        ConfigLayer::Feature, "extra", "Render", "feature",
        R"({"RenderGraph": {"passes": {"$remove": ["a"], "$append": ["e", "e"]}}})"));
    REQUIRE(context.Sync());
    CHECK(JoinedEquals(fixture.Graph().passes, "c,d,e,e"));
    const ConfigLayer stack[] = {ConfigLayer::Engine, ConfigLayer::Project, ConfigLayer::Feature};
    CHECK(fixture.SourcesAre("Render", "RenderGraph", "passes", stack));

    // $clear가 먼저이므로 같은 객체의 $add만 남는다.
    REQUIRE(
        context.AddLayerText(ConfigLayer::Feature, "reset", "Render", "reset",
                             R"({"RenderGraph": {"passes": {"$add": ["x"], "$clear": true}}})"));
    REQUIRE(context.Sync());
    CHECK(JoinedEquals(fixture.Graph().passes, "x"));

    // 연산 층을 빼면 남은 층으로 다시 합친다(약한 층을 나중에 넣어도 순서는 층 순서다).
    REQUIRE(context.Unset(ConfigLayer::Feature, "reset"));
    REQUIRE(context.Unset(ConfigLayer::Feature, "extra"));
    REQUIRE(context.Sync());
    CHECK(JoinedEquals(fixture.Graph().passes, "a,c,d,a"));
    REQUIRE(context.Unset(ConfigLayer::Engine, "Render.json"));
    REQUIRE(context.Sync());
    CHECK(JoinedEquals(fixture.Graph().passes, "c,d,a"));
}

// Set 필드도 같은 연산을 받는다($add는 없는 원소만, $append는 Set에 쓸 수 없다).
TEST(Config_SetOperations) {
    ConfigFixture fixture;
    ConfigContext& context = fixture.Context();
    REQUIRE(context.AddLayerText(ConfigLayer::Engine, "Render.json", "Render", "engine",
                                 R"({"RenderGraph": {"tags": ["x", "y"]}})"));
    REQUIRE(context.AddLayerText(
        ConfigLayer::Project, "Render.json", "Render", "project",
        R"({"RenderGraph": {"tags": {"$add": ["y", "z"], "$remove": ["x"]}}})"));
    REQUIRE(context.Sync());
    CHECK(JoinedEquals(fixture.Graph().tags, "y,z"));
    CHECK(fixture.Rejects(ConfigLayer::Project, "Render",
                          "{\"RenderGraph\": {\"tags\": {\"$append\": [\"t\"]}}}"));
    CHECK(fixture.HasDiagnostic(ConfigDiagnosticCode::InvalidArrayOperation, 1, 38));
}

// 원소 비교는 리플렉션 값 동등성이다: enum 도구 이름은 대소문자를 무시하고, 실수 1과 1.0은 같고,
// struct 원소는 멤버 순서와 무관하며 빠진 멤버는 기본값이다.
TEST(Config_ArrayElementsCompareAsReflectedValues) {
    ConfigFixture fixture;
    ConfigContext& context = fixture.Context();
    REQUIRE(
        context.AddLayerText(ConfigLayer::Engine, "Render.json", "Render", "engine",
                             R"({"RenderGraph": {"qualities": ["Low", "High"], "scales": [1.0, 2.5],
            "points": [{"x": 1, "y": 2}, {"x": 3, "y": 4}]}})"));
    REQUIRE(context.AddLayerText(ConfigLayer::Project, "Render.json", "Render", "project",
                                 R"({"RenderGraph": {
            "qualities": {"$remove": ["high"], "$add": ["low", "medium"]},
            "scales": {"$remove": [1]},
            "points": {"$remove": [{"y": 2, "x": 1}], "$add": [{"x": 3, "y": 4}, {"x": 5}]}}})"));
    REQUIRE(context.Sync());
    const GraphSettings& graph = fixture.Graph();
    REQUIRE(graph.qualities.Size() == 2);
    CHECK(graph.qualities[0] == FixtureQuality::Low &&
          graph.qualities[1] == FixtureQuality::Medium);
    REQUIRE(graph.scales.Size() == 1);
    CHECK(graph.scales[0] == 2.5f);
    REQUIRE(graph.points.Size() == 2);
    CHECK(graph.points[0].x == 3 && graph.points[0].y == 4);
    CHECK(graph.points[1].x == 5 && graph.points[1].y == 0);
}

// 잘못된 연산은 그 값의 원문 줄·열로 거부한다: 모르는 연산, $clear의 값, 배열이 아닌 목록, 빈 연산
// 객체, struct 자리의 `$` 키, Map 키의 `$`, 정적 배열이 아닌 단일 값 필드의 연산 객체(리플렉션 형식
// 진단), 배열 원소 안의 연산(지원하지 않음), 연산 목록 원소의 모르는 키(그 원소 안 위치로
// 되돌린다).
TEST(Config_InvalidArrayOperationsReportPositions) {
    ConfigFixture fixture;
    CHECK(fixture.Rejects(ConfigLayer::Engine, "Render",
                          "{\"RenderGraph\": {\n  \"passes\": {\"$insert\": [\"b\"]}}}"));
    CHECK(fixture.HasDiagnostic(ConfigDiagnosticCode::InvalidArrayOperation, 2, 25));
    CHECK(fixture.Rejects(ConfigLayer::Engine, "Render",
                          "{\"RenderGraph\": {\"passes\": {\"$clear\": false}}}"));
    CHECK(fixture.HasDiagnostic(ConfigDiagnosticCode::InvalidArrayOperation, 1, 39));
    CHECK(fixture.Rejects(ConfigLayer::Engine, "Render",
                          "{\"RenderGraph\": {\"passes\": {\"$add\": \"b\"}}}"));
    CHECK(fixture.HasDiagnostic(ConfigDiagnosticCode::InvalidArrayOperation, 1, 37));
    CHECK(fixture.Rejects(ConfigLayer::Engine, "Render", "{\"RenderGraph\": {\"passes\": {}}}"));
    CHECK(fixture.HasDiagnostic(ConfigDiagnosticCode::InvalidArrayOperation, 1, 28));
    CHECK(fixture.Rejects(ConfigLayer::Engine, "Render",
                          "{\"RenderGraph\": {\n  \"$add\": [\"a\"]}}"));
    CHECK(fixture.HasDiagnostic(ConfigDiagnosticCode::MisplacedOperation, 2, 11));
    CHECK(fixture.Rejects(ConfigLayer::Engine, "Render",
                          "{\"RenderGraph\": {\"origin\": {\"$clear\": true}}}"));
    CHECK(fixture.HasDiagnostic(ConfigDiagnosticCode::MisplacedOperation, 1, 39));
    CHECK(fixture.Rejects(ConfigLayer::Engine, "Render",
                          "{\"RenderGraph\": {\"byName\": {\"$a\": 1}}}"));
    CHECK(fixture.HasDiagnostic(ConfigDiagnosticCode::MisplacedOperation, 1, 35));
    CHECK(fixture.Rejects(ConfigLayer::Engine, "Render",
                          "{\"RenderGraph\": {\"maxPasses\": {\"$clear\": true}}}"));
    CHECK(fixture.HasDiagnostic(ConfigDiagnosticCode::FieldValue, 1, 31,
                                reflection_text::TextDiagnosticCode::InvalidValue));
    CHECK(fixture.Rejects(ConfigLayer::Engine, "Render",
                          "{\"RenderGraph\": {\"layers\": [{\"names\": {\"$add\": [\"n\"]}}]}}"));
    CHECK(fixture.HasDiagnostic(ConfigDiagnosticCode::UnsupportedOperation, 1, 39));
    CHECK(fixture.Rejects(ConfigLayer::Engine, "Render",
                          "{\"RenderGraph\": {\n  \"points\": {\"$add\": [{\"x\": 1},\n"
                          "    {\"z\": 2}]}}}"));
    CHECK(fixture.HasDiagnostic(ConfigDiagnosticCode::FieldValue, 3, 11,
                                reflection_text::TextDiagnosticCode::UnknownProperty));
    // 연산 목록 원소의 값 오류(enum에 없는 이름)도 그 원소 위치다.
    CHECK(fixture.Rejects(ConfigLayer::Engine, "Render",
                          "{\"RenderGraph\": {\"qualities\": {\"$add\": [\"Ultra\"]}}}"));
    CHECK(fixture.HasDiagnostic(ConfigDiagnosticCode::FieldValue, 1, 41,
                                reflection_text::TextDiagnosticCode::InvalidValue));
}

// 문자열 필드 값(원소·Map 값·연산 목록·대입 값 포함)의
// `{EngineDir}`·`{ProjectDir}`·`{UserDataDir}`·
// `{SavedDir}`를 펼친다. 이름 문법이 아닌 `{`는 그대로이고, 표에 없거나 값이 빈 이름은 오류다.
TEST(Config_PathTokensExpandInStrings) {
    ConfigContextOptions options;
    options.projectDir = "C:/Game";
    options.savedDir = "C:/Game/Saved";
    ConfigFixture fixture(options);
    ConfigContext& context = fixture.Context();
    REQUIRE(context.AddLayerText(
        ConfigLayer::Engine, "Render.json", "Render", "engine",
        R"({"RenderGraph": {"cacheDir": "{SavedDir}/Cache", "passes": ["{ProjectDir}/a", "{x", "{}"],
            "tags": {"$add": ["{SavedDir}"]}}})"));
    REQUIRE(context.Sync());
    CHECK(fixture.Graph().cacheDir.View() == "C:/Game/Saved/Cache");
    CHECK(JoinedEquals(fixture.Graph().passes, "C:/Game/a,{x,{}"));
    CHECK(JoinedEquals(fixture.Graph().tags, "C:/Game/Saved"));
    REQUIRE(context.AddAssignment(ConfigLayer::Session, "cli", "Render.RenderGraph.cacheDir",
                                  "{ProjectDir}/B"));
    REQUIRE(context.Sync());
    CHECK(fixture.Graph().cacheDir.View() == "C:/Game/B");

    CHECK(fixture.Rejects(ConfigLayer::Project, "Render",
                          "{\"RenderGraph\": {\"cacheDir\": \"{Nope}/x\"}}"));
    CHECK(fixture.HasDiagnostic(ConfigDiagnosticCode::InvalidPathToken, 1, 30));
    // 값이 없는 이름(EngineDir·UserDataDir을 주지 않았다)도 오류다.
    CHECK(fixture.Rejects(ConfigLayer::Project, "Render",
                          "{\"RenderGraph\": {\"passes\": [\"{EngineDir}\"]}}"));
    CHECK(fixture.HasDiagnostic(ConfigDiagnosticCode::InvalidPathToken, 1, 29));
    fixture.Context().Diagnostics().Clear();
    auto assigned = context.AddAssignment(ConfigLayer::Session, "cli",
                                          "Render.RenderGraph.cacheDir", "{UserDataDir}");
    CHECK(!assigned && fixture.HasDiagnostic(ConfigDiagnosticCode::InvalidPathToken));
}
