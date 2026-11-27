#include "ConfigTestSupport.h"

// 설정 CF1 층·출처 시험이다(Docs/Design/ConfigSystem.ko.md 3·4·8절): 층 순서(넣은 순서와 무관),
// 약한 쓰기 기록, Unset 복귀, sync 지점 밖 값 불변, struct·Map 병합과 배열 교체, 모르는 키·섹션의
// 줄·열 진단, 줄 주석, 사용자 층·출시 규칙의 층 허용, 대입 대상.
using namespace rocks;
namespace config_layer_tests_file {

using namespace rocks::config;
using namespace rocks::test::config;

// 강한 층이 이기고 출처 스택은 약한 것부터 쌓인다. Unset은 그 출처를 빼고 남은 최강 값으로
// 돌아간다. 같은 층 안에서는 나중에 넣은 것이 이긴다.
TEST(Config_LayerOrderSourcesAndUnset) {
    ConfigFixture fixture;
    ConfigContext& context = fixture.Context();
    REQUIRE(context.AddLayerText(ConfigLayer::Engine, "Render.json", "Render", "engine/Render.json",
                                 R"({"RenderGraph": {"maxPasses": 10, "workers": 4}})"));
    REQUIRE(context.AddLayerText(ConfigLayer::Project, "Render.json", "Render",
                                 "project/Render.json", R"({"RenderGraph": {"maxPasses": 20}})"));
    REQUIRE(context.AddAssignment(ConfigLayer::Console, "console", "Render.RenderGraph.maxPasses",
                                  "30"));
    REQUIRE(context.Sync());
    CHECK(fixture.Graph().maxPasses == 30 && fixture.Graph().workers == 4);
    const ConfigLayer all[] = {ConfigLayer::Engine, ConfigLayer::Project, ConfigLayer::Console};
    CHECK(fixture.SourcesAre("Render", "RenderGraph", "maxPasses", all));
    const ConfigLayer engineOnly[] = {ConfigLayer::Engine};
    CHECK(fixture.SourcesAre("Render", "RenderGraph", "workers", engineOnly));

    REQUIRE(context.Unset(ConfigLayer::Console, "console"));
    REQUIRE(context.Sync());
    CHECK(fixture.Graph().maxPasses == 20);
    const ConfigLayer files[] = {ConfigLayer::Engine, ConfigLayer::Project};
    CHECK(fixture.SourcesAre("Render", "RenderGraph", "maxPasses", files));

    // 같은 층(Project)의 두 번째 입력이 이긴다. 그 태그를 빼면 첫 입력이 드러난다.
    REQUIRE(context.AddLayerText(ConfigLayer::Project, "late", "Render", "late",
                                 R"({"RenderGraph": {"maxPasses": 21}})"));
    REQUIRE(context.Sync());
    CHECK(fixture.Graph().maxPasses == 21);
    REQUIRE(context.Unset(ConfigLayer::Project, "late"));
    REQUIRE(context.Unset(ConfigLayer::Project, "Render.json"));
    REQUIRE(context.Sync());
    CHECK(fixture.Graph().maxPasses == 10);
    // 없는 태그의 Unset은 무동작이다.
    REQUIRE(context.Unset(ConfigLayer::Feature, "nothing"));
    REQUIRE(context.Sync());
    CHECK(fixture.Graph().maxPasses == 10);
}

// 현재 출처보다 약한 층의 쓰기는 값을 바꾸지 않지만 출처 스택에 남고 Note WeakWrite를 남긴다. 강한
// 출처를 빼면 그 값이 드러난다(3.1절 규칙). Unset 뒤 같은 Sync에 넣은 입력은 빠지지 않는다(큐
// 순서).
TEST(Config_WeakWriteIsRecordedAndRevealedByUnset) {
    ConfigFixture fixture;
    ConfigContext& context = fixture.Context();
    REQUIRE(context.AddAssignment(ConfigLayer::Console, "console", "Render.RenderGraph.maxPasses",
                                  "30"));
    REQUIRE(context.Sync());
    context.Diagnostics().Clear();
    REQUIRE(context.AddLayerText(ConfigLayer::Project, "Render.json", "Render", "project",
                                 R"({"RenderGraph": {"maxPasses": 25}})"));
    REQUIRE(context.Sync());
    CHECK(fixture.Graph().maxPasses == 30);
    const ConfigLayer stack[] = {ConfigLayer::Project, ConfigLayer::Console};
    CHECK(fixture.SourcesAre("Render", "RenderGraph", "maxPasses", stack));
    REQUIRE(context.Diagnostics().Count() == 1);
    const ConfigDiagnostic weak = context.Diagnostics().Get(0);
    CHECK(weak.code == ConfigDiagnosticCode::WeakWrite && weak.severity == ConfigSeverity::Note &&
          weak.layer == ConfigLayer::Project && weak.path == "maxPasses");
    CHECK(context.Diagnostics().ErrorCount() == 0);

    // Unset → 새 입력: 새 입력은 남는다. 새 입력 → Unset: 같이 빠진다.
    REQUIRE(context.Unset(ConfigLayer::Console, "console"));
    REQUIRE(context.AddAssignment(ConfigLayer::Console, "console", "Render.RenderGraph.maxPasses",
                                  "31"));
    REQUIRE(context.Sync());
    CHECK(fixture.Graph().maxPasses == 31);
    REQUIRE(context.AddAssignment(ConfigLayer::Console, "console", "Render.RenderGraph.maxPasses",
                                  "32"));
    REQUIRE(context.Unset(ConfigLayer::Console, "console"));
    REQUIRE(context.Sync());
    CHECK(fixture.Graph().maxPasses == 25);
}

// 값은 Sync에서만 바뀐다. 그 사이 Get이 돌려준 참조의 내용은 그대로이고 참조 자체도 Sync 뒤에
// 유효하다.
TEST(Config_ValuesChangeOnlyAtSync) {
    ConfigFixture fixture;
    ConfigContext& context = fixture.Context();
    const GraphSettings& graph = fixture.Graph();
    CHECK(!context.HasPendingChanges());
    REQUIRE(context.AddLayerText(ConfigLayer::Engine, "Render.json", "Render", "engine",
                                 R"({"RenderGraph": {"maxPasses": 5, "quality": "High"}})"));
    CHECK(context.HasPendingChanges());
    CHECK(graph.maxPasses == 64 && graph.quality == FixtureQuality::Medium);
    REQUIRE(context.Sync());
    CHECK(!context.HasPendingChanges());
    CHECK(&graph == &fixture.Graph());
    CHECK(graph.maxPasses == 5 && graph.quality == FixtureQuality::High);
    // 큐가 비면 Sync는 아무것도 바꾸지 않는다.
    REQUIRE(context.Sync());
    CHECK(graph.maxPasses == 5);
}

// struct는 재귀 병합(적힌 멤버만), Map은 키별 병합, 배열은 통째 교체다. 출처는 override
// 단위(멤버·컨테이너 필드) 경로마다 쌓인다.
TEST(Config_StructMapMergeAndArrayReplace) {
    ConfigFixture fixture;
    ConfigContext& context = fixture.Context();
    REQUIRE(context.AddLayerText(
        ConfigLayer::Engine, "Render.json", "Render", "engine",
        R"({"RenderGraph": {"origin": {"x": 1, "y": 2}, "byName": {"a": 1, "b": 2},
            "passes": ["a", "b"]}})"));
    REQUIRE(context.AddLayerText(
        ConfigLayer::Project, "Render.json", "Render", "project",
        R"({"RenderGraph": {"origin": {"y": 5}, "byName": {"b": 3, "c": 4}, "passes": ["c"]}})"));
    REQUIRE(context.Sync());
    const GraphSettings& graph = fixture.Graph();
    CHECK(graph.origin.x == 1 && graph.origin.y == 5);
    REQUIRE(graph.byName.Count() == 3);
    const int32* a = graph.byName.Find(StringView("a"));
    const int32* b = graph.byName.Find(StringView("b"));
    const int32* c = graph.byName.Find(StringView("c"));
    CHECK(a != nullptr && *a == 1 && b != nullptr && *b == 3 && c != nullptr && *c == 4);
    REQUIRE(graph.passes.Size() == 1);
    CHECK(graph.passes[0].View() == "c");
    const ConfigLayer both[] = {ConfigLayer::Engine, ConfigLayer::Project};
    const ConfigLayer engine[] = {ConfigLayer::Engine};
    CHECK(fixture.SourcesAre("Render", "RenderGraph", "origin.y", both));
    CHECK(fixture.SourcesAre("Render", "RenderGraph", "origin.x", engine));
    CHECK(fixture.SourcesAre("Render", "RenderGraph", "byName", both));
    CHECK(fixture.SourcesAre("Render", "RenderGraph", "passes", both));
}

// 엔진·프로젝트 파일의 모르는 키·섹션·형식 오류는 원문 줄·열 진단으로 거부한다(아무것도 큐에 넣지
// 않는다). 섹션 키는 대소문자를 무시하고, 대소문자만 다른 섹션 두 개는 오류다.
TEST(Config_StrictKeysReportJsonPositions) {
    ConfigFixture fixture;
    CHECK(fixture.Rejects(ConfigLayer::Engine, "Render",
                          "{\n  \"RenderGraph\": {\n    \"maxPases\": 3\n  }\n}"));
    CHECK(fixture.HasDiagnostic(ConfigDiagnosticCode::FieldValue, 3, 17,
                                reflection_text::TextDiagnosticCode::UnknownProperty));
    CHECK(fixture.Rejects(ConfigLayer::Engine, "Render", "{\n  \"RenderGrph\": {}\n}"));
    CHECK(fixture.HasDiagnostic(ConfigDiagnosticCode::UnknownSection, 2, 17));
    CHECK(fixture.Rejects(ConfigLayer::Engine, "Render",
                          "{\"RenderGraph\": {\"maxPasses\": \"many\"}}"));
    CHECK(fixture.HasDiagnostic(ConfigDiagnosticCode::FieldValue, 1, 31,
                                reflection_text::TextDiagnosticCode::InvalidValue));
    CHECK(fixture.Rejects(ConfigLayer::Engine, "Render", "{\"RenderGraph\": 3}"));
    CHECK(fixture.HasDiagnostic(ConfigDiagnosticCode::InvalidSection, 1, 17));
    CHECK(fixture.Rejects(ConfigLayer::Engine, "Render",
                          "{\"rendergraph\": {}, \"RenderGraph\": {}}"));
    CHECK(fixture.HasDiagnostic(ConfigDiagnosticCode::InvalidSection));
    CHECK(fixture.Rejects(ConfigLayer::Engine, "Render", "[1]"));
    CHECK(fixture.HasDiagnostic(ConfigDiagnosticCode::InvalidSection, 1, 1));
    // JSON 문법 오류는 줄 주석을 받는 읽기 규칙에서도 그 위치로 보고한다.
    CHECK(fixture.Rejects(ConfigLayer::Engine, "Render", "{ // c\n  \"RenderGraph\": {,}\n}"));
    CHECK(fixture.HasDiagnostic(ConfigDiagnosticCode::JsonSyntax, 2, 19));
    // 섹션은 등록한 branch 안에서만 찾는다(RenderGraph는 Render branch다).
    CHECK(fixture.Rejects(ConfigLayer::Engine, "Game", "{\"RenderGraph\": {}}"));
    CHECK(fixture.HasDiagnostic(ConfigDiagnosticCode::UnknownSection));
    // 모르는 branch는 NotFound다.
    fixture.Context().Diagnostics().Clear();
    auto unknown =
        fixture.Context().AddLayerText(ConfigLayer::Engine, "x", "Physics", "<test>", "{}");
    CHECK(!unknown && unknown.Error() == ErrorCode::NotFound);
    CHECK(fixture.HasDiagnostic(ConfigDiagnosticCode::UnknownBranch));
    // `Settings` 섹션(콘솔 설정 이름 → 값)은 이름 문법과 값 모양(bool·수·문자열)을 본다.
    CHECK(fixture.Rejects(ConfigLayer::Engine, "Render", "{\"Settings\": {\"Render.X\": 1}}"));
    CHECK(fixture.HasDiagnostic(ConfigDiagnosticCode::InvalidSetting, 1, 27));
    CHECK(fixture.Rejects(ConfigLayer::Engine, "Render", "{\"Settings\": {\"render.x\": [1]}}"));
    CHECK(fixture.HasDiagnostic(ConfigDiagnosticCode::InvalidSetting, 1, 27));
    // 섹션 키는 대소문자를 무시한다.
    REQUIRE(fixture.Context().AddLayerText(ConfigLayer::Engine, "x", "Render", "<test>",
                                           R"({"rendergraph": {"MAXPASSES": 9}})"));
    REQUIRE(fixture.Context().Sync());
    CHECK(fixture.Graph().maxPasses == 9);
}

// 층 파일은 줄 주석을 받는다(4.1절). 사용자 층은 UserConfig 필드만 쓸 수 있고, 출시 규칙에서는
// DevelopmentOnly 필드를 어떤 층도 쓸 수 없다(층 허용 진단은 그 멤버 값 위치).
TEST(Config_CommentsAndLayerPermissions) {
    ConfigFixture fixture(DevelopmentRules());
    ConfigContext& context = fixture.Context();
    REQUIRE(context.AddLayerText(ConfigLayer::Engine, "Render.json", "Render", "engine",
                                 "// 엔진 기본값\n{\n  \"RenderGraph\": { // 그래프\n"
                                 "    \"maxPasses\": 12, // 패스 수 // 두 번\n"
                                 "    \"cacheDir\": \"a//b\"\n  }\n}\n"));
    REQUIRE(context.AddLayerText(ConfigLayer::User, "Render.json", "Render", "user",
                                 R"({"RenderGraph": {"maxPasses": 13}})"));
    REQUIRE(context.Sync());
    CHECK(fixture.Graph().maxPasses == 13 && fixture.Graph().cacheDir.View() == "a//b");
    CHECK(fixture.Rejects(ConfigLayer::User, "Render",
                          "{\"RenderGraph\": {\n  \"maxPasses\": 1,\n  \"workers\": 9}}"));
    CHECK(fixture.HasDiagnostic(ConfigDiagnosticCode::LayerNotAllowed, 3, 14));
    // 개발 규칙에서는 DevelopmentOnly 필드도 쓸 수 있다.
    REQUIRE(context.AddLayerText(ConfigLayer::Project, "Render.json", "Render", "project",
                                 R"({"RenderGraph": {"debugDraw": true}})"));
    REQUIRE(context.Sync());
    CHECK(fixture.Graph().debugDraw);

    ConfigContextOptions release;
    release.releaseRules = true;
    ConfigFixture shipping(release);
    CHECK(shipping.Rejects(ConfigLayer::Project, "Render",
                           R"({"RenderGraph": {"debugDraw": true}})"));
    CHECK(shipping.HasDiagnostic(ConfigDiagnosticCode::LayerNotAllowed, 1, 31));
    shipping.Context().Diagnostics().Clear();
    auto assigned = shipping.Context().AddAssignment(ConfigLayer::Console, "console",
                                                     "Render.RenderGraph.debugDraw", "true");
    CHECK(!assigned && shipping.HasDiagnostic(ConfigDiagnosticCode::LayerNotAllowed));
    CHECK(!shipping.Graph().debugDraw);
}

// 대입 대상은 `<Branch>.<Section>.<필드 경로>`이고 값은 명령행 표기다. 등록 섹션이 아니면
// UnknownTarget, 모르는 필드는 리플렉션 진단이다(위치 없음).
TEST(Config_AssignmentTargets) {
    ConfigFixture fixture;
    ConfigContext& context = fixture.Context();
    REQUIRE(context.AddAssignment(ConfigLayer::Session, "cli", "Render.RenderGraph.origin.x", "9"));
    REQUIRE(context.AddAssignment(ConfigLayer::Session, "cli", "Game.Gameplay.lives", "4"));
    REQUIRE(context.AddAssignment(ConfigLayer::Session, "cli", "Render.RenderGraph.passes", "p,q"));
    REQUIRE(context.Sync());
    CHECK(fixture.Graph().origin.x == 9 && fixture.Gameplay().lives == 4);
    REQUIRE(fixture.Graph().passes.Size() == 2);
    CHECK(fixture.Graph().passes[1].View() == "q");
    const ConfigLayer session[] = {ConfigLayer::Session};
    CHECK(fixture.SourcesAre("Render", "RenderGraph", "origin.x", session));

    context.Diagnostics().Clear();
    auto section = context.AddAssignment(ConfigLayer::Session, "cli", "Render.Nope.x", "1");
    CHECK(!section && section.Error() == ErrorCode::InvalidArgument &&
          fixture.HasDiagnostic(ConfigDiagnosticCode::UnknownTarget));
    context.Diagnostics().Clear();
    auto shortTarget =
        context.AddAssignment(ConfigLayer::Session, "cli", "Render.RenderGraph", "1");
    CHECK(!shortTarget && fixture.HasDiagnostic(ConfigDiagnosticCode::UnknownTarget));
    context.Diagnostics().Clear();
    auto field = context.AddAssignment(ConfigLayer::Session, "cli", "Render.RenderGraph.nope", "1");
    CHECK(!field && fixture.HasDiagnostic(ConfigDiagnosticCode::FieldValue, 0, 0,
                                          reflection_text::TextDiagnosticCode::UnknownProperty));
    CHECK(!context.HasPendingChanges());
}

// 설정 층 적용(struct 값 대상 대입 묶음)도 struct 검증 hook을 게시 전에
// 부른다(Docs/Design/CurveData/README.ko.md 17.2절 — CV3 리뷰 1): low > high인 span은
// FieldValue(InvalidStructValue) 진단이고 값은 그대로다.
TEST(Config_StructValidationHookRejectsLayerValue) {
    ConfigFixture fixture;
    REQUIRE(fixture.Context().RegisterSettings<config_fixture::SpanSettings>());
    REQUIRE(fixture.Context().Sync());
    const auto& settings = fixture.Context().Get<config_fixture::SpanSettings>();
    CHECK(fixture.Rejects(ConfigLayer::Engine, "Render",
                          R"({"SpanCheck": {"span": {"low": 5, "high": 1}}})"));
    CHECK(fixture.HasDiagnostic(ConfigDiagnosticCode::FieldValue, 0, 0,
                                reflection_text::TextDiagnosticCode::InvalidStructValue));
    CHECK(settings.span.low == 0 && settings.span.high == 0);
    REQUIRE(fixture.Context().AddLayerText(ConfigLayer::Engine, "x", "Render", "<test>",
                                           R"({"SpanCheck": {"span": {"low": 1, "high": 5}}})"));
    REQUIRE(fixture.Context().Sync());
    CHECK(settings.span.low == 1 && settings.span.high == 5);
}

// byte 배열(`Array<uint8>`) 설정 값은 Base64 문자열이고, 배열 연산
// 객체($clear·$remove·$add·$append)는 InvalidArrayOperation 진단으로 거부된다(CV4 리뷰 2 — 전에는
// 적용 전체가 하드 오류였다). 값은 그대로다.
TEST(Config_ByteArrayRejectsArrayOperations) {
    ConfigFixture fixture;
    REQUIRE(fixture.Context().RegisterSettings<config_fixture::BlobSettings>());
    REQUIRE(fixture.Context().Sync());
    const auto& settings = fixture.Context().Get<config_fixture::BlobSettings>();
    REQUIRE(fixture.Context().AddLayerText(ConfigLayer::Engine, "x", "Render", "<test>",
                                           R"({"BlobCheck": {"blob": "AQID"}})"));
    REQUIRE(fixture.Context().Sync());
    REQUIRE(settings.blob.Size() == 3);
    CHECK(settings.blob[0] == 1 && settings.blob[2] == 3);
    const char* const operations[] = {R"({"BlobCheck": {"blob": {"$clear": true}}})",
                                      R"({"BlobCheck": {"blob": {"$remove": [1]}}})",
                                      R"({"BlobCheck": {"blob": {"$add": [4]}}})",
                                      R"({"BlobCheck": {"blob": {"$append": [4]}}})"};
    for (const char* text : operations) {
        CHECK(fixture.Rejects(ConfigLayer::Project, "Render", text));
        CHECK(fixture.HasDiagnostic(ConfigDiagnosticCode::InvalidArrayOperation, 1, 24));
        CHECK(settings.blob.Size() == 3);
    }
}

// InterpCurve 설정(CoreTypeBindings 미러 — 묶음 리뷰 5): 정렬된 키는 받고 정렬을 깨는 층 값은 hook
// 거부(InvalidStructValue)이며 값이 그대로다. 묶음 리뷰 4: 배열 연산 `$remove`를 입력값
// 내림차순으로 적어도 결과가 정렬돼 있으면 받는다 — 연산 목록의 정규화(중간 import)는 hook을 보지
// 않고 최종 값만 hook이 본다.
TEST(Config_InterpCurveSettingsValidateFinalValue) {
    ConfigFixture fixture;
    REQUIRE(fixture.Context().RegisterSettings<config_fixture::CurveSettings>());
    REQUIRE(fixture.Context().Sync());
    const auto& settings = fixture.Context().Get<config_fixture::CurveSettings>();
    CHECK(fixture.Rejects(
        ConfigLayer::Engine, "Render",
        R"({"CurveCheck": {"curve": {"points": [{"inValue": 2}, {"inValue": 1}]}}})"));
    CHECK(fixture.HasDiagnostic(ConfigDiagnosticCode::FieldValue, 0, 0,
                                reflection_text::TextDiagnosticCode::InvalidStructValue));
    CHECK(settings.curve.PointCount() == 0);
    REQUIRE(fixture.Context().AddLayerText(
        ConfigLayer::Engine, "x", "Render", "<test>",
        R"({"CurveCheck": {"curve": {"points": [{"inValue": 0, "outValue": 1},
            {"inValue": 1, "outValue": 2}, {"inValue": 2, "outValue": 3}]}}})"));
    REQUIRE(fixture.Context().Sync());
    REQUIRE(settings.curve.PointCount() == 3);
    REQUIRE(fixture.Context().AddLayerText(
        ConfigLayer::Project, "y", "Render", "<test>",
        R"({"CurveCheck": {"curve": {"points": {"$remove": [{"inValue": 2, "outValue": 3},
            {"inValue": 0, "outValue": 1}]}}}})"));
    REQUIRE(fixture.Context().Sync());
    REQUIRE(settings.curve.PointCount() == 1);
    CHECK(settings.curve.GetPoint(0).inValue == 1.0f &&
          settings.curve.GetPoint(0).outValue == 2.0f);
}
} // namespace config_layer_tests_file
