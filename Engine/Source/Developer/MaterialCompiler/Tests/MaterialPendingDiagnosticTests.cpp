#include <MaterialCompiler/MaterialSourceWriter.h>

#include "MaterialGraphFixture.h"

// MS2c1 시험이다(Docs/Design/MaterialSystem 6.10절 늦은 진단, 19.2.1절): TODO 노드(FontSample —
// MPQ-Font)와 모르는 노드는 static 해석·DCE 뒤 살아 있을 때만 진단하고, 조건 값 가지로만 닿으면
// 경고다(번역은 성공하고 원문은 그 자리를 0으로 낸다).
using namespace rocks;
using namespace rocks::material;
namespace material_pending_diagnostic_tests_file {

using namespace material_graph_fixture;

namespace {

usize CountWarnings(const MaterialTranslation& out, MaterialDiagnosticCode code) noexcept {
    return CountDiagnostics(out, code, nodegraph::GraphDiagnosticSeverity::Warning);
}

} // namespace

// 진입점에 닿는 TODO 노드는 PrerequisiteMissing 오류 하나다(선행 ID가 문구에 있다).
TEST(MaterialPending_ReachedTodoNodeIsError) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid font = fixture.Add("Material.FontSample");
    fixture.Connect(font, "R", OutputNodeId, "AmbientOcclusion");
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(), out));
    CHECK(!out.succeeded);
    CHECK(HasOnlyErrorAt(out, MaterialDiagnosticCode::PrerequisiteMissing, font));
    bool named = false;
    for (const nodegraph::GraphDiagnostic& diagnostic : out.diagnostics.items) {
        const StringView text = diagnostic.message.View();
        for (usize at = 0; at + 8 <= text.ByteCount(); ++at) {
            named = named || StringView(text.Data() + at, 8) == StringView("MPQ-Font");
        }
    }

    CHECK(named);
}

// DCE·static 해석으로 빠진 TODO 노드와 모르는 노드는 진단이 없다.
TEST(MaterialPending_DeadNodesAreSilent) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid font = fixture.Add("Material.FontSample");
    const Guid unknown = fixture.Add("Material.DoesNotExist");
    const Guid add = fixture.Add("Material.Add");
    fixture.Connect(font, "R", add, "A");
    fixture.Connect(unknown, "Result", add, "B");
    const Guid lerp = fixture.Add("Material.LinearInterpolate");
    fixture.Connect(AddConstant(fixture, 0.25f), "Result", lerp, "A");
    fixture.Connect(add, "Result", lerp, "B");
    fixture.Connect(AddConstant(fixture, 0.0f), "Result", lerp, "Alpha");
    fixture.Connect(lerp, "Result", OutputNodeId, "AmbientOcclusion");
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(), out));
    CHECK(out.succeeded);
    CHECK(CountErrors(out) == 0);
    CHECK(CountWarnings(out, MaterialDiagnosticCode::PrerequisiteMissing) == 0);
    CHECK(CountWarnings(out, MaterialDiagnosticCode::UnknownNode) == 0);
}

// 조건 값(렌더 경로) 가지로만 닿으면 경고이고 번역은 성공한다. 원문은 자리를 0으로 낸다.
TEST(MaterialPending_ConditionalBranchWarns) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid font = fixture.Add("Material.FontSample");
    const Guid select = fixture.Add("Material.RenderPathSwitch");
    fixture.Connect(AddConstant(fixture, 0.1f), "Result", select, "Default");
    fixture.Connect(font, "R", select, "Deferred");
    fixture.Connect(select, "Result", OutputNodeId, "AmbientOcclusion");
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(), out));
    CHECK(out.succeeded);
    CHECK(CountErrors(out) == 0);
    CHECK(CountWarnings(out, MaterialDiagnosticCode::PrerequisiteMissing) == 1);
    MaterialGeneratedSource source(env.allocator);
    REQUIRE(
        WriteMaterialSource(env.allocator, out.module, Traits(), TestAssetId, MainGraphId, source));
    CHECK(source.succeeded);

    // 같은 노드가 조건 밖으로도 닿으면 오류다.
    GraphFixture both(env);
    const Guid shared = both.Add("Material.FontSample");
    const Guid path = both.Add("Material.RenderPathSwitch");
    both.Connect(AddConstant(both, 0.1f), "Result", path, "Default");
    both.Connect(shared, "R", path, "Deferred");
    const Guid add = both.Add("Material.Add");
    both.Connect(path, "Result", add, "A");
    both.Connect(shared, "G", add, "B");
    both.Connect(add, "Result", OutputNodeId, "AmbientOcclusion");
    MaterialTranslation plain(env.allocator);
    REQUIRE(Translate(both, Traits(), plain));
    CHECK(!plain.succeeded);
    CHECK(HasErrorAt(plain, MaterialDiagnosticCode::PrerequisiteMissing, shared));
}

// 모르는 노드가 진입점에 닿으면 V12 오류 하나다.
TEST(MaterialPending_ReachedUnknownNodeIsError) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid unknown = fixture.Add("Material.DoesNotExist");
    fixture.Connect(unknown, "Result", OutputNodeId, "AmbientOcclusion");
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(), out));
    CHECK(!out.succeeded);
    CHECK(HasOnlyErrorAt(out, MaterialDiagnosticCode::UnknownNode, unknown));
}

} // namespace material_pending_diagnostic_tests_file
