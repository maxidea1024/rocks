#include <Core/Text/String.h>
#include <MaterialSchema/Nodes/Switches/FormatFactInput.generated.h>
#include <MaterialSchema/Nodes/Switches/FormatFactInput.h>
#include <MaterialSchema/Nodes/Switches/SwitchesNodeEnums.h>

#include "LoweringContext.h"
#include "LoweringTable.h"
#include "Translator.h"

// 스위치 노드 lowering이다(Docs/Design/MaterialSystem 6.9절 MS2b3). 번역 때 고르는 스위치는 고른
// 입력만 번역하고(늦은 입력 — 결과 타입은 그 입력 타입), 실행 중 값 스위치는 입력을 모두 번역해
// Bool 문맥 값을 조건으로 한 Select를 낸다(UE GenericSwitch —
// HLSLMaterialTranslator.cpp:11603–11650).
namespace rocks::material::detail {

namespace {

// key 입력의 값이다. 연결이 없으면 message로 MissingInput을 진단하고 NoIrValue다.
Result<IrValueId> RequiredInput(LoweringContext& context, StringView key,
                                StringView message) noexcept {
    auto connected = context.IsConnected(key);
    if (!connected) {
        return Result<IrValueId>::Failure(connected.Error());
    }

    if (!connected.Value()) {
        if (auto reported = context.Error(MaterialDiagnosticCode::MissingInput, key, message);
            !reported) {
            return Result<IrValueId>::Failure(reported.Error());
        }

        return Result<IrValueId>::Success(NoIrValue);
    }

    return context.Input(key);
}

// 둘 다 필수인 입력의 연결을 확인한다(UE는 고르기 전에 두 입력을 모두 검사한다). 둘 다 있으면
// 참이다.
Result<bool> RequireBoth(LoweringContext& context, StringView first, StringView firstMessage,
                         StringView second, StringView secondMessage) noexcept {
    const StringView keys[] = {first, second};
    const StringView messages[] = {firstMessage, secondMessage};
    bool all = true;
    for (usize index = 0; index < 2; ++index) {
        auto connected = context.IsConnected(keys[index]);
        if (!connected) {
            return Result<bool>::Failure(connected.Error());
        }

        if (!connected.Value()) {
            all = false;
            if (auto reported = context.Error(MaterialDiagnosticCode::MissingInput, keys[index],
                                              messages[index]);
                !reported) {
                return Result<bool>::Failure(reported.Error());
            }
        }
    }

    return Result<bool>::Success(all);
}

// 번역 때 고르는 스위치의 고른 입력 하나다: Default 연결이 없으면 message로 MissingInput, chosen이
// 연결되면 그것, 아니면 Default만 번역한다(UE GetEffectiveInput —
// MaterialExpressions.cpp:9067–9107).
Result<IrValueId> ChosenOrDefault(LoweringContext& context, StringView chosen,
                                  StringView message) noexcept {
    auto hasDefault = context.IsConnected("Default");
    if (!hasDefault) {
        return Result<IrValueId>::Failure(hasDefault.Error());
    }

    if (!hasDefault.Value()) {
        if (auto reported = context.Error(MaterialDiagnosticCode::MissingInput, "Default", message);
            !reported) {
            return Result<IrValueId>::Failure(reported.Error());
        }

        return Result<IrValueId>::Success(NoIrValue);
    }

    if (!chosen.IsEmpty()) {
        auto connected = context.IsConnected(chosen);
        if (!connected) {
            return Result<IrValueId>::Failure(connected.Error());
        }

        if (connected.Value()) {
            return context.Input(chosen);
        }
    }

    return context.Input("Default");
}

// key가 연결되면 그 값, 아니면 fallback이다(UE "연결되지 않은 입력은 Default").
Result<IrValueId> InputOr(LoweringContext& context, StringView key, IrValueId fallback) noexcept {
    auto connected = context.IsConnected(key);
    if (!connected) {
        return Result<IrValueId>::Failure(connected.Error());
    }

    return connected.Value() ? context.Input(key) : Result<IrValueId>::Success(fallback);
}

// Select(조건 문맥 값 condition, whenTrue, whenFalse)다(UE GenericSwitch — 한쪽이 값 없으면 남은
// 쪽).
Result<IrValueId> SwitchOn(LoweringContext& context, StringView condition, IrValueId whenTrue,
                           IrValueId whenFalse) noexcept {
    if (whenTrue == NoIrValue || whenFalse == NoIrValue) {
        return Result<IrValueId>::Success(NoIrValue);
    }

    if (whenTrue == whenFalse) {
        return Result<IrValueId>::Success(whenTrue);
    }

    auto test = context.ContextValue(condition);
    if (!test) {
        return test;
    }

    return context.Select(test.Value(), whenTrue, whenFalse);
}

Result<void> EmitSwitch(LoweringContext& context, Result<IrValueId> value) noexcept {
    if (!value) {
        return Result<void>::Failure(value.Error());
    }

    return context.Output("Result", value.Value());
}

// UE StaticSwitch(MaterialExpressions.cpp:8889–8914, HLSLMaterialTranslator.cpp:9018–9025).
Result<void> LowerStaticSwitch(LoweringContext& context) noexcept {
    bool value = false;
    auto read = context.ReadBool("defaultValue", false, value);
    if (!read || !read.Value()) {
        return read ? Result<void>::Success() : Result<void>::Failure(read.Error());
    }

    auto connected = context.IsConnected("Value");
    if (!connected) {
        return Result<void>::Failure(connected.Error());
    }

    if (connected.Value()) {
        auto condition = context.Input("Value");
        if (!condition) {
            return Result<void>::Failure(condition.Error());
        }

        if (condition.Value() == NoIrValue) {
            // 위쪽 오류나 V7(StaticOnly)가 이미 진단했다.
            return Result<void>::Success();
        }

        if (!context.ConstantBool(condition.Value(), value)) {
            return context.Error(MaterialDiagnosticCode::NonStaticValue, "Value",
                                 "Failed to cast Value input to static bool type");
        }
    }

    const StringView chosen = value ? StringView("A") : StringView("B");
    return EmitSwitch(
        context, RequiredInput(context, chosen, "Static switch has no input for the chosen value"));
}

// UE QualitySwitch(MaterialExpressions.cpp:9067–9087).
Result<void> LowerQualitySwitch(LoweringContext& context) noexcept {
    constexpr StringView Levels[] = {"Low", "High", "Medium", "Epic"};
    const MaterialQualityLevel quality = context.Target(MaterialTargetField::Quality).quality;
    const uint32 level = static_cast<uint32>(quality);
    const StringView chosen =
        quality == MaterialQualityLevel::None || level >= sizeof(Levels) / sizeof(Levels[0])
            ? StringView()
            : Levels[level];
    return EmitSwitch(context,
                      ChosenOrDefault(context, chosen, "Quality switch missing default input"));
}

// 렌더 경로(6.3절 3항 — 조건 값). Mobile 입력은 Rocks 렌더 경로에 없어 쓰지 않는다.
Result<void> LowerRenderPathSwitch(LoweringContext& context) noexcept {
    auto fallback = RequiredInput(context, "Default", "Shading path switch missing default input");
    if (!fallback || fallback.Value() == NoIrValue) {
        return fallback ? Result<void>::Success() : Result<void>::Failure(fallback.Error());
    }

    auto deferred = InputOr(context, "Deferred", fallback.Value());
    auto forward = InputOr(context, "Forward", fallback.Value());
    if (!deferred || !forward) {
        return Result<void>::Failure(!deferred ? deferred.Error() : forward.Error());
    }

    auto inner = SwitchOn(context, "RenderPathForwardPlus", forward.Value(), fallback.Value());
    if (!inner) {
        return Result<void>::Failure(inner.Error());
    }

    return EmitSwitch(context,
                      SwitchOn(context, "RenderPathDeferred", deferred.Value(), inner.Value()));
}

// 기능 등급(19.3.4절 — 조건 값). Low 품질 모바일 흉내면 Mobile 입력만(UE
// MaterialExpressions.cpp:9224–9229).
Result<void> LowerCapabilityTierSwitch(LoweringContext& context) noexcept {
    const bool simulate = context.Target(MaterialTargetField::SimulateMobileForLowQuality)
                              .simulateMobileForLowQuality;
    if (simulate &&
        context.Target(MaterialTargetField::Quality).quality == MaterialQualityLevel::Low) {
        // 번역 때 고른다(고른 입력 하나만 — UE MaterialExpressions.cpp:9224–9243).
        return EmitSwitch(context, ChosenOrDefault(context, "ES3_1",
                                                   "Feature Level switch missing default input"));
    }

    auto fallback = RequiredInput(context, "Default", "Feature Level switch missing default input");
    if (!fallback || fallback.Value() == NoIrValue) {
        return fallback ? Result<void>::Success() : Result<void>::Failure(fallback.Error());
    }

    auto mobile = InputOr(context, "ES3_1", fallback.Value());
    auto sm5 = InputOr(context, "SM5", fallback.Value());
    auto sm6 = InputOr(context, "SM6", fallback.Value());
    if (!mobile || !sm5 || !sm6) {
        return Result<void>::Failure(!mobile ? mobile.Error() : !sm5 ? sm5.Error() : sm6.Error());
    }

    auto inner = SwitchOn(context, "CapabilityTierSm5", sm5.Value(), mobile.Value());
    if (!inner) {
        return Result<void>::Failure(inner.Error());
    }

    return EmitSwitch(context, SwitchOn(context, "CapabilityTierSm6", sm6.Value(), inner.Value()));
}

// UE ShaderStageSwitch(MaterialExpressions.cpp:17651–17660).
Result<void> LowerStageSwitch(LoweringContext& context) noexcept {
    auto both = RequireBoth(context, "PixelShader", "Missing input PixelShader", "VertexShader",
                            "Missing input VertexShader");
    if (!both || !both.Value()) {
        return both ? Result<void>::Success() : Result<void>::Failure(both.Error());
    }

    return EmitSwitch(context, context.Input(context.Context() == EvaluationContext::Pixel
                                                 ? StringView("PixelShader")
                                                 : StringView("VertexShader")));
}

// UE PreviousFrameSwitch(MaterialExpressions.cpp:9026–9033).
Result<void> LowerPreviousFrameSwitch(LoweringContext& context) noexcept {
    const bool previous = context.Context() == EvaluationContext::VertexPrevious;
    return EmitSwitch(
        context,
        RequiredInput(context, previous ? StringView("PreviousFrame") : StringView("CurrentFrame"),
                      "Previous frame switch has no input for this frame"));
}

// UE DataDrivenShaderPlatformInfoSwitch(MaterialExpressions.cpp:9330–9393).
Result<void> LowerFormatFactSwitch(LoweringContext& context) noexcept {
    auto reader = context.Properties();
    if (!reader || reader.Value() == nullptr) {
        return reader ? Result<void>::Success() : Result<void>::Failure(reader.Error());
    }

    auto list = reader.Value()->OpenStructArray("ddspiPropertyNames",
                                                GeneratedType<FormatFactInput>::Schema());
    if (!list) {
        return context.Error(MaterialDiagnosticCode::InvalidProperty, StringView(),
                             "Format fact switch properties cannot be read");
    }

    constexpr StringView Invalid = "Format fact switch condition is invalid; the default material "
                                   "will be used until this is fixed";
    if (list.Value().Count() == 0) {
        return context.Error(MaterialDiagnosticCode::InvalidProperty, StringView(), Invalid);
    }

    bool result = true;
    bool any = false;
    for (usize index = 0; index < list.Value().Count(); ++index) {
        auto element = list.Value().Element(index);
        if (!element) {
            return Result<void>::Failure(element.Error());
        }

        auto name = element.Value().ReadText("inputName");
        auto condition = element.Value().ReadInteger("propertyCondition", 0);
        if (!name || !condition) {
            return Result<void>::Failure(!name ? name.Error() : condition.Error());
        }

        if (name.Value().IsEmpty() || name.Value() == StringView("None")) {
            continue;
        }

        bool fact = false;
        if (name.Value() == StringView("IsMobile")) {
            fact = context.Target(MaterialTargetField::IsMobile).isMobile;
        } else if (name.Value() == StringView("IsPC")) {
            fact = context.Target(MaterialTargetField::IsPc).isPc;
        } else if (name.Value() == StringView("IsConsole")) {
            fact = context.Target(MaterialTargetField::IsConsole).isConsole;
        } else {
            return context.Error(MaterialDiagnosticCode::InvalidProperty, StringView(), Invalid);
        }

        any = true;
        const bool wantsFalse = condition.Value() == static_cast<int64>(FormatFactCondition::False);
        result = result && (wantsFalse ? !fact : fact);
    }

    if (!any) {
        return context.Error(MaterialDiagnosticCode::InvalidProperty, StringView(),
                             "Format fact switch condition is empty; the default material will be "
                             "used until this is fixed");
    }

    return EmitSwitch(
        context, RequiredInput(context, result ? StringView("InputTrue") : StringView("InputFalse"),
                               "Format fact switch has no input for the chosen condition"));
}

// UE RequiredSamplersSwitch(MaterialExpressions.cpp:9784–9797).
Result<void> LowerSamplerBudgetSwitch(LoweringContext& context) noexcept {
    int64 required = 0;
    auto read = context.ReadInteger("requiredSamplers", 16, required);
    if (!read || !read.Value()) {
        return read ? Result<void>::Success() : Result<void>::Failure(read.Error());
    }

    const uint32 limit = context.Target(MaterialTargetField::MaxSamplers).maxSamplers;
    const bool within = required <= static_cast<int64>(limit);
    return EmitSwitch(
        context, RequiredInput(context, within ? StringView("InputTrue") : StringView("InputFalse"),
                               "Sampler budget switch has no input for the chosen case"));
}

// UE BindlessSwitch(MaterialExpressions.cpp:9510–9535).
Result<void> LowerBindlessFeatureSwitch(LoweringContext& context) noexcept {
    auto both = RequireBoth(context, "Default", "Missing input Default", "Bindless",
                            "Missing input Bindless");
    if (!both || !both.Value()) {
        return both ? Result<void>::Success() : Result<void>::Failure(both.Error());
    }

    const MaterialBindlessMode mode = context.Target(MaterialTargetField::Bindless).bindless;
    const bool bindless =
        mode == MaterialBindlessMode::All || mode == MaterialBindlessMode::Minimal;
    return EmitSwitch(context,
                      context.Input(bindless ? StringView("Bindless") : StringView("Default")));
}

// Yes·No 스위치(UE DistanceFieldsRenderingSwitch·VirtualTextureFeatureSwitch).
Result<void> LowerFeatureSwitch(LoweringContext& context, StringView name, bool enabled) noexcept {
    SystemAllocator scratch;
    String yes(scratch);
    String no(scratch);
    const StringView yesParts[] = {"Missing ", name, " input 'Yes'"};
    const StringView noParts[] = {"Missing ", name, " input 'No'"};
    for (StringView part : yesParts) {
        if (auto appended = yes.TryAppend(part); !appended) {
            return appended;
        }
    }

    for (StringView part : noParts) {
        if (auto appended = no.TryAppend(part); !appended) {
            return appended;
        }
    }

    auto both = RequireBoth(context, "Yes", yes.View(), "No", no.View());
    if (!both || !both.Value()) {
        return both ? Result<void>::Success() : Result<void>::Failure(both.Error());
    }

    return EmitSwitch(context, context.Input(enabled ? StringView("Yes") : StringView("No")));
}

Result<void> LowerDistanceFieldFeatureSwitch(LoweringContext& context) noexcept {
    return LowerFeatureSwitch(context, "DistanceFieldFeatureSwitch",
                              context.Target(MaterialTargetField::DistanceFields).distanceFields);
}

Result<void> LowerVirtualTextureFeatureSwitch(LoweringContext& context) noexcept {
    return LowerFeatureSwitch(
        context, "VirtualTextureFeatureSwitch",
        context.Target(MaterialTargetField::VirtualTexturing).virtualTexturing);
}

// UE MaterialProxyReplace(MaterialExpressions.cpp:17684–17692).
Result<void> LowerProxyBakePassSwitch(LoweringContext& context) noexcept {
    auto both = RequireBoth(context, "Realtime", "Missing MaterialProxyReplace input Realtime",
                            "MaterialProxy", "Missing MaterialProxyReplace input MaterialProxy");
    if (!both || !both.Value()) {
        return both ? Result<void>::Success() : Result<void>::Failure(both.Error());
    }

    const bool proxy = context.Target(MaterialTargetField::ProxyBake).proxyBake;
    return EmitSwitch(context,
                      context.Input(proxy ? StringView("MaterialProxy") : StringView("Realtime")));
}

// UE RuntimeVirtualTextureReplace(MaterialExpressions.cpp:3799–3807): RVT 출력 노드 입력 범위에서만
// VirtualTextureOutput이다. RVT 출력 노드(MS2d) 전까지는 늘 Default다.
Result<void> LowerRuntimeVirtualTexturePassSwitch(LoweringContext& context) noexcept {
    auto both =
        RequireBoth(context, "Default", "Missing RuntimeVirtualTextureReplace input 'Default'",
                    "VirtualTextureOutput",
                    "Missing RuntimeVirtualTextureReplace input 'VirtualTextureOutput'");
    if (!both || !both.Value()) {
        return both ? Result<void>::Success() : Result<void>::Failure(both.Error());
    }

    return EmitSwitch(context, context.Input("Default"));
}

// 두 입력을 모두 번역하는 실행 중 값 스위치(UE GenericSwitch)다.
Result<void> LowerRuntimeSwitch(LoweringContext& context, StringView condition,
                                StringView alternative, StringView alternativeMessage) noexcept {
    auto both =
        RequireBoth(context, "Default", "Missing input Default", alternative, alternativeMessage);
    if (!both || !both.Value()) {
        return both ? Result<void>::Success() : Result<void>::Failure(both.Error());
    }

    auto fallback = context.Input("Default");
    auto chosen = context.Input(alternative);
    if (!fallback || !chosen) {
        return Result<void>::Failure(!fallback ? fallback.Error() : chosen.Error());
    }

    return EmitSwitch(context, SwitchOn(context, condition, chosen.Value(), fallback.Value()));
}

// UE ShadowReplace·ReflectionCapturePassSwitch·MeshPaintTextureReplace
// (HLSLMaterialTranslator.cpp:11670–11688, 9142–9145).
Result<void> LowerShadowPassSwitch(LoweringContext& context) noexcept {
    return LowerRuntimeSwitch(context, "ShadowPass", "Shadow", "Missing input Shadow");
}

Result<void> LowerReflectionCaptureSwitch(LoweringContext& context) noexcept {
    return LowerRuntimeSwitch(context, "ReflectionCapturePass", "Reflection",
                              "Missing input Reflection");
}

Result<void> LowerMeshPaintTextureSwitch(LoweringContext& context) noexcept {
    return LowerRuntimeSwitch(context, "MeshPaintTextureValid", "MeshPaintTexture",
                              "Missing input MeshPaintTexture");
}

// UE NaniteReplace(MaterialExpressions.cpp:18637–18648): 지원하지 않는 대상은 Default만.
Result<void> LowerVirtualizedGeometryPassSwitch(LoweringContext& context) noexcept {
    auto both =
        RequireBoth(context, "Default", "Missing input Default", "Nanite", "Missing input Nanite");
    if (!both || !both.Value()) {
        return both ? Result<void>::Success() : Result<void>::Failure(both.Error());
    }

    if (!context.Target(MaterialTargetField::VirtualizedGeometry).virtualizedGeometry) {
        return EmitSwitch(context, context.Input("Default"));
    }

    return LowerRuntimeSwitch(context, "VirtualizedGeometryPass", "Nanite", "Missing input Nanite");
}

// UE LightmassReplace(MaterialExpressions.cpp:17570–17601).
Result<void> LowerLightBakePassSwitch(LoweringContext& context) noexcept {
    auto both = RequireBoth(context, "Realtime", "Missing LightmassReplace input Realtime",
                            "Lightmass", "Missing LightmassReplace input Lightmass");
    if (!both || !both.Value()) {
        return both ? Result<void>::Success() : Result<void>::Failure(both.Error());
    }

    auto baked = context.Input("Lightmass");
    if (!baked) {
        return Result<void>::Failure(baked.Error());
    }

    if (context.Target(MaterialTargetField::LightBake).lightBake) {
        return context.Output("Result", baked.Value());
    }

    auto realtime = context.Input("Realtime");
    if (!realtime) {
        return Result<void>::Failure(realtime.Error());
    }

    if (baked.Value() == NoIrValue || realtime.Value() == NoIrValue) {
        // 값 없는 쪽이 있으면 남은 쪽이다.
        return context.Output("Result",
                              baked.Value() == NoIrValue ? realtime.Value() : baked.Value());
    }

    return EmitSwitch(context, SwitchOn(context, "LightBakePass", baked.Value(), realtime.Value()));
}

// UE GIReplace(HLSLMaterialTranslator.cpp:11665–11668): StaticIndirect는 번역하지만 쓰지 않는다.
Result<void> LowerGlobalIlluminationPassSwitch(LoweringContext& context) noexcept {
    auto fallback = RequiredInput(context, "Default", "Missing GIReplace input 'Default'");
    if (!fallback || fallback.Value() == NoIrValue) {
        return fallback ? Result<void>::Success() : Result<void>::Failure(fallback.Error());
    }

    auto unused = InputOr(context, "StaticIndirect", fallback.Value());
    auto dynamic = InputOr(context, "DynamicIndirect", fallback.Value());
    if (!unused || !dynamic) {
        return Result<void>::Failure(!unused ? unused.Error() : dynamic.Error());
    }

    return EmitSwitch(
        context, SwitchOn(context, "GlobalIlluminationPass", dynamic.Value(), fallback.Value()));
}

// UE RayTracingQualitySwitch(MaterialExpressions.cpp:17749–17775): 지원하지 않는 대상은 Normal만.
Result<void> LowerRayTracingQualitySwitch(LoweringContext& context) noexcept {
    auto both = RequireBoth(context, "Normal", "Missing RayTracingQualitySwitch input 'Normal'",
                            "RayTraced", "Missing RayTracingQualitySwitch input 'RayTraced'");
    if (!both || !both.Value()) {
        return both ? Result<void>::Success() : Result<void>::Failure(both.Error());
    }

    auto normal = context.Input("Normal");
    if (!normal) {
        return Result<void>::Failure(normal.Error());
    }

    if (!context.Target(MaterialTargetField::RayTracing).rayTracing) {
        return context.Output("Result", normal.Value());
    }

    auto traced = context.Input("RayTraced");
    if (!traced) {
        return Result<void>::Failure(traced.Error());
    }

    return EmitSwitch(context, SwitchOn(context, "RayTracingPass", traced.Value(), normal.Value()));
}

// UE PathTracingQualitySwitch(MaterialExpressions.cpp:17812–17838).
Result<void> LowerPathTracingQualitySwitch(LoweringContext& context) noexcept {
    auto both = RequireBoth(context, "Normal", "Missing PathTracingQualitySwitch input 'Normal'",
                            "PathTraced", "Missing PathTracingQualitySwitch input 'PathTraced'");
    if (!both || !both.Value()) {
        return both ? Result<void>::Success() : Result<void>::Failure(both.Error());
    }

    auto normal = context.Input("Normal");
    auto traced = context.Input("PathTraced");
    if (!normal || !traced) {
        return Result<void>::Failure(!normal ? normal.Error() : traced.Error());
    }

    return EmitSwitch(context,
                      SwitchOn(context, "PathTracingPass", traced.Value(), normal.Value()));
}

// UE PathTracingRayTypeSwitch(HLSLMaterialTranslator.cpp:11700–11709): Main에서 시작해 광선
// 종류마다 겹친다(뒤가 이긴다).
Result<void> LowerPathTracingRayTypeSwitch(LoweringContext& context) noexcept {
    struct Ray {
        StringView key;
        StringView condition;
    };

    constexpr Ray Rays[] = {{"Shadow", "PathTracingShadowRay"},
                            {"IndirectDiffuse", "PathTracingIndirectDiffuseRay"},
                            {"IndirectSpecular", "PathTracingIndirectSpecularRay"},
                            {"IndirectVolume", "PathTracingIndirectVolumeRay"}};
    auto main = RequiredInput(context, "Main", "Missing PathTracingRayTypeSwitch input 'Main'");
    if (!main || main.Value() == NoIrValue) {
        return main ? Result<void>::Success() : Result<void>::Failure(main.Error());
    }

    IrValueId result = main.Value();
    for (const Ray& ray : Rays) {
        auto value = InputOr(context, ray.key, main.Value());
        auto chained =
            value ? SwitchOn(context, ray.condition, value.Value(), result) : Move(value);
        if (!chained) {
            return Result<void>::Failure(chained.Error());
        }

        result = chained.Value();
    }

    return context.Output("Result", result);
}

// 늦은 입력 노드의 계획 입력이다(조건만 먼저 — 6.9절 MS2b3).
constexpr StringView StaticSwitchEager[] = {"Value"};

constexpr LoweringEntry SwitchEntries[] = {
    {"Material.StaticSwitch", &LowerStaticSwitch, true, StaticSwitchEager},
    {"Material.QualitySwitch", &LowerQualitySwitch, true, {}},
    {"Material.RenderPathSwitch", &LowerRenderPathSwitch, true, {}},
    {"Material.CapabilityTierSwitch", &LowerCapabilityTierSwitch, true, {}},
    {"Material.StageSwitch", &LowerStageSwitch, true, {}},
    {"Material.PreviousFrameSwitch", &LowerPreviousFrameSwitch, true, {}},
    {"Material.FormatFactSwitch", &LowerFormatFactSwitch, true, {}},
    {"Material.SamplerBudgetSwitch", &LowerSamplerBudgetSwitch, true, {}},
    {"Material.BindlessFeatureSwitch", &LowerBindlessFeatureSwitch, true, {}},
    {"Material.DistanceFieldFeatureSwitch", &LowerDistanceFieldFeatureSwitch, true, {}},
    {"Material.VirtualTextureFeatureSwitch", &LowerVirtualTextureFeatureSwitch, true, {}},
    {"Material.ProxyBakePassSwitch", &LowerProxyBakePassSwitch, true, {}},
    {"Material.RuntimeVirtualTexturePassSwitch", &LowerRuntimeVirtualTexturePassSwitch, true, {}},
    {"Material.VirtualizedGeometryPassSwitch", &LowerVirtualizedGeometryPassSwitch, true, {}},
    {"Material.LightBakePassSwitch", &LowerLightBakePassSwitch, true, {}},
    {"Material.RayTracingQualitySwitch", &LowerRayTracingQualitySwitch, true, {}},
    {"Material.ShadowPassSwitch", &LowerShadowPassSwitch},
    {"Material.ReflectionCaptureSwitch", &LowerReflectionCaptureSwitch},
    {"Material.MeshPaintTextureSwitch", &LowerMeshPaintTextureSwitch},
    {"Material.GlobalIlluminationPassSwitch", &LowerGlobalIlluminationPassSwitch},
    {"Material.PathTracingQualitySwitch", &LowerPathTracingQualitySwitch},
    {"Material.PathTracingRayTypeSwitch", &LowerPathTracingRayTypeSwitch},
};

} // namespace

Span<const LoweringEntry> SwitchLowerings() noexcept {
    return SwitchEntries;
}

} // namespace rocks::material::detail
