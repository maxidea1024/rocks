#include <MaterialSchema/MaterialContextValue.h>
#include <MaterialSchema/Nodes/Constants/StaticBoolProperties.generated.h>
#include <MaterialSchema/Nodes/Constants/StaticBoolProperties.h>
#include <MaterialSchema/Nodes/Switches/FormatFactInput.generated.h>
#include <MaterialSchema/Nodes/Switches/FormatFactInput.h>
#include <MaterialSchema/Nodes/Switches/FormatFactSwitchProperties.generated.h>
#include <MaterialSchema/Nodes/Switches/FormatFactSwitchProperties.h>
#include <MaterialSchema/Nodes/Switches/SamplerBudgetSwitchProperties.generated.h>
#include <MaterialSchema/Nodes/Switches/SamplerBudgetSwitchProperties.h>
#include <MaterialSchema/Nodes/Switches/StaticSwitchProperties.generated.h>
#include <MaterialSchema/Nodes/Switches/StaticSwitchProperties.h>

#include "MaterialGraphFixture.h"

// MS2b3 시험이다(Docs/Design/MaterialSystem 6.9절 스위치): 번역 때 고르는 스위치는 고른 가지만
// 번역하고(고르지 않은 가지의 오류는 없다 — 늦은 입력), 읽은 번역 대상 항목을 기록하며, 실행 중 값
// 스위치는 Bool 문맥 값의 Select다. V7(static 아닌 Value)·V15(깊이 진입점의 렌더 경로)·늦은 가지의
// 순환(V2) 음성 대조를 함께 본다.
using namespace rocks;
using namespace rocks::material;
namespace material_switch_lowering_tests_file {

using namespace material_graph_fixture;

namespace {

uint64 ContextIndex(StringView name) noexcept {
    const Span<const MaterialContextValue> table = MaterialContextValues();
    for (usize index = 0; index < table.Size(); ++index) {
        if (table[index].name == name) {
            return index;
        }
    }

    return table.Size();
}

bool HasExtern(const MaterialTranslation& out, StringView name) noexcept {
    for (const IrValue& value : out.module.Values()) {
        if (value.op == IrOp::Extern && value.immediate[0] == ContextIndex(name)) {
            return true;
        }
    }

    return false;
}

const IrValue* EntryValue(const MaterialTranslation& out, IrEntry entry, StringView slot) noexcept {
    for (const IrResult& result : out.module.Results()) {
        if (result.entry == entry && result.slot == slot) {
            return &ValueOf(out, result.value);
        }
    }

    return nullptr;
}

// 연결 없는 Ddx다(번역하면 MissingInput — 고르지 않은 가지 표지).
Guid AddBroken(GraphFixture& fixture) noexcept {
    return fixture.Add("Material.Ddx");
}

Guid AddStaticBool(GraphFixture& fixture, bool value) noexcept {
    StaticBoolProperties properties;
    properties.value = value;
    return fixture.Add("Material.StaticBool", fixture.env.Save(properties));
}

bool IsFloat(const IrValue* value, float expected) noexcept {
    return value != nullptr && value->op == IrOp::Constant && value->type == IrFloat(1) &&
           FloatOf(*value, 0) == expected;
}

} // namespace

// StaticSwitch: 고른 가지만 번역한다. Value 연결이 없으면 defaultValue, static이 아니면 V7.
TEST(MaterialSwitch_StaticSwitch) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    {
        GraphFixture fixture(env);
        const Guid select =
            fixture.Add("Material.StaticSwitch", env.Save(StaticSwitchProperties{}));
        fixture.Connect(AddStaticBool(fixture, true), "Result", select, "Value");
        fixture.Connect(AddConstant(fixture, 0.25f), "Result", select, "A");
        fixture.Connect(AddBroken(fixture), "Result", select, "B");
        fixture.Connect(select, "Result", OutputNodeId, "AmbientOcclusion");
        MaterialTranslation out(env.allocator);
        REQUIRE(Translate(fixture, Traits(), out));
        CHECK(out.succeeded);
        CHECK(CountErrors(out) == 0);
        CHECK(IsFloat(EntryValue(out, IrEntry::Surface, "AmbientOcclusion"), 0.25f));
    }

    {
        // Value 연결이 없으면 defaultValue(거짓 → B)다.
        GraphFixture fixture(env);
        const Guid select =
            fixture.Add("Material.StaticSwitch", env.Save(StaticSwitchProperties{}));
        fixture.Connect(AddBroken(fixture), "Result", select, "A");
        fixture.Connect(AddConstant(fixture, 0.75f), "Result", select, "B");
        fixture.Connect(select, "Result", OutputNodeId, "AmbientOcclusion");
        MaterialTranslation out(env.allocator);
        REQUIRE(Translate(fixture, Traits(), out));
        CHECK(out.succeeded);
        CHECK(IsFloat(EntryValue(out, IrEntry::Surface, "AmbientOcclusion"), 0.75f));
    }

    {
        // static이 아닌 Value는 V7 하나다.
        GraphFixture fixture(env);
        const Guid select =
            fixture.Add("Material.StaticSwitch", env.Save(StaticSwitchProperties{}));
        const Guid varying = AddVarying(fixture, 0.5f);
        fixture.Connect(varying, "Result", select, "Value");
        fixture.Connect(AddConstant(fixture, 0.25f), "Result", select, "A");
        fixture.Connect(AddConstant(fixture, 0.75f), "Result", select, "B");
        fixture.Connect(select, "Result", OutputNodeId, "AmbientOcclusion");
        MaterialTranslation out(env.allocator);
        REQUIRE(Translate(fixture, Traits(), out));
        CHECK(!out.succeeded);
        CHECK(HasOnlyErrorAt(out, MaterialDiagnosticCode::NonStaticValue, select));
    }

    {
        // 고른 가지에 연결이 없으면 MissingInput이다.
        GraphFixture fixture(env);
        const Guid select =
            fixture.Add("Material.StaticSwitch", env.Save(StaticSwitchProperties{}));
        fixture.Connect(AddStaticBool(fixture, true), "Result", select, "Value");
        fixture.Connect(AddConstant(fixture, 0.75f), "Result", select, "B");
        fixture.Connect(select, "Result", OutputNodeId, "AmbientOcclusion");
        MaterialTranslation out(env.allocator);
        REQUIRE(Translate(fixture, Traits(), out));
        CHECK(HasOnlyErrorAt(out, MaterialDiagnosticCode::MissingInput, select));
    }
}

// QualitySwitch: 대상 품질 입력(연결이 없으면 Default)만 번역하고 Quality 항목을 기록한다.
TEST(MaterialSwitch_QualitySwitch) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    const auto build = [&](GraphFixture& fixture) noexcept {
        const Guid select = fixture.Add("Material.QualitySwitch");
        fixture.Connect(AddConstant(fixture, 0.1f), "Result", select, "Default");
        fixture.Connect(AddConstant(fixture, 0.2f), "Result", select, "High");
        fixture.Connect(AddBroken(fixture), "Result", select, "Low");
        fixture.Connect(select, "Result", OutputNodeId, "AmbientOcclusion");
        return select;
    };

    MaterialTranslationTarget high;
    high.quality = MaterialQualityLevel::High;
    GraphFixture first(env);
    (void)build(first);
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(first, Traits(), out, high));
    CHECK(out.succeeded);
    CHECK(IsFloat(EntryValue(out, IrEntry::Surface, "AmbientOcclusion"), 0.2f));
    CHECK(out.consultedTarget == ToMask(MaterialTargetField::Quality));

    MaterialTranslationTarget medium;
    medium.quality = MaterialQualityLevel::Medium;
    GraphFixture second(env);
    (void)build(second);
    MaterialTranslation fallback(env.allocator);
    REQUIRE(Translate(second, Traits(), fallback, medium));
    CHECK(IsFloat(EntryValue(fallback, IrEntry::Surface, "AmbientOcclusion"), 0.1f));

    MaterialTranslationTarget low;
    low.quality = MaterialQualityLevel::Low;
    GraphFixture third(env);
    const Guid select = build(third);
    (void)select;
    MaterialTranslation broken(env.allocator);
    REQUIRE(Translate(third, Traits(), broken, low));
    CHECK(!broken.succeeded);
    CHECK(CountErrors(broken) == 1);

    // 대상을 읽지 않는 머티리얼은 기록이 없다.
    GraphFixture plain(env);
    MaterialTranslation none(env.allocator);
    REQUIRE(Translate(plain, Traits(), none, high));
    CHECK(none.consultedTarget == NoMaterialTargetFields);
}

// RenderPathSwitch: 조건 값 Select(편차 — 6.3절 3항). 깊이 진입점(WPO)에 닿으면 V15다.
TEST(MaterialSwitch_RenderPath) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid select = fixture.Add("Material.RenderPathSwitch");
    fixture.Connect(AddConstant(fixture, 0.1f), "Result", select, "Default");
    fixture.Connect(AddConstant(fixture, 0.2f), "Result", select, "Deferred");
    fixture.Connect(select, "Result", OutputNodeId, "AmbientOcclusion");
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(), out));
    REQUIRE(out.succeeded);
    CHECK(HasExtern(out, "RenderPathDeferred"));
    // Forward 입력이 없어 Forward+ 쪽은 Default와 같은 값이라 조건이 없다.
    CHECK(!HasExtern(out, "RenderPathForwardPlus"));

    fixture.Connect(select, "Result", OutputNodeId, "WorldPositionOffset");
    MaterialTranslation depth(env.allocator);
    REQUIRE(Translate(fixture, Traits(), depth));
    CHECK(!depth.succeeded);
    CHECK(HasOnlyErrorAt(depth, MaterialDiagnosticCode::RenderPathDepth, select));
}

// StageSwitch는 문맥마다 다른 입력이다: 정점 문맥에 픽셀 전용 값(미분)이 닿지 않는다.
// PreviousFrameSwitch는 이전 정점 문맥에서 PreviousFrame이다.
TEST(MaterialSwitch_StageAndPreviousFrame) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    {
        GraphFixture fixture(env);
        const Guid select = fixture.Add("Material.StageSwitch");
        const Guid derivative = fixture.Add("Material.Ddx");
        fixture.Connect(AddVarying(fixture, 0.5f), "Result", derivative, "Value");
        fixture.Connect(derivative, "Result", select, "PixelShader");
        fixture.Connect(AddConstant(fixture, 0.25f), "Result", select, "VertexShader");
        fixture.Connect(select, "Result", OutputNodeId, "WorldPositionOffset");
        fixture.Connect(select, "Result", OutputNodeId, "AmbientOcclusion");
        MaterialTranslation out(env.allocator);
        REQUIRE(Translate(fixture, Traits(), out));
        CHECK(out.succeeded);
        CHECK(EntryValue(out, IrEntry::Surface, "AmbientOcclusion")->op == IrOp::Ddx);
    }

    {
        GraphFixture fixture(env);
        const Guid select = fixture.Add("Material.PreviousFrameSwitch");
        fixture.Connect(AddConstant(fixture, 1.0f), "Result", select, "CurrentFrame");
        fixture.Connect(AddConstant(fixture, 2.0f), "Result", select, "PreviousFrame");
        fixture.Connect(select, "Result", OutputNodeId, "WorldPositionOffset");
        MaterialTranslation out(env.allocator);
        REQUIRE(Translate(fixture, Traits(), out));
        REQUIRE(out.succeeded);
        const IrValue* current = EntryValue(out, IrEntry::Vertex, "WorldPositionOffset");
        const IrValue* previous = EntryValue(out, IrEntry::VertexPrevious, "WorldPositionOffset");
        REQUIRE(current != nullptr);
        REQUIRE(previous != nullptr);
        CHECK(FloatOf(*current, 0) == 1.0f);
        CHECK(FloatOf(*previous, 0) == 2.0f);
    }
}

// FormatFactSwitch·SamplerBudgetSwitch: 대상 형식 사실·샘플러 상한으로 고르고 그 항목을 기록한다.
TEST(MaterialSwitch_FormatFacts) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    const auto build = [&](GraphFixture& fixture, StringView name) noexcept {
        FormatFactSwitchProperties properties(env.allocator);
        FormatFactInput input;
        input.inputName = env.Intern(name);
        input.propertyCondition = FormatFactCondition::True;
        (void)properties.ddspiPropertyNames.TryPushBack(input);
        const Guid select = fixture.Add("Material.FormatFactSwitch", env.Save(properties));
        fixture.Connect(AddConstant(fixture, 0.2f), "Result", select, "InputTrue");
        fixture.Connect(AddConstant(fixture, 0.1f), "Result", select, "InputFalse");
        fixture.Connect(select, "Result", OutputNodeId, "AmbientOcclusion");
        return select;
    };

    MaterialTranslationTarget mobile;
    mobile.isMobile = true;
    mobile.isPc = false;
    GraphFixture fixture(env);
    (void)build(fixture, "IsMobile");
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(), out, mobile));
    CHECK(IsFloat(EntryValue(out, IrEntry::Surface, "AmbientOcclusion"), 0.2f));
    CHECK(out.consultedTarget == ToMask(MaterialTargetField::IsMobile));

    GraphFixture invalid(env);
    const Guid wrong = build(invalid, "IsToaster");
    MaterialTranslation rejected(env.allocator);
    REQUIRE(Translate(invalid, Traits(), rejected, mobile));
    CHECK(HasOnlyErrorAt(rejected, MaterialDiagnosticCode::InvalidProperty, wrong));

    GraphFixture budget(env);
    SamplerBudgetSwitchProperties properties;
    properties.requiredSamplers = 32;
    const Guid select = budget.Add("Material.SamplerBudgetSwitch", env.Save(properties));
    budget.Connect(AddConstant(budget, 0.2f), "Result", select, "InputTrue");
    budget.Connect(AddConstant(budget, 0.1f), "Result", select, "InputFalse");
    budget.Connect(select, "Result", OutputNodeId, "AmbientOcclusion");
    MaterialTranslation over(env.allocator);
    REQUIRE(Translate(budget, Traits(), over));
    CHECK(IsFloat(EntryValue(over, IrEntry::Surface, "AmbientOcclusion"), 0.1f));
    CHECK(over.consultedTarget == ToMask(MaterialTargetField::MaxSamplers));
}

// 실행 중 값 스위치는 두 입력을 모두 번역한다(고르지 않은 쪽의 오류도 남는다 — UE GenericSwitch).
// 지원하지 않는 대상의 RayTracingQualitySwitch는 RayTraced를 번역하지 않는다.
TEST(MaterialSwitch_RuntimeAndPlatformGated) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    {
        GraphFixture fixture(env);
        const Guid select = fixture.Add("Material.ShadowPassSwitch");
        fixture.Connect(AddConstant(fixture, 0.1f), "Result", select, "Default");
        fixture.Connect(AddVarying(fixture, 0.5f), "Result", select, "Shadow");
        fixture.Connect(select, "Result", OutputNodeId, "AmbientOcclusion");
        MaterialTranslation out(env.allocator);
        REQUIRE(Translate(fixture, Traits(), out));
        REQUIRE(out.succeeded);
        CHECK(EntryValue(out, IrEntry::Surface, "AmbientOcclusion")->op == IrOp::Select);
        CHECK(HasExtern(out, "ShadowPass"));
    }

    {
        GraphFixture fixture(env);
        const Guid select = fixture.Add("Material.ShadowPassSwitch");
        fixture.Connect(AddConstant(fixture, 0.1f), "Result", select, "Default");
        fixture.Connect(AddBroken(fixture), "Result", select, "Shadow");
        fixture.Connect(select, "Result", OutputNodeId, "AmbientOcclusion");
        MaterialTranslation out(env.allocator);
        REQUIRE(Translate(fixture, Traits(), out));
        CHECK(CountErrors(out) == 1);
    }

    {
        GraphFixture fixture(env);
        const Guid select = fixture.Add("Material.RayTracingQualitySwitch");
        fixture.Connect(AddConstant(fixture, 0.1f), "Result", select, "Normal");
        fixture.Connect(AddBroken(fixture), "Result", select, "RayTraced");
        fixture.Connect(select, "Result", OutputNodeId, "AmbientOcclusion");
        MaterialTranslation out(env.allocator);
        REQUIRE(Translate(fixture, Traits(), out));
        CHECK(out.succeeded);
        CHECK(IsFloat(EntryValue(out, IrEntry::Surface, "AmbientOcclusion"), 0.1f));
        CHECK(out.consultedTarget == ToMask(MaterialTargetField::RayTracing));
    }

    {
        // 조명 굽기 컴파일러는 Lightmass 입력이다.
        GraphFixture fixture(env);
        const Guid select = fixture.Add("Material.LightBakePassSwitch");
        fixture.Connect(AddConstant(fixture, 0.1f), "Result", select, "Realtime");
        fixture.Connect(AddConstant(fixture, 0.3f), "Result", select, "Lightmass");
        fixture.Connect(select, "Result", OutputNodeId, "AmbientOcclusion");
        MaterialTranslationTarget bake;
        bake.lightBake = true;
        MaterialTranslation out(env.allocator);
        REQUIRE(Translate(fixture, Traits(), out, bake));
        CHECK(IsFloat(EntryValue(out, IrEntry::Surface, "AmbientOcclusion"), 0.3f));
    }
}

// 늦은 가지의 순환도 V2 하나다(가지 계획이 번역 중인 스위치에 닿는다).
TEST(MaterialSwitch_LazyBranchCycle) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid select = fixture.Add("Material.QualitySwitch");
    const Guid add = fixture.Add("Material.Add");
    fixture.Connect(select, "Result", add, "A");
    fixture.Connect(add, "Result", select, "Default");
    fixture.Connect(select, "Result", OutputNodeId, "AmbientOcclusion");
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(), out));
    CHECK(!out.succeeded);
    CHECK(CountDiagnostics(out, MaterialDiagnosticCode::Cycle,
                           nodegraph::GraphDiagnosticSeverity::Error) == 1);
    CHECK(CountErrors(out) == 1);
}

// 리뷰 반영: QualitySwitch는 고른 입력만 번역한다(고르지 않은 Default의 오류는 없다). 실행 중
// 스위치의 필수 입력은 UE 문구로 검사한다. CustomizedUv에 닿는 렌더 경로는 V15가 아니다.
TEST(MaterialSwitch_ReviewRegressions) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    {
        GraphFixture fixture(env);
        const Guid select = fixture.Add("Material.QualitySwitch");
        fixture.Connect(AddBroken(fixture), "Result", select, "Default");
        fixture.Connect(AddConstant(fixture, 0.4f), "Result", select, "Epic");
        fixture.Connect(select, "Result", OutputNodeId, "AmbientOcclusion");
        MaterialTranslationTarget epic;
        epic.quality = MaterialQualityLevel::Epic;
        MaterialTranslation out(env.allocator);
        REQUIRE(Translate(fixture, Traits(), out, epic));
        CHECK(out.succeeded);
        CHECK(IsFloat(EntryValue(out, IrEntry::Surface, "AmbientOcclusion"), 0.4f));
    }

    {
        GraphFixture fixture(env);
        const Guid select = fixture.Add("Material.ShadowPassSwitch");
        fixture.Connect(AddConstant(fixture, 0.1f), "Result", select, "Default");
        fixture.Connect(select, "Result", OutputNodeId, "AmbientOcclusion");
        MaterialTranslation out(env.allocator);
        REQUIRE(Translate(fixture, Traits(), out));
        CHECK(HasOnlyErrorAt(out, MaterialDiagnosticCode::MissingInput, select));
    }

    {
        GraphFixture fixture(env);
        const Guid select = fixture.Add("Material.RenderPathSwitch");
        fixture.Connect(AddConstant(fixture, 0.1f), "Result", select, "Default");
        fixture.Connect(AddConstant(fixture, 0.2f), "Result", select, "Deferred");
        const Guid append = fixture.Add("Material.Append");
        fixture.Connect(select, "Result", append, "A");
        fixture.Connect(select, "Result", append, "B");
        fixture.Connect(append, "Result", OutputNodeId, "CustomizedUv0");
        const Guid uv = fixture.Add("Material.TextureCoordinate");
        fixture.Connect(uv, "Result", OutputNodeId, "AmbientOcclusion");
        MaterialTraitsDesc traits = Traits();
        traits.customizedUvCount = 1;
        MaterialTranslation out(env.allocator);
        REQUIRE(Translate(fixture, traits, out));
        CHECK(CountErrors(out) == 0);
    }
}

} // namespace material_switch_lowering_tests_file
